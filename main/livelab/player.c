#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "netstream.h"
#include "m4a.h"
#include "output.h"
#include "livelab.h"

#define TAG "player"
#define NET_BUF (1536 * 1024)   // compressed-data buffer in PSRAM (~12 s of FLAC; AirPlay sessions need PSRAM too, so no bigger)
#define CHUNK 1024              // frames per decode call
#define HEAD_CACHE (64 * 1024)  // file-head cache: metadata (incl. seek table) is almost always in here
#define HIST_WINDOW (32 * 1024) // read-back window over recently downloaded data: dr_flac steps back a few KB when seeking

// ---------- Shared state ----------
static SemaphoreHandle_t s_mx;             // protects the strings and metadata below
static char *s_uri;
static char s_title[128], s_artist[128], s_album[128], s_art[300], s_host[64], s_error[96];
static char s_codec[8];
static volatile int s_rate, s_bits, s_ch;
static volatile int64_t s_total_frames;    // 0 = unknown
static volatile int64_t s_base_frame;      // start frame at flush (position after a seek)
static volatile const char *s_state = "NO_MEDIA_PRESENT";
static volatile double s_resume_sec;       // playback position before reboot: next Play starts here
static volatile bool s_restored;           // "paused" restored after reboot: the decoder isn't actually running, Play must start from scratch
static volatile int64_t s_play_us;         // time Play was received, for measuring start-up latency
static volatile int s_start_ms = -1;
static volatile int64_t s_hdr_us, s_ready_us;  // start-up phases: time response headers arrived / file header parsed
static volatile int64_t s_ready_bytes;          // bytes read while parsing the file header
static volatile int64_t s_dec_busy_us;    // accumulated time spent decoding
// MP3 duration estimate: uses the decoder's own "decoded frames / bytes read" ratio (playback lags decoding, don't mix them)
static volatile int64_t s_dec_frame;      // decoder's current absolute frame position
static volatile int64_t s_est_f0, s_est_c0; // estimation baseline (start of decoding or after a seek)

// ---------- HTTP download task ----------
static StreamBufferHandle_t s_net;
static StaticStreamBuffer_t s_net_struct;
static SemaphoreHandle_t s_http_start, s_http_idle;  // idle can be taken = download task is idle
static volatile int64_t s_http_off;
static volatile bool s_http_abort, s_http_eof;
static volatile int64_t s_size = -1, s_downloaded;
static char s_ctype[48];
static EventGroupHandle_t s_ev;
#define EV_HDR BIT0      // response headers received (file size, type)
#define EV_DEC_IDLE BIT1 // decode task idle

static esp_err_t http_ev(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    if (!strcasecmp(e->header_key, "Content-Range")) {
        const char *sl = strchr(e->header_value, '/');
        if (sl && sl[1] != '*') s_size = strtoll(sl + 1, NULL, 10);
    } else if (!strcasecmp(e->header_key, "Content-Type")) {
        strlcpy(s_ctype, e->header_value, sizeof s_ctype);
    }
    return ESP_OK;
}

