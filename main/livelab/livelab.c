#include "esp_heap_caps.h"
#include "livelab.h"
#include <string.h>
#include "dacp_client.h"
#include "dlna.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "playback_events.h"
#include "player.h"
#include "rtsp_server.h"
#include "web.h"
#include "web_server.h"
#include "wifi.h"
#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

#define TAG "livelab"
#define DLNA_PORT 49152

static QueueHandle_t s_evq;
#define EV_KICK_AIRPLAY ((playback_event_t)100)  // internal arbiter-task command: disconnect the AirPlay session

typedef struct {
    playback_source_t src;
    playback_event_t ev;
} ev_t;
static volatile playback_event_t s_last[PLAYBACK_SOURCE_COUNT];  // latest playback event of each source
static volatile bool s_seen[PLAYBACK_SOURCE_COUNT];

static bool src_playing(playback_source_t s)
{
    // Also check whether upstream's own session record still exists: when the peer disconnects (TEARDOWN) there isn't always a DISCONNECTED event
    return s_seen[s] && s_last[s] == PLAYBACK_EVENT_PLAYING && playback_events_source_active(s);
}

bool livelab_source_playing(playback_source_t s) { return src_playing(s); }

const char *livelab_active_source(void)
{
    switch (output_source()) {
    case OUT_SRC_DLNA: return "dlna";
    case OUT_SRC_BT: return "bluetooth";
    case OUT_SRC_AIRPLAY: return playback_events_source_active(PLAYBACK_SOURCE_AIRPLAY) ? "airplay" : "none";
    default: return "none";
    }
}

void livelab_take(out_src_t src)
{
    out_src_t cur = output_source();
    // First stop other sources that are playing: DLNA stops (NetEase receives STOPPED), AirPlay asks the phone to pause via DACP,
    // Bluetooth is disconnected outright (staying connected slows WiFi, and both lossless DLNA and AirPlay need bandwidth)
    if (src != OUT_SRC_DLNA && (cur == OUT_SRC_DLNA || !strcmp(player_state(), "PLAYING") ||
                                !strcmp(player_state(), "PAUSED_PLAYBACK"))) {
        ESP_LOGI(TAG, "%s pre-empts: stopping DLNA", output_source_name(src));
        player_stop();
    }
    if (src != OUT_SRC_AIRPLAY && src_playing(PLAYBACK_SOURCE_AIRPLAY)) {
        if (dacp_is_active()) {
            ESP_LOGI(TAG, "%s pre-empts: asking iPhone to pause AirPlay", output_source_name(src));
            dacp_send_playpause();
        } else {
            // AirPlay 2 iPhones don't enable DACP remote control, so the only option is to drop the session (the iPhone stops by itself when it sees AirPlay disconnect).
            // Restarting the RTSP server has to wait for its task to exit; do it in the arbiter task so the caller (e.g. NetEase's SOAP request) isn't blocked
            ESP_LOGI(TAG, "%s pre-empts: disconnecting AirPlay session", output_source_name(src));
            ev_t e = {PLAYBACK_SOURCE_AIRPLAY, EV_KICK_AIRPLAY};
            xQueueSend(s_evq, &e, 0);
        }
    }
#ifdef CONFIG_BT_A2DP_ENABLE
    if (src != OUT_SRC_BT && bt_a2dp_sink_is_connected()) {
        ESP_LOGI(TAG, "%s pre-empts: disconnecting Bluetooth", output_source_name(src));
        bt_a2dp_sink_disconnect();
    }
#endif
    livelab_output_take(src);
}

void livelab_release(out_src_t src)
{
    if (output_source() == src) livelab_output_take(OUT_SRC_AIRPLAY);  // idle: AirPlay pulls on demand, silence when there's no session
}

static bool s_player_up;  // DLNA / player started (only after the network is up)

void livelab_ota_prepare(void)
{
    if (!s_player_up) return;
    player_suspend_for_ota();
    livelab_release(OUT_SRC_DLNA);
}

// Upstream playback events (AirPlay / Bluetooth): only pre-empt the output on a transition from "not playing" to "playing" (AirPlay sends PLAYING repeatedly while playing,
// which must not be treated as a new start each time, or it would grab back the output DLNA just took). Done in our own task so the Bluetooth stack and RTSP task aren't blocked

static void on_event(playback_source_t source, playback_event_t event, const playback_event_data_t *data, void *ctx)
{
    if (source <= PLAYBACK_SOURCE_NONE || source >= PLAYBACK_SOURCE_COUNT || event == PLAYBACK_EVENT_METADATA) return;
    bool was_playing = src_playing(source);
    s_last[source] = event;
    s_seen[source] = true;
    if (event == PLAYBACK_EVENT_PLAYING && !was_playing) {
        ev_t e = {source, event};
        xQueueSend(s_evq, &e, 0);
    }
}

static void arbiter_task(void *arg)
{
    ev_t e;
    while (xQueueReceive(s_evq, &e, portMAX_DELAY)) {
        if (e.ev == EV_KICK_AIRPLAY) {
            rtsp_server_stop();   // drop all AirPlay connections
            rtsp_server_start();  // listen again so the iPhone can reconnect later
            s_last[PLAYBACK_SOURCE_AIRPLAY] = PLAYBACK_EVENT_DISCONNECTED;  // session dropped, no longer treat it as playing
            continue;
        }
        if (e.ev != PLAYBACK_EVENT_PLAYING) continue;
        if (e.src == PLAYBACK_SOURCE_AIRPLAY && output_source() != OUT_SRC_AIRPLAY) livelab_take(OUT_SRC_AIRPLAY);
        else if (e.src == PLAYBACK_SOURCE_BLUETOOTH && output_source() != OUT_SRC_BT) livelab_take(OUT_SRC_BT);
    }
}

