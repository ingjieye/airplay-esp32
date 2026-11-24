#include "output.h"
#include <math.h>
#include <string.h>
#include "audio_output.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rtsp_server.h"
#include "spdif_enc.h"

#define TAG "output"
#define SPDIF_GPIO GPIO_NUM_14
// I2S pins of the DAC (3.5mm analog output), from the seller; ESP32 can only output MCLK on IO0/1/3
#define DAC_BCK GPIO_NUM_33
#define DAC_LRCK GPIO_NUM_32
#define DAC_SDO GPIO_NUM_25
#define DAC_MCLK GPIO_NUM_0
#define BLOCK 192                         // frames processed per step (= one channel-status block = one DMA descriptor)
#define DMA_DESC 6                        // 6 blocks ≈ 26 ms (44.1k)
#define FRAME_BYTES 8                     // stereo 2 x int32
#define PCM_FRAMES (32 * 1024)            // PCM buffer of 32K frames (~0.74 s at 44.1k), in PSRAM
#define PREBUF_FRAMES 4096                // after a flush, collect this many before playing to avoid stutter at the start (~93 ms at 44.1k)
#define MAX_RATE 96000                    // limit of the DLT1150A optical module
#define RELOCK_MS 300                     // after a sample-rate change, output silence first so the speaker can relock instead of swallowing the start of the track
// Sleep at least 1 tick: at a low tick rate pdMS_TO_TICKS turns a few ms into 0, and vTaskDelay(0) is a busy wait
#define SLEEP_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

static const output_desc_t OUTPUTS[] = {
    {"spdif", "Optical S/PDIF", true, ""},
    {"analog", "3.5mm analog", true, ""},
};
#define NOUT (int)(sizeof OUTPUTS / sizeof OUTPUTS[0])
#define ANALOG (s_cur == 1)

static int s_cur;                          // current output index
static volatile int s_out_req = -1;        // output switch request (handled by the output task)
static volatile out_src_t s_src = OUT_SRC_NONE;
static volatile int s_volume = 100;
static volatile bool s_mute, s_fixed_optical = true;
static volatile int32_t s_gain = 65536;    // gain derived from volume (Q16)
static spdif_enc_t s_enc;
static volatile int s_rate = 44100, s_bits = 16;
// Flush and format changes are both executed by the output task itself between two blocks (no lock: a high-priority task looping release/re-take starves tasks waiting for the lock)
static volatile bool s_flush_req;
static volatile int s_fmt_req_rate, s_fmt_req_bits;  // non-zero rate means a switch is requested
static volatile esp_err_t s_fmt_result;
static StreamBufferHandle_t s_pcm;
static StaticStreamBuffer_t s_pcm_struct;
static volatile bool s_paused, s_active, s_prebuf = true;
static volatile uint64_t s_played;
static volatile uint32_t s_underruns;
static volatile int64_t s_first_audio_us;
static int64_t s_hold_until_us;          // output only silence until this time (speaker relocking)
// DMA cursor for AirPlay sync: frames handed to I2S, frames sent out by DMA, time of the latest send
static volatile uint64_t s_submitted, s_sent;
static volatile int64_t s_sent_us;

static void update_gain(void)
{
    int v = s_volume;
    s_gain = s_mute || v <= 0 ? 0 : v >= 100 ? 65536 : (int32_t)(65536 * powf(10, (v - 100) * 0.5f / 20));
}

#if CONFIG_LIVELAB_SIM
// ---------- Simulator: stream goes to the host over TCP (the host is 10.0.2.2 in QEMU user networking), paced in real time ----------
// Optical S/PDIF stream goes to port 9000, analog output (raw 32-bit stereo I2S) to 9001
#include "lwip/sockets.h"
static int s_sink = -1;
static int64_t s_next_us, s_last_try_us;