static void http_task(void *arg)
{
    static uint8_t buf[4096];
    while (1) {
        xSemaphoreTake(s_http_start, portMAX_DELAY);
        int64_t off = s_http_off;
        int fails = 0;
        while (!s_http_abort) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            esp_http_client_config_t c = {.url = s_uri, .event_handler = http_ev, .timeout_ms = 10000, .buffer_size = 4096};
            esp_http_client_handle_t cl = esp_http_client_init(&c);
            xSemaphoreGive(s_mx);
            char range[48];
            snprintf(range, sizeof range, "bytes=%lld-", off);
            esp_http_client_set_header(cl, "Range", range);
            int status = 0;
            int64_t clen = -1;
            if (esp_http_client_open(cl, 0) == ESP_OK) {
                clen = esp_http_client_fetch_headers(cl);
                status = esp_http_client_get_status_code(cl);
            }
            if (status == 200 && off == 0 && s_size < 0) s_size = clen;  // server without Range support
            if (status == 206 || (status == 200 && off == 0)) {
                xEventGroupSetBits(s_ev, EV_HDR);
                while (!s_http_abort) {
                    int r = esp_http_client_read(cl, (char *)buf, sizeof buf);
                    if (r <= 0) break;
                    for (int w = 0; w < r && !s_http_abort;)
                        w += xStreamBufferSend(s_net, buf + w, r - w, pdMS_TO_TICKS(100));
                    off += r;
                    s_downloaded += r;
                    fails = 0;
                }
            } else {
                ESP_LOGW(TAG, "HTTP %d (Range %s)", status, range);
            }
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
            if (s_http_abort || (s_size >= 0 && off >= s_size)) break;
            if (status == 403 || status == 404 || status == 410 || ++fails > 5) {
                xSemaphoreTake(s_mx, portMAX_DELAY);
                snprintf(s_error, sizeof s_error, "Download failed: HTTP %d", status);
                xSemaphoreGive(s_mx);
                xEventGroupSetBits(s_ev, EV_HDR);
                break;
            }
            ESP_LOGW(TAG, "Connection lost, reconnecting at %lld (attempt %d)", off, fails);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        s_http_eof = true;
        xSemaphoreGive(s_http_idle);
    }
}

// Stop the download task and wait until it is idle (only called from the decode task)
static void http_stop(void)
{
    s_http_abort = true;
    xSemaphoreTake(s_http_idle, portMAX_DELAY);
    xSemaphoreGive(s_http_idle);
    s_http_abort = false;
}

static void http_restart(void *ctx, int64_t off)
{
    http_stop();
    xSemaphoreTake(s_http_idle, portMAX_DELAY);  // mark as busy
    xStreamBufferReset(s_net);
    s_http_eof = false;
    s_http_off = off;
    xSemaphoreGive(s_http_start);
}

// ---------- Decode task ----------
static volatile bool s_dec_abort;
static volatile bool s_dec_break;  // make the decoder return to its main loop ASAP (stop or seek): interrupts a write waiting for buffer space
static volatile int64_t s_seek_us; // start time of the latest seek, for measuring seek-to-audio latency
static volatile int64_t s_seek_frame = -1;
static netstream_t s_ns;

static volatile bool s_dec_ready;  // decoder initialised successfully (reading too much before that means the format is wrong)

static size_t net_recv(void *ctx, uint8_t *buf, size_t n)
{
    if (!s_dec_ready && s_ns.consumed > 2 * 1024 * 1024) return 0;  // 2 MB read and still no format: treat as EOF so init fails
    while (!s_dec_abort) {
        size_t r = xStreamBufferReceive(s_net, buf, n, pdMS_TO_TICKS(50));
        if (r) return r;
        if (s_http_eof && xStreamBufferBytesAvailable(s_net) == 0) return 0;
    }
    return 0;
}

static size_t rd_cb(void *u, void *b, size_t n) { return ns_read(&s_ns, b, n); }
static drflac_bool32 flac_seek_cb(void *u, int o, drflac_seek_origin g)
{
    return ns_seek(&s_ns, o, g == DRFLAC_SEEK_SET ? NS_SEEK_SET : g == DRFLAC_SEEK_CUR ? NS_SEEK_CUR : NS_SEEK_END);
}
static drflac_bool32 flac_tell_cb(void *u, drflac_int64 *p) { int64_t x; ns_tell(&s_ns, &x); *p = x; return DRFLAC_TRUE; }
static drmp3_bool32 mp3_seek_cb(void *u, int o, drmp3_seek_origin g)
{
    return ns_seek(&s_ns, o, g == DRMP3_SEEK_SET ? NS_SEEK_SET : g == DRMP3_SEEK_CUR ? NS_SEEK_CUR : NS_SEEK_END);
}
static drmp3_bool32 mp3_tell_cb(void *u, drmp3_int64 *p) { int64_t x; ns_tell(&s_ns, &x); *p = x; return DRMP3_TRUE; }
static drwav_bool32 wav_seek_cb(void *u, int o, drwav_seek_origin g)
{
    return ns_seek(&s_ns, o, g == DRWAV_SEEK_SET ? NS_SEEK_SET : g == DRWAV_SEEK_CUR ? NS_SEEK_CUR : NS_SEEK_END);
}
static drwav_bool32 wav_tell_cb(void *u, drwav_int64 *p) { int64_t x; ns_tell(&s_ns, &x); *p = x; return DRWAV_TRUE; }

