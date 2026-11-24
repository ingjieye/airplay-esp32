#include "esp_heap_caps.h"
#include "gena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <errno.h>
#include <fcntl.h>
#include "lwip/sockets.h"
#include "esp_memory_utils.h"
#include "nvs.h"

#define TAG "gena"
#define MAX_SUBS 6
#define TIMEOUT_S 1800  // lease when the controller gives no TIMEOUT; otherwise use its value (clamped to 1 minute - 1 day)
static const char *SVC_NAME[GENA_NSVC] = {"AVTransport", "RenderingControl", "ConnectionManager"};

typedef struct {
    bool used, pending;  // pending: needs a notification (initial event of a new subscription, or state changed)
    int svc;
    char sid[48];
    char host[40];
    int port;
    char path[128];
    uint32_t seq;
    int64_t expire_us;
    int fails;
} sub_t;

static sub_t s_subs[MAX_SUBS];
static SemaphoreHandle_t s_mx;
static gena_body_fn s_body;

// ---------- Subscriptions saved to NVS ----------
// After the device reboots the controller (NetEase etc.) doesn't know: it doesn't poll, just waits for events, and only renews shortly before timeout,
// so it keeps thinking we're playing. Save the subscriptions, restore them at boot and push the current state right away so the controller catches up.
// Written only when subscriptions are added/removed and on normal reboot; renewals and notifications don't write, to avoid flash wear.
// A task writing flash must not have its stack in PSRAM (IDF asserts), and the GENA and DLNA HTTP task stacks are both in PSRAM,
// so normally the esp_timer task (stack in internal RAM) does the write, which also merges several subscriptions in a row into one write.
#define NVS_NS "dlna"
#define SEQ_GAP 100  // after an abnormal reboot (crash, power loss) the saved SEQ is stale; jump ahead so it's larger than anything the controller has seen
typedef struct {
    uint8_t svc;
    uint16_t port;
    uint32_t seq;
    char sid[48], host[40], path[128];
} saved_sub_t;

// The caller must not hold s_mx, and its stack must not be in PSRAM
static void subs_save(bool clean)
{
    static saved_sub_t v[MAX_SUBS];  // the esp_timer task stack is small, keep this off the stack
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        const sub_t *s = &s_subs[i];
        if (!s->used) continue;
        v[n] = (saved_sub_t){.svc = s->svc, .port = s->port, .seq = s->seq};
        strcpy(v[n].sid, s->sid);
        strcpy(v[n].host, s->host);
        strcpy(v[n].path, s->path);
        n++;
    }
    xSemaphoreGive(s_mx);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (n) nvs_set_blob(h, "subs", v, n * sizeof v[0]);
    else nvs_erase_key(h, "subs");
    nvs_set_u8(h, "subs_clean", clean);
    nvs_commit(h);
    nvs_close(h);
}

static void subs_load(void)
{
    saved_sub_t v[MAX_SUBS];
    size_t len = sizeof v;
    uint8_t clean = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    esp_err_t e = nvs_get_blob(h, "subs", v, &len);
    nvs_get_u8(h, "subs_clean", &clean);
    nvs_close(h);
    if (e != ESP_OK) return;
    int n = len / sizeof v[0];
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < n && i < MAX_SUBS; i++) {
        sub_t *s = &s_subs[i];
        *s = (sub_t){.used = true, .pending = true, .svc = v[i].svc, .port = v[i].port,
                     .seq = v[i].seq + (clean ? 0 : SEQ_GAP), .expire_us = now + TIMEOUT_S * 1000000LL};
        strcpy(s->sid, v[i].sid);
        strcpy(s->host, v[i].host);
        strcpy(s->path, v[i].path);
        ESP_LOGI(TAG, "Restored subscription %s → http://%s:%d%s (%s, SEQ %lu)", SVC_NAME[s->svc], s->host, s->port, s->path,
                 s->sid, (unsigned long)s->seq);
    }
}

static esp_timer_handle_t s_save_timer;
static void save_timer_cb(void *arg) { subs_save(false); }
static void subs_changed(void)
{
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 500 * 1000);
}

