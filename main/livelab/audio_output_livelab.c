// LiveLab implementation of the upstream audio_output.h interface: AirPlay / Bluetooth both go through output.c,
// sharing one output task with DLNA (optical S/PDIF 24 bit / 3.5mm DAC, switchable at runtime).
#include "audio_output.h"
#include <string.h>
#include "esp_log.h"
#include "output.h"
#include "livelab.h"

#define TAG "audio_out"
#define BT_MAX_PENDING 2048   // max frames of Bluetooth PCM backlog (~46 ms); beyond that the Bluetooth write task blocks

// Per-source sample rate: while another source holds the output, just remember it; switch when the output is acquired (must not interrupt DLNA that is playing)
static volatile int s_airplay_rate = 44100, s_bt_rate = 44100;

void livelab_output_take(out_src_t src)
{
    output_set_source(src);
    if (src == OUT_SRC_AIRPLAY) output_set_format(s_airplay_rate, 16);
    else if (src == OUT_SRC_BT) output_set_format(s_bt_rate, 16);
}

esp_err_t audio_output_init(void)
{
    output_init();
    return ESP_OK;
}

// AirPlay service started: when no other source is using it, the output task pulls data from AirPlay on demand
void audio_output_start(void)
{
    if (output_source() == OUT_SRC_NONE) output_set_source(OUT_SRC_AIRPLAY);
}

void audio_output_stop(void)
{
    if (output_source() == OUT_SRC_AIRPLAY) output_set_source(OUT_SRC_NONE);
}

void audio_output_flush(void)
{
    if (output_source() == OUT_SRC_BT) output_flush();
}

void audio_output_set_source_rate(int rate)
{
    if (rate <= 0) return;
    s_airplay_rate = rate;
    if (output_source() == OUT_SRC_AIRPLAY) output_set_format(rate, 16);
}

void audio_output_set_sample_rate(uint32_t rate)
{
    s_bt_rate = (int)rate;
    if (output_source() == OUT_SRC_BT) output_set_format((int)rate, 16);
}

uint32_t audio_output_get_hardware_latency_us(void)
{
    // In steady state the DMA ring sits between (N-1) and N descriptors; take the midpoint
    uint64_t frames = output_dma_frames() - output_dma_frames() / 12;
    return (uint32_t)(frames * 1000000ULL / output_rate());
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us)
{
    uint32_t frames;
    output_pipeline(now_us, &frames);
    if (pipeline_us) *pipeline_us = (uint32_t)((uint64_t)frames * 1000000ULL / output_rate());
    return true;
}

// Bluetooth: 16-bit interleaved stereo, may not be whole frames; a half frame is kept for next time
#define PCM_FRAME_BYTES 4
static uint8_t s_carry[PCM_FRAME_BYTES];
static size_t s_carry_len;

static esp_err_t write16(const void *data, size_t bytes, int32_t volume_q15)
{
    static int32_t buf[256 * 2];
    // Arbitration hands the output to Bluetooth when it starts playing; if it didn't get the output (pre-empted by another source), drop the data
    if (output_source() != OUT_SRC_BT) {
        s_carry_len = 0;
        return ESP_OK;
    }
    int32_t g = volume_q15 >= 0 && output_volume_applies() ? volume_q15 * 2 : 65536;
    const uint8_t *src = data;
    while (bytes > 0) {
        int16_t frame[2];
        size_t n = 0;
        while (bytes > 0 && n < 256) {
            size_t take = PCM_FRAME_BYTES - s_carry_len;
            if (take > bytes) take = bytes;
            memcpy(s_carry + s_carry_len, src, take);
            s_carry_len += take;
            src += take;
            bytes -= take;
            if (s_carry_len < PCM_FRAME_BYTES) break;
            memcpy(frame, s_carry, PCM_FRAME_BYTES);
            s_carry_len = 0;
            for (int c = 0; c < 2; c++) {
                int32_t v = (int32_t)frame[c] << 8;
                buf[2 * n + c] = g >= 65536 ? v : (int32_t)(((int64_t)v * g) >> 16);
            }
            n++;
        }
        if (n) output_write(buf, (int)n, NULL, BT_MAX_PENDING);
    }
    return ESP_OK;
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait)
{
    (void)wait;
    return write16(data, bytes, -1);
}

esp_err_t audio_output_write_pcm(const void *data, size_t bytes, int32_t volume_q15, TickType_t wait)
{
    (void)wait;
    return write16(data, bytes, volume_q15);
}