static void set_error(const char *msg)
{
    if (s_dec_abort) return;  // truncated data read when interrupted by a track change / stop, not a real error
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(s_error, msg, sizeof s_error);
    xSemaphoreGive(s_mx);
    ESP_LOGE(TAG, "%s", msg);
}

// Convert n frames of ch-channel int32 (full-scale 32 bit) in place to stereo 24 bit
static void to_stereo24(int32_t *b, int n, int ch)
{
    for (int i = 0; i < n; i++) {
        int32_t l = b[i * ch] >> 8, r = (ch > 1 ? b[i * ch + 1] : b[i * ch]) >> 8;
        b[2 * i] = l;
        b[2 * i + 1] = r;
    }
}

static void play_track(void)
{
    static uint8_t *head_cache, *hist;
    if (!head_cache) head_cache = heap_caps_malloc(HEAD_CACHE, MALLOC_CAP_SPIRAM);
    if (!hist) hist = heap_caps_malloc(HIST_WINDOW, MALLOC_CAP_SPIRAM);
    s_ns = (netstream_t){.size = -1, .restart = http_restart, .recv = net_recv, .cache = head_cache,
                         .cache_cap = head_cache ? HEAD_CACHE : 0, .hist = hist, .hist_cap = hist ? HIST_WINDOW : 0};
    s_dec_ready = false;
    s_size = -1;
    s_downloaded = 0;
    s_ctype[0] = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_error[0] = 0;
    xSemaphoreGive(s_mx);
    xEventGroupClearBits(s_ev, EV_HDR);
    http_restart(NULL, 0);
    if (!(xEventGroupWaitBits(s_ev, EV_HDR, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000)) & EV_HDR)) {
        set_error("Timed out connecting to the audio source");
        return;
    }
    if (s_error[0]) return;
    s_hdr_us = esp_timer_get_time();
    s_ns.size = s_size;

    // Detect the format from the magic bytes at the start of the file, not Content-Type (NetEase's old storage labels FLAC as audio/mpeg)
    uint8_t h[12] = {0};
    int hn = ns_peek(&s_ns, h, sizeof h);
    bool is_flac = hn >= 4 && !memcmp(h, "fLaC", 4);
    bool is_mp3 = hn >= 3 && (!memcmp(h, "ID3", 3) || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0));
    // Some NetEase tracks are WAV (also labelled audio/mpeg)
    bool is_wav = hn >= 12 && (!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4)) && !memcmp(h + 8, "WAVE", 4);
    bool is_m4a = hn >= 8 && !memcmp(h + 4, "ftyp", 4);  // some NetEase tracks only exist as M4A (AAC)
    if (!is_flac && !is_mp3 && !is_wav && !is_m4a) {
        char m[sizeof s_ctype + 80];  // big enough for the type and message, avoids truncation errors under -Os
        if (hn >= 4 && !memcmp(h, "OggS", 4)) snprintf(m, sizeof m, "Unsupported format: Ogg (%s)", s_ctype);
        else snprintf(m, sizeof m, "Unrecognised audio format (%s, starts with %02X %02X %02X %02X)", s_ctype, h[0], h[1], h[2], h[3]);
        set_error(m);
        return;
    }
    const char *kind = is_flac ? "FLAC" : is_wav ? "WAV" : is_m4a ? "M4A" : "MP3";
    if (is_mp3 != !!strstr(s_ctype, "mpeg")) ESP_LOGW(TAG, "Content-Type is %s, detected as %s from file content", s_ctype, kind);
    s_ns.allow_seek_end = !is_mp3;

    drflac *fl = NULL;
    drmp3 *mp = NULL;
    drwav *wv = NULL;
    m4a_t *m4 = NULL;
    int ch;
    if (is_m4a) {
        char m[96];
        m4 = m4a_open(&s_ns, m, sizeof m);
        if (!m4) { set_error(m); return; }
        int rate;
        int64_t total;
        m4a_info(m4, &rate, &ch, &total);
        s_rate = rate; s_bits = 16;
        s_total_frames = total;
        strcpy(s_codec, "AAC");
    } else if (is_wav) {
        wv = heap_caps_malloc(sizeof(drwav), MALLOC_CAP_8BIT);
        if (!wv || !drwav_init(wv, rd_cb, wav_seek_cb, wav_tell_cb, NULL, NULL)) {
            free(wv);
            set_error("Failed to parse WAV");
            return;
        }
        s_rate = wv->sampleRate; s_bits = wv->bitsPerSample; ch = wv->channels;
        s_total_frames = wv->totalPCMFrameCount;
        strcpy(s_codec, "WAV");
    } else if (!is_mp3) {
        fl = drflac_open(rd_cb, flac_seek_cb, flac_tell_cb, NULL, NULL);
        if (!fl) { set_error("Failed to parse FLAC"); return; }
        s_rate = fl->sampleRate; s_bits = fl->bitsPerSample; ch = fl->channels;
        s_total_frames = fl->totalPCMFrameCount;
        strcpy(s_codec, "FLAC");
    } else {
        mp = heap_caps_malloc(sizeof(drmp3), MALLOC_CAP_8BIT);
        if (!mp || !drmp3_init(mp, rd_cb, mp3_seek_cb, mp3_tell_cb, NULL, NULL, NULL)) {
            free(mp);
            set_error("Failed to parse MP3");
            return;
        }
        s_rate = mp->sampleRate; s_bits = 16; ch = mp->channels;
        s_total_frames = 0;  // MP3: don't scan the whole file, estimate duration from bitrate
        strcpy(s_codec, "MP3");
    }
    s_ch = ch;
    if (s_resume_sec > 0) {
        s_seek_frame = (int64_t)(s_resume_sec * s_rate);
        s_resume_sec = 0;
    }
    s_dec_ready = true;
    s_ready_us = esp_timer_get_time();
    s_ready_bytes = s_ns.consumed;
    ESP_LOGI(TAG, "%s %d Hz / %d bit / %d ch, file %lld bytes, duration %.1f s", s_codec, s_rate, s_bits, ch, s_size,
             s_total_frames ? (double)s_total_frames / s_rate : 0);

    if (!output_set_format(s_rate, s_bits >= 24 ? 24 : 16)) {
        char m[sizeof s_ctype + 80];  // big enough for the type and message, avoids truncation errors under -Os
        snprintf(m, sizeof m, "Unsupported: %d Hz (optical module limit is 96kHz)", s_rate);
        set_error(m);
    } else {
        int32_t *buf = heap_caps_malloc(CHUNK * (ch > 2 ? ch : 2) * sizeof(int32_t), MALLOC_CAP_8BIT);
        int16_t *b16 = mp ? heap_caps_malloc(CHUNK * ch * sizeof(int16_t), MALLOC_CAP_8BIT) : NULL;
        output_set_active(true);
        s_dec_frame = s_est_f0 = 0;
        s_est_c0 = s_ns.consumed;
        while (!s_dec_abort) {
            int64_t sk = s_seek_frame;
            if (sk >= 0) {
                s_seek_frame = -1;
                s_dec_break = s_dec_abort;
                output_flush();  // mute first: seeking takes time, don't keep playing old audio from the buffer
                int64_t t0 = esp_timer_get_time(), c0 = s_ns.consumed;
                int r0 = s_ns.restarts;
                bool ok = fl ? drflac_seek_to_pcm_frame(fl, sk) : wv ? drwav_seek_to_pcm_frame(wv, sk)
                        : m4 ? m4a_seek(m4, sk) : drmp3_seek_to_pcm_frame(mp, sk);
                ESP_LOGI(TAG, "Seek to %.1f s%s: positioning %d ms (%d re-requests, read %lld KB)", (double)sk / s_rate,
                         ok ? "" : " FAILED", (int)((esp_timer_get_time() - t0) / 1000), s_ns.restarts - r0,
                         (s_ns.consumed - c0) / 1024);
                output_flush();
                s_seek_us = t0;
                s_base_frame = sk;
                s_dec_frame = s_est_f0 = sk;
                s_est_c0 = s_ns.consumed;
            }
            int n;
            int64_t t0 = esp_timer_get_time();
            if (fl) {
                n = drflac_read_pcm_frames_s32(fl, CHUNK, (drflac_int32 *)buf);
            } else if (wv) {
                n = drwav_read_pcm_frames_s32(wv, CHUNK, (drwav_int32 *)buf);  // left-aligned to 32 bit, same as FLAC
            } else if (m4) {
                n = m4a_read(m4, buf, CHUNK);
            } else {
                n = drmp3_read_pcm_frames_s16(mp, CHUNK, b16);
                for (int i = 0; i < n * ch; i++) buf[i] = (int32_t)b16[i] << 16;
            }
            if (n <= 0) break;
            to_stereo24(buf, n, ch);
            s_dec_busy_us += esp_timer_get_time() - t0;
            s_dec_frame += n;
            output_write(buf, n, &s_dec_break, 0);
        }
        output_set_active(false);
        free(buf);
        free(b16);
    }
    if (fl) drflac_close(fl);
    if (wv) { drwav_uninit(wv); free(wv); }
    m4a_close(m4);
    if (mp) { drmp3_uninit(mp); free(mp); }
}