void gena_save(void)
{
    if (esp_ptr_external_ram(__builtin_frame_address(0))) { ESP_LOGW(TAG, "Current task stack is in PSRAM, can't write flash, subscriptions not saved"); return; }
    if (s_mx) subs_save(true);
}

// Parse CALLBACK: <http://192.168.31.66:49152/path> (there may be several, take the first)
static bool parse_callback(const char *cb, sub_t *s)
{
    const char *p = strstr(cb, "http://");
    if (!p) return false;
    p += 7;
    const char *hend = p + strcspn(p, ":/>");
    snprintf(s->host, sizeof s->host, "%.*s", (int)(hend - p), p);
    s->port = 80;
    if (*hend == ':') s->port = atoi(hend + 1);
    const char *path = strchr(p, '/');
    const char *end = strchr(p, '>');
    if (path && (!end || path < end)) snprintf(s->path, sizeof s->path, "%.*s", (int)((end ? end : path + strlen(path)) - path), path);
    else strcpy(s->path, "/");
    return s->host[0] != 0;
}

// Grant the lease the controller requested in TIMEOUT (as gmrender / upmpdcli do)
static int req_timeout(httpd_req_t *r)
{
    char t[32] = {0};
    if (httpd_req_get_hdr_value_str(r, "TIMEOUT", t, sizeof t) != ESP_OK) return TIMEOUT_S;
    if (!strncasecmp(t, "Second-infinite", 15)) return 86400;
    int v = !strncasecmp(t, "Second-", 7) ? atoi(t + 7) : 0;
    return v <= 0 ? TIMEOUT_S : v < 60 ? 60 : v > 86400 ? 86400 : v;
}

