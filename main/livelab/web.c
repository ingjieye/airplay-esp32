#include "web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dacp_client.h"
#include "dlna.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "livelab.h"
#include "output.h"
#include "playback_control.h"
#include "playback_events.h"
#include "player.h"
#include "settings.h"
#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

#define TAG "web"
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
static char s_ip[16];

static esp_err_t page_handler(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);  // EMBED_TXTFILES appends an extra \0
}

// Append a JSON string (escaped)
static int jstr(char *b, int o, int n, const char *s)
{
    if (o < n) b[o] = '"';
    o++;
    for (; *s; s++) {
        unsigned char c = *s;
        if (o + 7 >= n) break;
        if (c == '"' || c == '\\') { b[o++] = '\\'; b[o++] = c; }
        else if (c < 0x20) o += snprintf(b + o, n - o, "\\u%04x", c);
        else b[o++] = c;
    }
    if (o < n) b[o] = '"';
    return o + 1;
}

#define ADD(...) do { o += snprintf(b + o, o < n ? n - o : 0, __VA_ARGS__); } while (0)
#define ADDS(key, val) do { ADD("\"%s\":", key); o = jstr(b, o, n, val); ADD(","); } while (0)
#define JB(x) ((x) ? "true" : "false")

static bool bt_connected(void)
{
#ifdef CONFIG_BT_A2DP_ENABLE
    return bt_a2dp_sink_is_connected();
#else
    return false;
#endif
}