static void decoder_task(void *arg)
{
    while (1) {
        xEventGroupSetBits(s_ev, EV_DEC_IDLE);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xEventGroupClearBits(s_ev, EV_DEC_IDLE);
        s_dec_abort = false;
        s_dec_break = false;
        output_flush();
        s_base_frame = 0;
        play_track();
        http_stop();
        if (s_dec_abort) continue;  // interrupted by Stop / track change, the caller sets the state
        while (output_pending_frames() > 0 && !s_dec_abort) vTaskDelay(pdMS_TO_TICKS(20));  // wait for the buffer to drain
        if (!s_dec_abort) {
            s_state = "STOPPED";
            ESP_LOGI(TAG, "Playback finished%s", s_error[0] ? " (with error)" : "");
            livelab_release(OUT_SRC_DLNA);
        }
    }
}

static TaskHandle_t s_dec_task;

static void dec_stop(void)
{
    s_dec_abort = true;
    s_dec_break = true;
    output_pause(false);  // unpause first: the decoder may be waiting for the PCM buffer to drain
    output_flush();       // mute immediately: don't keep playing already-decoded old data while waiting for the decoder to stop
    xEventGroupWaitBits(s_ev, EV_DEC_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    output_flush();       // the decoder may have written a bit more before stopping
}

// ---------- Metadata ----------
static void xml_text(const char *m, const char *tag, char *out, size_t n)
{
    out[0] = 0;
    char open[48], close[48];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *p = m ? strstr(m, open) : NULL;
    if (!p || !(p = strchr(p, '>'))) return;
    const char *e = strstr(++p, close);
    if (!e) return;
    size_t l = e - p < (int)n - 1 ? e - p : n - 1;
    memcpy(out, p, l);
    out[l] = 0;
    // Unescape common entities
    char *w = out;
    for (char *r = out; *r;) {
        if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; }
        else if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; }
        else if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; }
        else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; }
        else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
        else *w++ = *r++;
    }
    *w = 0;
}