static void hw_start(void) { s_next_us = 0; }
static void hw_stop(void)
{
    if (s_sink >= 0) close(s_sink);
    s_sink = -1;
    s_last_try_us = 0;
}
static esp_err_t hw_set_rate(int rate) { return ESP_OK; }

static void hw_write(const void *buf, size_t len)
{
    int64_t now = esp_timer_get_time();
    s_next_us = s_next_us ? s_next_us + BLOCK * 1000000LL / s_rate : now;
    int port = CONFIG_LIVELAB_SIM_SINK_PORT + (ANALOG ? 1 : 0);
    if (s_next_us - now > 10000) SLEEP_MS((s_next_us - now) / 1000);
    else if (now - s_next_us > 200000) s_next_us = now;  // too far behind (slow simulator): don't catch up
    if (s_sink < 0 && now - s_last_try_us > 1000000) {
        s_last_try_us = now;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
                                .sin_addr.s_addr = inet_addr("10.0.2.2")};
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            s_sink = fd;
            ESP_LOGI(TAG, "Connected to host audio sink 10.0.2.2:%d", port);
        } else {
            close(fd);
        }
    }
    if (s_sink >= 0 && send(s_sink, buf, len, 0) != (int)len) {
        close(s_sink);
        s_sink = -1;
    }
    s_submitted += BLOCK;
    s_sent = s_submitted;  // sent at real-time pace, so it can be treated as played immediately
    s_sent_us = esp_timer_get_time();
}
#else
// ---------- Hardware: I2S0 drives the optical module (IO14) or the DAC, only one at a time ----------
static i2s_chan_handle_t s_tx;
static i2s_std_clk_config_t clk_cfg(int rate)
{
    i2s_std_clk_config_t c = I2S_STD_CLK_DEFAULT_CONFIG(ANALOG ? rate : rate * 2);  // optical: each S/PDIF subframe = one 64-bit I2S frame
    c.clk_src = I2S_CLK_SRC_APLL;
    if (ANALOG)
        c.mclk_multiple = I2S_MCLK_MULTIPLE_128;  // with APLL, IO0 outputs the APLL itself (= 2 x internal MCLK), i.e. the 256fs DACs usually want
    else
        c.mclk_multiple = rate > 48000 ? I2S_MCLK_MULTIPLE_128 : I2S_MCLK_MULTIPLE_256;
    return c;
}

static bool IRAM_ATTR on_sent(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    s_sent += BLOCK;  // each descriptor is exactly one block
    s_sent_us = esp_timer_get_time();
    return false;
}

static void hw_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESC;
    cc.dma_frame_num = ANALOG ? BLOCK : BLOCK * 2;  // one descriptor is exactly one block: when an underrun replays old descriptors the B preamble stays in the right place
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
    i2s_std_config_t sc = {
        .clk_cfg = clk_cfg(s_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = I2S_GPIO_UNUSED, .ws = I2S_GPIO_UNUSED,
                     .dout = SPDIF_GPIO, .din = I2S_GPIO_UNUSED},
    };
    if (ANALOG)
        sc.gpio_cfg = (i2s_std_gpio_config_t){.mclk = DAC_MCLK, .bclk = DAC_BCK, .ws = DAC_LRCK,
                                              .dout = DAC_SDO, .din = I2S_GPIO_UNUSED};
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    i2s_event_callbacks_t cb = {.on_sent = on_sent};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_tx, &cb, NULL));
    s_submitted = s_sent = 0;
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

static void hw_stop(void)
{
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    // Deleting the channel doesn't disconnect the GPIO matrix: without a reset the old pin stays wired to the I2S signal and the DAC receives the optical bitstream (noise)
    const gpio_num_t pins[] = {SPDIF_GPIO, DAC_BCK, DAC_LRCK, DAC_SDO, DAC_MCLK};
    for (int i = 0; i < 5; i++) gpio_reset_pin(pins[i]);
}