esp_err_t gena_handler(httpd_req_t *r)
{
    int svc = (int)(intptr_t)r->user_ctx;
    char sid[48] = {0}, cb[200] = {0};
    httpd_req_get_hdr_value_str(r, "SID", sid, sizeof sid);
    httpd_req_get_hdr_value_str(r, "CALLBACK", cb, sizeof cb);
    int64_t now = esp_timer_get_time();
    int tmo = req_timeout(r);
    char tbuf[32], treq[32] = "none";
    snprintf(tbuf, sizeof tbuf, "Second-%d", tmo);
    httpd_req_get_hdr_value_str(r, "TIMEOUT", treq, sizeof treq);

    xSemaphoreTake(s_mx, portMAX_DELAY);
    sub_t *found = NULL;
    for (int i = 0; i < MAX_SUBS; i++)
        if (s_subs[i].used && sid[0] && !strcmp(s_subs[i].sid, sid)) found = &s_subs[i];

    if (r->method == HTTP_UNSUBSCRIBE) {
        if (found) found->used = false;
        xSemaphoreGive(s_mx);
        ESP_LOGI(TAG, "Unsubscribe %s %s%s", SVC_NAME[svc], sid, found ? "" : " (unknown SID)");
        if (found) subs_changed();
        if (!found) { httpd_resp_set_status(r, "412 Precondition Failed"); }
        return httpd_resp_send(r, NULL, 0);
    }

    if (sid[0]) {  // renewal
        if (!found) {
            xSemaphoreGive(s_mx);
            ESP_LOGW(TAG, "Renewal failed: unknown SID %s", sid);
            httpd_resp_set_status(r, "412 Precondition Failed");
            return httpd_resp_send(r, NULL, 0);
        }
        found->expire_us = now + tmo * 1000000LL;
        xSemaphoreGive(s_mx);
        ESP_LOGI(TAG, "Renew %s %s (requested TIMEOUT %s)", SVC_NAME[svc], sid, treq);
        httpd_resp_set_hdr(r, "SID", sid);
        httpd_resp_set_hdr(r, "TIMEOUT", tbuf);
        return httpd_resp_send(r, NULL, 0);
    }

    // New subscription: if the same callback already has a subscription, keep its SID (as Macast does: the old SID the controller holds stays valid for renewal),
    // just refresh the lease and re-push the current state; otherwise find a free slot, evicting the one expiring soonest if there is none
    sub_t n = {0};
    if (!parse_callback(cb, &n)) {
        xSemaphoreGive(s_mx);
        ESP_LOGW(TAG, "Subscription without a valid CALLBACK: %s", cb);
        httpd_resp_set_status(r, "412 Precondition Failed");
        return httpd_resp_send(r, NULL, 0);
    }
    sub_t *s = NULL;
    for (int i = 0; i < MAX_SUBS && !s; i++)
        if (s_subs[i].used && s_subs[i].svc == svc && s_subs[i].port == n.port && !strcmp(s_subs[i].host, n.host) &&
            !strcmp(s_subs[i].path, n.path))
            s = &s_subs[i];
    if (s) {
        s->expire_us = now + tmo * 1000000LL;
        s->fails = 0;
        s->pending = true;
        char sid_copy[48];
        strcpy(sid_copy, s->sid);
        xSemaphoreGive(s_mx);
        ESP_LOGI(TAG, "Duplicate subscription %s → http://%s:%d%s, keeping %s (requested TIMEOUT %s)", SVC_NAME[svc], n.host, n.port, n.path,
                 sid_copy, treq);
        httpd_resp_set_hdr(r, "SID", sid_copy);
        httpd_resp_set_hdr(r, "TIMEOUT", tbuf);
        return httpd_resp_send(r, NULL, 0);
    }
    for (int i = 0; i < MAX_SUBS && !s; i++)
        if (!s_subs[i].used) s = &s_subs[i];
    if (!s) {
        s = &s_subs[0];
        for (int i = 1; i < MAX_SUBS; i++)
            if (s_subs[i].expire_us < s->expire_us) s = &s_subs[i];
    }
    *s = n;
    uint32_t a = esp_random(), b = esp_random();
    snprintf(s->sid, sizeof s->sid, "uuid:%08lx-%04lx-4%03lx-a%03lx-%08lx%04lx", (unsigned long)a,
             (unsigned long)(b & 0xFFFF), (unsigned long)((b >> 16) & 0xFFF), (unsigned long)(a & 0xFFF),
             (unsigned long)esp_random(), (unsigned long)(b >> 16));
    s->svc = svc;
    s->expire_us = now + tmo * 1000000LL;
    s->used = true;
    s->pending = true;  // the initial event is sent after the subscribe response
    char sid_copy[48];
    strcpy(sid_copy, s->sid);
    ESP_LOGI(TAG, "Subscribe %s → http://%s:%d%s (%s, requested TIMEOUT %s)", SVC_NAME[svc], s->host, s->port, s->path, s->sid, treq);
    xSemaphoreGive(s_mx);
    subs_changed();
    httpd_resp_set_hdr(r, "SID", sid_copy);
    httpd_resp_set_hdr(r, "TIMEOUT", tbuf);
    return httpd_resp_send(r, NULL, 0);
}

// Returns the HTTP status code from the controller, 0 if unreachable or no response
static int send_notify(const sub_t *s, const char *body)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(s->port), .sin_addr.s_addr = inet_addr(s->host)};
    int code = 0;
    // 2 s connect timeout: when the phone is offline a blocking connect retries for 20+ s, holding up notifications to other subscribers and reboots
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int c = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (c < 0 && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        int err = 0;
        socklen_t el = sizeof err;
        c = (select(fd + 1, NULL, &w, NULL, &tv) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && !err) ? 0 : -1;
    }
    fcntl(fd, F_SETFL, fl);
    if (c == 0) {
        char h[400];
        int bl = strlen(body);
        int hl = snprintf(h, sizeof h,
                          "NOTIFY %s HTTP/1.1\r\nHOST: %s:%d\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                          "NT: upnp:event\r\nNTS: upnp:propchange\r\nSID: %s\r\nSEQ: %lu\r\nCONTENT-LENGTH: %d\r\n"
                          "CONNECTION: close\r\n\r\n",
                          s->path, s->host, s->port, s->sid, (unsigned long)s->seq, bl);
        if (send(fd, h, hl, 0) == hl && send(fd, body, bl, 0) == bl) {
            char resp[32] = {0};
            int n = recv(fd, resp, sizeof resp - 1, 0);
            if (n > 12 && !strncmp(resp, "HTTP/", 5)) code = atoi(strchr(resp, ' ') ? strchr(resp, ' ') + 1 : "0");
        }
    }
    close(fd);
    return code;
}