// ---------- Public interface (called from the HTTP server task) ----------
static uint8_t *s_net_store;  // storage for the compressed-data buffer; freed before OTA, reallocated on next playback

static void net_alloc(void)
{
    if (s_net_store) return;
    size_t cap = NET_BUF;
    s_net_store = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (!s_net_store) { cap = 64 * 1024; s_net_store = heap_caps_malloc(cap + 1, MALLOC_CAP_8BIT); }
    s_net = xStreamBufferCreateStatic(cap, 1, s_net_store, &s_net_struct);
}

void player_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
    s_http_start = xSemaphoreCreateBinary();
    s_http_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_http_idle);
    net_alloc();
    xTaskCreatePinnedToCoreWithCaps(http_task, "http_dl", 6144, NULL, 6, NULL, 0, MALLOC_CAP_SPIRAM);  // stack in PSRAM; decode and output task stacks stay in internal RAM (fast)
    xTaskCreatePinnedToCore(decoder_task, "decoder", 16384, NULL, 7, &s_dec_task, CONFIG_FREERTOS_NUMBER_OF_CORES - 1);
}

void player_set_uri(const char *uri, const char *meta)
{
    dec_stop();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    free(s_uri);
    s_uri = strdup(uri ? uri : "");
    xml_text(meta, "dc:title", s_title, sizeof s_title);
    xml_text(meta, "upnp:artist", s_artist, sizeof s_artist);
    xml_text(meta, "upnp:album", s_album, sizeof s_album);
    xml_text(meta, "upnp:albumArtURI", s_art, sizeof s_art);
    const char *h = strstr(s_uri, "://");
    h = h ? h + 3 : s_uri;
    size_t l = strcspn(h, "/:?");
    snprintf(s_host, sizeof s_host, "%.*s", (int)(l < 63 ? l : 63), h);
    s_error[0] = 0;
    s_codec[0] = 0;
    s_rate = s_bits = s_ch = 0;
    s_total_frames = 0;
    s_size = -1;
    s_downloaded = 0;
    s_ns.consumed = 0;
    s_ns.restarts = 0;
    s_start_ms = -1;
    s_seek_frame = -1;  // NetEase sends Seek(0) before pushing a new track; must not carry over to the new track
    s_resume_sec = 0;
    s_restored = false;
    xSemaphoreGive(s_mx);
    s_state = "STOPPED";
    ESP_LOGI(TAG, "New track: %s - %s (%s)", s_title, s_artist, s_host);
}