static esp_err_t hw_set_rate(int rate)
{
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t c = clk_cfg(rate);
    esp_err_t e = i2s_channel_reconfig_std_clock(s_tx, &c);
    s_submitted = s_sent = 0;
    i2s_channel_enable(s_tx);
    return e;
}

static void hw_write(const void *buf, size_t len)
{
    size_t w;
    i2s_channel_write(s_tx, buf, len, &w, portMAX_DELAY);
    s_submitted += BLOCK;
}
#endif

// Pull one block from the AirPlay timeline (16 bit), padding the shortfall with silence. AirPlay sync relies on this "pull only when needed"
static int pull_airplay(int32_t *pcm)
{
    static int16_t tmp[BLOCK * 2];
    int n = 0;
    while (n < BLOCK) {
        size_t k = audio_output_read_source(tmp, BLOCK - n);
        if (k == 0) break;
        int32_t g = output_volume_applies() ? (int32_t)airplay_get_volume_q15() * 2 : 65536;  // q15 -> q16
        for (size_t i = 0; i < k * 2; i++) {
            int32_t v = (int32_t)tmp[i] << 8;
            pcm[2 * n + i] = g >= 65536 ? v : (int32_t)(((int64_t)v * g) >> 16);
        }
        n += k;
    }
    return n;
}

static void output_task(void *arg)
{
    static int32_t pcm[BLOCK * 2];
    static uint32_t enc[BLOCK * 4];
    while (1) {
        if (s_out_req >= 0) {
            hw_stop();
            s_cur = s_out_req;
            hw_start();
            s_hold_until_us = esp_timer_get_time() + RELOCK_MS * 1000;  // the optical receiver has to relock
            ESP_LOGI(TAG, "Output switched to %s", OUTPUTS[s_cur].name);
            s_out_req = -1;
        }
        if (s_flush_req) {
            xStreamBufferReset(s_pcm);  // the writer is waiting for the flush to complete, so it isn't writing concurrently
            s_played = 0;
            s_prebuf = true;
            s_first_audio_us = 0;
            s_flush_req = false;
        }
        if (s_fmt_req_rate) {
            int rate = s_fmt_req_rate, bits = s_fmt_req_bits;
            esp_err_t e = ESP_OK;
            if (rate == s_rate) {
                // Only the bit depth changed: just update the channel-status bits; clock unchanged, no signal interruption, the speaker doesn't need to relock
                s_bits = bits;
                spdif_enc_init(&s_enc, rate, bits);
                ESP_LOGI(TAG, "Channel status updated to %d bit (same sample rate, I2S not restarted)", bits);
            } else if ((e = hw_set_rate(rate)) == ESP_OK) {
                s_rate = rate;
                s_bits = bits;
                spdif_enc_init(&s_enc, rate, bits);
                s_hold_until_us = esp_timer_get_time() + RELOCK_MS * 1000;
                ESP_LOGI(TAG, "Output format switched to %d Hz / %d bit (I2S restarted, %d ms of silence for the speaker to lock)", rate, bits, RELOCK_MS);
            } else {
                ESP_LOGE(TAG, "Failed to switch to %d Hz: %s", rate, esp_err_to_name(e));
            }
            s_fmt_result = e;
            s_fmt_req_rate = 0;
        }
        int n = 0;
        bool hold = esp_timer_get_time() < s_hold_until_us;
        out_src_t src = s_src;
        if (src == OUT_SRC_AIRPLAY) {
            if (!hold) n = pull_airplay(pcm);
        } else if (src != OUT_SRC_NONE) {
            size_t avail = xStreamBufferBytesAvailable(s_pcm) / FRAME_BYTES;
            if (s_prebuf && (avail >= PREBUF_FRAMES || (!s_active && avail > 0))) s_prebuf = false;
            if (!s_paused && !s_prebuf && !hold) {
                // The writer only writes whole frames, so what arrives here is always whole frames
                n = xStreamBufferReceive(s_pcm, pcm, BLOCK * FRAME_BYTES, 0) / FRAME_BYTES;
                if (n < BLOCK && s_active && s_played > 0) s_underruns++;
            }
        }
        if (n > 0 && s_first_audio_us == 0) s_first_audio_us = esp_timer_get_time();
        if (n < BLOCK) memset(pcm + n * 2, 0, (BLOCK - n) * FRAME_BYTES);
        s_played += n;
        if (ANALOG) {
            // 32-bit I2S slots, 24-bit samples in the high bits. AirPlay already applied its own volume when pulled
            int32_t g = src == OUT_SRC_DLNA && output_volume_applies() ? s_gain : 65536;
            for (int i = 0; i < BLOCK * 2; i++)
                pcm[i] = (g == 65536 ? pcm[i] : (int32_t)(((int64_t)pcm[i] * g) >> 16)) << 8;
            hw_write(pcm, sizeof pcm);
        } else {
            if (src == OUT_SRC_DLNA && output_volume_applies() && s_gain != 65536)
                for (int i = 0; i < BLOCK * 2; i++) pcm[i] = (int32_t)(((int64_t)pcm[i] * s_gain) >> 16);
            spdif_enc_frames(&s_enc, pcm, BLOCK, enc);
            hw_write(enc, sizeof enc);
        }
    }
}