static void gena_task(void *arg)
{
    char *last[GENA_NSVC] = {0};
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));  // at most 5 events per second
        int64_t now = esp_timer_get_time();
        bool dropped = false;
        // 1. Services whose state changed: notify all subscribers
        char *cur[GENA_NSVC];
        for (int v = 0; v < GENA_NSVC; v++) {
            cur[v] = s_body(v);
            bool changed = !last[v] || strcmp(last[v], cur[v]);
            free(last[v]);
            last[v] = cur[v];
            if (!changed) continue;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            for (int i = 0; i < MAX_SUBS; i++)
                if (s_subs[i].used && s_subs[i].svc == v) s_subs[i].pending = true;
            xSemaphoreGive(s_mx);
        }
        // 2. Notify one by one (sending happens outside the lock, so a slow network won't block the HTTP server)
        for (int i = 0; i < MAX_SUBS; i++) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            // Only delete when expired AND unreachable: some controllers don't renew on time but keep receiving callbacks (NetEase temporarily can't receive when the screen is locked or it's in the background)
            if (s_subs[i].used && s_subs[i].expire_us < now && s_subs[i].fails > 0) {
                ESP_LOGI(TAG, "Subscription expired and unreachable, removing %s", s_subs[i].sid);
                s_subs[i].used = false;
                dropped = true;
            }
            bool go = s_subs[i].used && s_subs[i].pending;
            sub_t copy = s_subs[i];
            if (go) s_subs[i].pending = false;
            xSemaphoreGive(s_mx);
            if (!go) continue;
            int code = send_notify(&copy, last[copy.svc]);
            bool ok = code == 200;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            if (s_subs[i].used && !strcmp(s_subs[i].sid, copy.sid)) {
                s_subs[i].seq++;  // SEQ increments whether or not it succeeded (required by the spec)
                s_subs[i].fails = ok ? 0 : s_subs[i].fails + 1;  // don't cancel when unreachable: the next push gets through once the phone is back in the foreground
                if (code == 412) {  // the controller explicitly no longer recognises this SID (pupnp / npupnp do the same)
                    s_subs[i].used = false;
                    dropped = true;
                }
            }
            xSemaphoreGive(s_mx);
            if (code == 412) ESP_LOGI(TAG, "%s:%d returned 412, no longer recognises %s, removing subscription", copy.host, copy.port, copy.sid);
            else if (!ok) ESP_LOGW(TAG, "Failed to push %s event to %s:%d (%d)", SVC_NAME[copy.svc], copy.host, copy.port, code);
        }
        if (dropped) subs_changed();
    }
}

void gena_resync(void)
{
    if (!s_mx) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++)
        if (s_subs[i].used) s_subs[i].pending = true;
    xSemaphoreGive(s_mx);
}

void gena_notify_now(void)
{
    if (!s_mx) return;
    char *body[GENA_NSVC];
    for (int v = 0; v < GENA_NSVC; v++) body[v] = s_body(v);
    for (int i = 0; i < MAX_SUBS; i++) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        bool go = s_subs[i].used;
        sub_t copy = s_subs[i];
        if (go) s_subs[i].seq++;  // reserve the sequence number first so a concurrent push from the GENA task can't reuse it
        xSemaphoreGive(s_mx);
        if (!go) continue;
        int code = send_notify(&copy, body[copy.svc]);
        ESP_LOGI(TAG, "Pushed %s event immediately to %s:%d: %d", SVC_NAME[copy.svc], copy.host, copy.port, code);
    }
    for (int v = 0; v < GENA_NSVC; v++) free(body[v]);
}

void gena_start(gena_body_fn body_fn)
{
    s_body = body_fn;
    s_mx = xSemaphoreCreateMutex();
    esp_timer_create_args_t ta = {.callback = save_timer_cb, .name = "gena_save"};
    esp_timer_create(&ta, &s_save_timer);
    subs_load();
    subs_changed();  // if we reboot abnormally after this, the saved SEQ is no longer accurate; mark as abnormal
    xTaskCreatePinnedToCoreWithCaps(gena_task, "gena", 4096, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);  // stack in PSRAM, saves internal RAM
}