void player_play(void)
{
    if (s_restored) {  // pause restored after reboot: start as if stopped, from the remembered position
        s_restored = false;
        s_state = "STOPPED";
    }
    if (!strcmp((const char *)s_state, "PAUSED_PLAYBACK")) {
        if (output_source() != OUT_SRC_DLNA) livelab_take(OUT_SRC_DLNA);
        output_pause(false);
        s_state = "PLAYING";
        return;
    }
    if (!strcmp((const char *)s_state, "STOPPED") && s_uri && *s_uri) {
        dec_stop();
        net_alloc();  // the buffer was freed if an OTA update failed
        livelab_take(OUT_SRC_DLNA);  // latest source wins: pause AirPlay, disconnect Bluetooth
        s_play_us = esp_timer_get_time();
        s_start_ms = -1;
        s_state = "PLAYING";
        xTaskNotifyGive(s_dec_task);
    }
}

void player_pause(void)
{
    if (!strcmp((const char *)s_state, "PLAYING")) {
        output_pause(true);
        s_state = "PAUSED_PLAYBACK";
    }
}

void player_stop(void)
{
    s_restored = false;
    dec_stop();
    if (s_uri) s_state = "STOPPED";
    livelab_release(OUT_SRC_DLNA);
}

void player_seek(double sec)
{
    if (s_restored && sec >= 0) { s_resume_sec = sec; return; }  // not started yet: remember it, Play starts from here
    if (s_rate <= 0 || sec < 0) return;
    // Set the interrupt before the target: the other way round, the decoder may finish the seek between the two statements and clear the interrupt, and the leftover interrupt would drop a block of good audio
    s_dec_break = true;  // the decoder may be stuck waiting for buffer space; make it handle the seek right away
    s_seek_frame = (int64_t)(sec * s_rate);
}

const char *player_state(void) { return (const char *)s_state; }