#if CONFIG_LIVELAB_SIM
// ---------- Simulator only: pretend AirPlay is playing ----------
// Replace AirPlay's data source with an incrementing sawtooth (+1 per sample, left and right opposite) and emit AirPlay playback events,
// to test pre-emption and the AirPlay pull path in QEMU (real AirPlay needs mDNS, PTP and UDP, which don't run in the simulator)
#include "audio_output.h"
#include "audio_receiver.h"
static volatile int16_t s_ramp;

static size_t sim_airplay_source(int16_t *buf, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        buf[2 * i] = s_ramp;
        buf[2 * i + 1] = (int16_t)-s_ramp;
        s_ramp++;
    }
    return frames;
}

static esp_err_t sim_handler(httpd_req_t *r)
{
    char cmd[24] = {0};
    int l = httpd_req_recv(r, cmd, sizeof cmd - 1);
    if (l <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "empty");
    if (!strcmp(cmd, "airplay_play")) {
        s_ramp = 0;
        audio_output_set_source(sim_airplay_source);
        playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_CONNECTED, NULL);
        playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_PLAYING, NULL);
    } else if (!strcmp(cmd, "airplay_repeat")) {
        // Real AirPlay sends PLAYING repeatedly while playing (on every SETRATEANCHORTIME etc.); must not be treated as a new start
        playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_PLAYING, NULL);
    } else if (!strcmp(cmd, "airplay_stop")) {
        audio_output_set_source(NULL);
        playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_DISCONNECTED, NULL);
    } else {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "unknown");
    }
    ESP_LOGI(TAG, "Simulator: %s", cmd);
    return httpd_resp_sendstr(r, "ok");
}
#endif

// Boot-failure counter shared with the bootloader hook (bootloader_components/livelab_boot_hooks):
// clear it once the firmware has been up for a while, so only boot loops lead to recovery mode.
#define BOOT_MAGIC 0xB0070000u
static void healthy_cb(void *arg)
{
    REG_WRITE(RTC_CNTL_STORE0_REG, BOOT_MAGIC);
    ESP_LOGI(TAG, "Running for 60 s: boot-failure counter cleared");
}

static bool get_ip(char *ip, size_t n)
{
    if (ethernet_is_connected() && ethernet_get_ip_str(ip, n) == ESP_OK && ip[0]) return true;
    return wifi_is_connected() && wifi_get_ip_str(ip, n) == ESP_OK && ip[0];
}

void livelab_start(void)
{
    static bool started;
    if (started) return;
    char ip[16];
    if (!web_server_handle()) return;  // web server not up yet, call again later
    if (!get_ip(ip, sizeof ip)) {
        ESP_LOGW(TAG, "No IP yet, DLNA will start later");
        return;
    }
    started = true;
    output_init();
    player_init();
    s_player_up = true;

    s_evq = xQueueCreate(8, sizeof(ev_t));
    xTaskCreatePinnedToCoreWithCaps(arbiter_task, "arbiter", 4096, NULL, 5, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    playback_events_register(on_event, NULL);

    // DLNA uses its own HTTP server: upstream's web server only allows 2-3 connections, NetEase's control, description and event subscriptions wouldn't fit
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = DLNA_PORT;
    cfg.ctrl_port = DLNA_PORT + 1;
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 4;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.task_caps = MALLOC_CAP_SPIRAM;  // stack in PSRAM: with Bluetooth on, only ~70 KB of internal RAM is left
    httpd_handle_t h;
    ESP_ERROR_CHECK(httpd_start(&h, &cfg));
    dlna_start(h, ip, DLNA_PORT);
    web_start(web_server_handle(), ip);
#if CONFIG_LIVELAB_SIM
    httpd_uri_t su = {.uri = "/sim", .method = HTTP_POST, .handler = sim_handler};
    httpd_register_uri_handler(h, &su);  // attached to the DLNA HTTP server, doesn't take a slot on the web server
#endif
    ESP_LOGI(TAG, "DLNA started: http://%s:%d/description.xml, web page http://%s/", ip, DLNA_PORT, ip);

    // Rollback: a new image stays "pending verify" until it gets this far (network up, web and OTA
    // reachable); if it reboots before that, the bootloader goes back to the previous image.
#if CONFIG_LIVELAB_BOOT_TEST == 1
    ESP_LOGE(TAG, "Boot test 1: crashing before marking the image valid");
    abort();
#endif
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "Firmware marked valid");
#if CONFIG_LIVELAB_BOOT_TEST == 2
    ESP_LOGE(TAG, "Boot test 2: crashing in 10 s");
    static esp_timer_handle_t ct;
    const esp_timer_create_args_t ca = {.callback = (esp_timer_cb_t)abort, .name = "crash_test"};
    if (esp_timer_create(&ca, &ct) == ESP_OK) esp_timer_start_once(ct, 10 * 1000000LL);
#endif
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = {.callback = healthy_cb, .name = "healthy"};
    if (!t && esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 60 * 1000000LL);
}