void output_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    nvs_handle_t h;
    if (nvs_open("player", NVS_READONLY, &h) == ESP_OK) {
        char id[16];
        size_t l = sizeof id;
        if (nvs_get_str(h, "output", id, &l) == ESP_OK)
            for (int i = 0; i < NOUT; i++)
                if (!strcmp(id, OUTPUTS[i].id) && OUTPUTS[i].available) s_cur = i;
        uint8_t fixed;
        if (nvs_get_u8(h, "fixed_opt", &fixed) == ESP_OK) s_fixed_optical = fixed;
        nvs_close(h);
    }
    uint8_t *store = heap_caps_malloc(PCM_FRAMES * FRAME_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!store) store = heap_caps_malloc(PCM_FRAMES / 8 * FRAME_BYTES + 1, MALLOC_CAP_8BIT);  // fallback without PSRAM
    s_pcm = xStreamBufferCreateStatic(store ? PCM_FRAMES * FRAME_BYTES : 0, 1, store, &s_pcm_struct);
    spdif_enc_init(&s_enc, s_rate, s_bits);
    hw_start();
    xTaskCreatePinnedToCore(output_task, "output", 4096, NULL, AUDIO_PLAYBACK_TASK_PRIORITY,
                            NULL, CONFIG_FREERTOS_NUMBER_OF_CORES - 1);
    ESP_LOGI(TAG, "Output: %s", OUTPUTS[s_cur].name);
}

const char *output_source_name(out_src_t src)
{
    static const char *names[] = {"none", "dlna", "airplay", "bluetooth"};
    return names[src];
}

void output_set_source(out_src_t src)
{
    if (src == s_src) return;
    ESP_LOGI(TAG, "Source: %s -> %s", output_source_name(s_src), output_source_name(src));
    s_src = src;
    s_paused = false;
    s_active = false;
    output_flush();
}

out_src_t output_source(void) { return s_src; }
int output_rate(void) { return s_rate; }
int output_bits(void) { return s_bits; }

bool output_set_format(int rate, int bits)
{
    if (rate > MAX_RATE) return false;
    if (rate == s_rate && bits == s_bits) return true;
    // Wait for data in the old format to finish playing (buffered data belongs to push-type sources; discarded when paused)
    for (int i = 0; i < 400 && xStreamBufferBytesAvailable(s_pcm) > 0 && !s_paused; i++) SLEEP_MS(5);
    s_fmt_req_bits = bits;
    s_fmt_req_rate = rate;  // write rate last: the output task only acts once rate is non-zero
    while (s_fmt_req_rate) SLEEP_MS(1);
    return s_fmt_result == ESP_OK;
}