static void update_start_ms(void)
{
    if (s_seek_us && output_first_audio_us() > s_seek_us) {
        ESP_LOGI(TAG, "Seek to audio %d ms", (int)((output_first_audio_us() - s_seek_us) / 1000));
        s_seek_us = 0;
    }
    if (s_start_ms < 0 && s_dec_ready && output_source() == OUT_SRC_DLNA && output_first_audio_us() > 0 && s_play_us > 0) {
        s_start_ms = (output_first_audio_us() - s_play_us) / 1000;
        ESP_LOGI(TAG, "Start-up latency %d ms (Play received to first audible frame)", s_start_ms);
        ESP_LOGI(TAG, "Start-up phases: headers %d ms, file header parse %d ms (read %lld KB, %d re-requests), buffer to audio %d ms",
                 (int)((s_hdr_us - s_play_us) / 1000), (int)((s_ready_us - s_hdr_us) / 1000), s_ready_bytes / 1024,
                 s_ns.restarts, (int)((output_first_audio_us() - s_ready_us) / 1000));
    }
}

bool player_has_error(void) { return s_error[0] != 0; }

void player_suspend_for_ota(void)
{
    bool had = s_uri && *s_uri;
    double pos = player_position();
    dec_stop();  // stop download and decoding: if playing during OTA, download and upload fight over WiFi and the upload times out
    s_rate = 0;
    heap_caps_free(s_net_store);  // free 1.5 MB of PSRAM so the whole firmware image fits in memory (fast)
    s_net_store = NULL;
    if (had) player_restore_paused(pos);  // report paused (synced to the controller via events); restore as usual after OTA
    ESP_LOGI(TAG, "Stopped playback before OTA, freed buffer, position %.1f s", pos);
}

void player_restore_paused(double sec)
{
    s_resume_sec = sec;
    s_restored = true;
    s_state = "PAUSED_PLAYBACK";
}

double player_position(void)
{
    update_start_ms();
    if (s_rate <= 0) return s_resume_sec;
    return (double)(s_base_frame + (int64_t)output_played_frames()) / s_rate;
}

double player_duration(void)
{
    if (s_rate <= 0) return 0;
    if (s_total_frames > 0) return (double)s_total_frames / s_rate;
    // Unknown (MP3): convert remaining bytes to duration using the decoder's "decoded frames / bytes read" ratio
    int64_t df = s_dec_frame - s_est_f0, dc = s_ns.consumed - s_est_c0;
    if (s_size > 0 && df > s_rate && dc > 0)
        return ((double)s_est_f0 + (double)(s_size - s_est_c0) * df / dc) / s_rate;
    return 0;
}

void player_get_info(player_info_t *o)
{
    memset(o, 0, sizeof *o);
    update_start_ms();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    o->state = (const char *)s_state;
    strlcpy(o->title, s_title, sizeof o->title);
    strlcpy(o->artist, s_artist, sizeof o->artist);
    strlcpy(o->album, s_album, sizeof o->album);
    strlcpy(o->art, s_art, sizeof o->art);
    strlcpy(o->codec, s_codec, sizeof o->codec);
    strlcpy(o->error, s_error, sizeof o->error);
    strlcpy(o->host, s_host, sizeof o->host);
    xSemaphoreGive(s_mx);
    o->lossless = !strcmp(o->codec, "FLAC") || !strcmp(o->codec, "WAV");
    o->sample_rate = s_rate;
    o->bits = s_bits;
    o->channels = s_ch;
    o->duration = player_duration();
    o->position = player_position();
    o->file_size = s_size;
    o->downloaded = s_downloaded;
    o->consumed = s_ns.consumed;
    o->buf_bytes = s_net_store ? xStreamBufferBytesAvailable(s_net) : 0;
    o->buf_cap = s_net_store ? xStreamBufferBytesAvailable(s_net) + xStreamBufferSpacesAvailable(s_net) : 0;
    o->pcm_ms = s_rate > 0 ? output_pending_frames() * 1000LL / s_rate : 0;
    o->avg_kbps = (o->duration > 0 && s_size > 0) ? (int)(s_size * 8 / o->duration / 1000) : 0;
    o->underruns = output_underruns();
    o->start_ms = s_start_ms;
    o->restarts = s_ns.restarts;
    static int64_t last_busy, last_t;  // only called by web polling, single-threaded
    int64_t now = esp_timer_get_time(), busy = s_dec_busy_us;
    if (last_t && now > last_t) o->dec_load = (int)((busy - last_busy) * 100 / (now - last_t));
    last_busy = busy;
    last_t = now;
}