static esp_err_t status_handler(httpd_req_t *r)
{
    static player_info_t pi;  // large; keep it off the HTTP task stack
    player_get_info(&pi);
    const int n = 4096;
    char *b = malloc(n);
    if (!b) return httpd_resp_send_500(r);
    int o = 0;
    const char *src = livelab_active_source();
    ADD("{");
    // ---- DLNA (field names kept from the old version; the simulator tests depend on them) ----
    ADDS("state", pi.state);
    ADDS("title", pi.title);
    ADDS("artist", pi.artist);
    ADDS("album", pi.album);
    ADDS("art", pi.art);
    ADDS("codec", pi.codec);
    ADDS("error", pi.error);
    ADDS("host", pi.host);
    ADD("\"lossless\":%s,\"rate\":%d,\"bits\":%d,\"ch\":%d,", JB(pi.lossless), pi.sample_rate, pi.bits, pi.channels);
    ADD("\"duration\":%.1f,\"position\":%.2f,\"size\":%lld,\"downloaded\":%lld,\"consumed\":%lld,",
        pi.duration, pi.position, pi.file_size, pi.downloaded, pi.consumed);
    ADD("\"buf\":%d,\"bufCap\":%d,\"pcmMs\":%d,\"avgKbps\":%d,\"underruns\":%lu,\"startMs\":%d,\"restarts\":%d,\"decLoad\":%d,",
        pi.buf_bytes, pi.buf_cap, pi.pcm_ms, pi.avg_kbps, (unsigned long)pi.underruns, pi.start_ms, pi.restarts, pi.dec_load);

    // ---- Current source and "now playing" (AirPlay / Bluetooth come from upstream playback events) ----
    ADDS("source", src);
    if (strcmp(src, "dlna")) {
        playback_metadata_t md;
        playback_source_t ps = !strcmp(src, "airplay") ? PLAYBACK_SOURCE_AIRPLAY : PLAYBACK_SOURCE_BLUETOOTH;
        bool have = strcmp(src, "none") && playback_events_get_metadata(&md);
        ADD("\"np\":{");
        ADDS("title", have ? md.title : "");
        ADDS("artist", have ? md.artist : "");
        ADDS("album", have ? md.album : "");
        ADD("\"duration\":%lu,\"position\":%lu,\"playing\":%s},", have ? (unsigned long)md.duration_secs : 0,
            have ? (unsigned long)md.position_secs : 0, JB(strcmp(src, "none") && livelab_source_playing(ps)));
    }
    ADD("\"outRate\":%d,\"outBits\":%d,", output_rate(), output_bits());
    ADD("\"airplay\":{\"connected\":%s,\"playing\":%s,\"remote\":%s},",
        JB(playback_events_source_active(PLAYBACK_SOURCE_AIRPLAY)), JB(livelab_source_playing(PLAYBACK_SOURCE_AIRPLAY)),
        JB(dacp_is_active()));
#ifdef CONFIG_BT_A2DP_ENABLE
    ADD("\"bt\":{\"supported\":true,\"connected\":%s,\"playing\":%s},", JB(bt_connected()),
        JB(livelab_source_playing(PLAYBACK_SOURCE_BLUETOOTH)));
#else
    ADD("\"bt\":{\"supported\":false,\"connected\":false,\"playing\":false},");
#endif

    // ---- Volume: follows the current source ----
    int vol = !strcmp(src, "airplay") ? playback_control_get_level_percent() : output_volume();
    bool muted = !strcmp(src, "airplay") ? playback_control_is_muted() : output_muted();
    ADD("\"vol\":{\"value\":%d,\"muted\":%s,\"applies\":%s,\"settable\":%s},\"fixedOptical\":%s,", vol, JB(muted),
        JB(output_volume_applies()), JB(strcmp(src, "bluetooth")), JB(output_fixed_optical()));

    // ---- Outputs ----
    ADD("\"output\":\"%s\",\"outputs\":[", output_get(output_current())->id);
    for (int i = 0; i < output_count(); i++) {
        const output_desc_t *d = output_get(i);
        ADD("%s{", i ? "," : "");
        ADDS("id", d->id);
        ADDS("name", d->name);
        ADDS("note", d->note);
        ADD("\"available\":%s}", JB(d->available));
    }
    ADD("],");

    // ---- Device ----
    char name[65];
    settings_get_device_name(name, sizeof name);
    wifi_ap_record_t ap;
    bool sta = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof macs, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_app_desc_t *app = esp_app_get_description();
    ADD("\"dev\":{");
    ADDS("name", name);
    ADDS("ip", s_ip);
    ADDS("mac", macs);
    ADDS("ssid", sta ? (const char *)ap.ssid : "");
    ADDS("fw", app->version);
    ADDS("idf", app->idf_ver);
    ADD("\"rssi\":%d,\"channel\":%d,\"uptime\":%lld,\"heap\":%u,\"heapMin\":%u,\"psram\":%u}", sta ? ap.rssi : 0,
        sta ? ap.primary : 0, esp_timer_get_time() / 1000000,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // Legacy fields for compatibility
    ADD(",\"ip\":\"%s\",\"rssi\":%d,\"uptime\":%lld,\"heap\":%u,\"psram\":%u}", s_ip, sta ? ap.rssi : 0,
        esp_timer_get_time() / 1000000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (o >= n) { free(b); ESP_LOGE(TAG, "Status JSON too long"); return httpd_resp_send_500(r); }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, b, o);
    free(b);
    return e;
}

static int recv_body(httpd_req_t *r, char *buf, size_t n)
{
    int l = httpd_req_recv(r, buf, n - 1);
    if (l <= 0) return -1;
    buf[l] = 0;
    return l;
}

static esp_err_t output_handler(httpd_req_t *r)
{
    char id[16];
    if (recv_body(r, id, sizeof id) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    esp_err_t e = output_select(id);
    if (e == ESP_ERR_NOT_SUPPORTED) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Output not connected yet");
    if (e != ESP_OK) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Unknown output");
    return httpd_resp_sendstr(r, "ok");
}

// Local control (web page buttons), forwarded according to the current source:
//   DLNA: control the player directly; state is synced to NetEase via UPnP events (previous/next belong to the controller, impossible in DLNA)
//   AirPlay: remote-control the iPhone via DACP; Bluetooth: remote-control the phone via AVRCP
static esp_err_t control_handler(httpd_req_t *r)
{
    char cmd[32];
    if (recv_body(r, cmd, sizeof cmd) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    const char *src = livelab_active_source();
    bool ok = true;
    if (!strncmp(cmd, "volume:", 7) || !strncmp(cmd, "mute:", 5)) {
        bool is_vol = cmd[0] == 'v';
        int v = atoi(cmd + (is_vol ? 7 : 5));
        if (!strcmp(src, "airplay")) {
            if (is_vol) playback_control_set_volume_percent(v);
            else playback_control_set_muted(v);
        } else if (!strcmp(src, "bluetooth")) {
            ok = false;  // Bluetooth volume is controlled by the phone
        } else {
            dlna_set_volume(is_vol ? v : output_volume(), is_vol ? output_muted() : v);
        }
    } else if (!strcmp(src, "dlna") || !strcmp(src, "none")) {
        if (!strcmp(cmd, "play")) player_play();
        else if (!strcmp(cmd, "pause")) player_pause();
        else if (!strcmp(cmd, "stop")) player_stop();
        else if (!strncmp(cmd, "seek:", 5)) player_seek(atof(cmd + 5));
        else ok = false;
    } else {
        // AirPlay / Bluetooth: the protocols only have a play/pause toggle; decide whether to send it from the current state
        playback_source_t ps = !strcmp(src, "airplay") ? PLAYBACK_SOURCE_AIRPLAY : PLAYBACK_SOURCE_BLUETOOTH;
        bool playing = livelab_source_playing(ps);
        playback_control_set_source(ps);
        if (!strcmp(cmd, "play")) { if (!playing) playback_control_play_pause(); }
        else if (!strcmp(cmd, "pause") || !strcmp(cmd, "stop")) { if (playing) playback_control_play_pause(); }
        else if (!strcmp(cmd, "next")) playback_control_next();
        else if (!strcmp(cmd, "prev")) playback_control_prev();
        else ok = false;
    }
    if (!ok) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Command not supported by the current source");
    ESP_LOGI(TAG, "Web control (%s): %s", src, cmd);
    return httpd_resp_sendstr(r, "ok");
}

// Settings: fixed_optical=0|1 (fixed volume on optical)
static esp_err_t settings_handler(httpd_req_t *r)
{
    char kv[48];
    if (recv_body(r, kv, sizeof kv) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    if (!strncmp(kv, "fixed_optical=", 14)) output_set_fixed_optical(kv[14] == '1');
    else return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Unknown setting");
    ESP_LOGI(TAG, "Setting: %s", kv);
    return httpd_resp_sendstr(r, "ok");
}

void web_start(httpd_handle_t h, const char *ip)
{
    strlcpy(s_ip, ip, sizeof s_ip);
    const httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/ll/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/ll/output", .method = HTTP_POST, .handler = output_handler},
        {.uri = "/api/ll/control", .method = HTTP_POST, .handler = control_handler},
        {.uri = "/api/ll/settings", .method = HTTP_POST, .handler = settings_handler},
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) {
        esp_err_t e = httpd_register_uri_handler(h, &u[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "Failed to register %s: %s", u[i].uri, esp_err_to_name(e));
    }
}