int output_write(const int32_t *frames, int n, volatile bool *abort, int max_pending)
{
    const uint8_t *p = (const uint8_t *)frames;
    size_t left = (size_t)n * FRAME_BYTES;
    size_t cap = max_pending > 0 ? (size_t)max_pending * FRAME_BYTES : PCM_FRAMES * FRAME_BYTES;
    while (left && !(abort && *abort)) {
        // Whole frames only: there is a single writer and free space can only grow, so the write below always succeeds completely
        size_t space = xStreamBufferSpacesAvailable(s_pcm) / FRAME_BYTES * FRAME_BYTES;
        size_t used = xStreamBufferBytesAvailable(s_pcm);
        size_t room = used >= cap ? 0 : cap - used;
        if (room < space) space = room / FRAME_BYTES * FRAME_BYTES;
        if (space == 0) { SLEEP_MS(5); continue; }
        size_t w = xStreamBufferSend(s_pcm, p, space < left ? space : left, 0);
        p += w;
        left -= w;
    }
    return n - left / FRAME_BYTES;
}

void output_flush(void)
{
    s_flush_req = true;
    while (s_flush_req) SLEEP_MS(1);  // the output task checks once per block (~4 ms)
}

bool output_pipeline(int64_t *now_us, uint32_t *frames)
{
    uint64_t sub = s_submitted, sent = s_sent;
    int64_t sent_us = s_sent_us, now = esp_timer_get_time();
    uint32_t q = sub > sent ? (uint32_t)(sub - sent) : 0;
    // The TX-done interrupt only fires once per descriptor; in between, subtract what has been played based on elapsed time
    if (sent_us > 0 && now > sent_us) {
        uint64_t drained = (uint64_t)(now - sent_us) * s_rate / 1000000;
        if (drained > BLOCK) drained = BLOCK;
        q = drained < q ? q - (uint32_t)drained : 0;
    }
    if (now_us) *now_us = now;
    if (frames) *frames = q;
    return true;
}

uint32_t output_dma_frames(void) { return DMA_DESC * BLOCK; }
void output_pause(bool pause) { s_paused = pause; }
void output_set_active(bool active) { s_active = active; }
uint64_t output_played_frames(void) { return s_played; }
int output_pending_frames(void) { return xStreamBufferBytesAvailable(s_pcm) / FRAME_BYTES; }
uint32_t output_underruns(void) { return s_underruns; }
int64_t output_first_audio_us(void) { return s_first_audio_us; }
int output_count(void) { return NOUT; }
const output_desc_t *output_get(int i) { return i >= 0 && i < NOUT ? &OUTPUTS[i] : NULL; }
int output_current(void) { return s_cur; }

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("player", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t output_select(const char *id)
{
    for (int i = 0; i < NOUT; i++) {
        if (strcmp(id, OUTPUTS[i].id)) continue;
        if (!OUTPUTS[i].available) return ESP_ERR_NOT_SUPPORTED;
        if (i != s_cur) {
            s_out_req = i;
            while (s_out_req >= 0) SLEEP_MS(1);
        }
        nvs_handle_t h;
        if (nvs_open("player", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "output", id);
            nvs_commit(h);
            nvs_close(h);
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void output_set_volume(int volume, bool mute)
{
    s_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    s_mute = mute;
    update_gain();
}

int output_volume(void) { return s_volume; }
bool output_muted(void) { return s_mute; }
void output_set_fixed_optical(bool fixed)
{
    s_fixed_optical = fixed;
    save_u8("fixed_opt", fixed);
}
bool output_fixed_optical(void) { return s_fixed_optical; }
bool output_volume_applies(void) { return ANALOG || !s_fixed_optical; }
int32_t output_gain_q16(void) { return output_volume_applies() ? s_gain : 65536; }
