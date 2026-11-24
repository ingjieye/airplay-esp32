// LiveLab recovery app (factory partition).
//
// Started by the bootloader when otadata is empty: after repeated failed boots (see
// bootloader_components/livelab_boot_hooks), or when the main firmware's "Recovery mode"
// button selects the factory partition. It only does three things:
//   1. join the WiFi saved by the main firmware (NVS namespace "airplay"), or open the
//      hotspot "ESP32-Recovery" (192.168.4.1) if that fails;
//   2. accept a firmware image at POST /api/ota/update (same as the main firmware, so the
//      usual curl command works) and boot it;
//   3. boot an existing image again (POST /api/boot?part=ota_0|ota_1).
// It is installed once over UART and never updated over the air.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/rtc_cntl_reg.h"
#if CONFIG_RECOVERY_SIM
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#endif

#define TAG "recovery"
#define BOOT_MAGIC 0xB0070000u  // boot-failure counter, see livelab_boot_hooks
#define AP_SSID "ESP32-Recovery"

static EventGroupHandle_t s_ev;
#define EV_IP BIT0
static char s_ip[16] = "192.168.4.1";
static char s_msg[96];

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>LiveLab Recovery</title><style>"
    "body{font:15px/1.5 -apple-system,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#fff;color:#222}"
    "@media(prefers-color-scheme:dark){body{background:#121211;color:#ecebe6}}"
    "button{font:inherit;padding:8px 14px;margin:4px 4px 0 0;border-radius:8px;border:1px solid #fb923c;"
    "background:#fb923c;color:#fff}button.g{background:none;color:#fb923c}pre{white-space:pre-wrap;font-size:13px}"
    "progress{width:100%}</style></head><body><h2>LiveLab Recovery</h2>"
    "<p>The main firmware is not running. Upload a firmware image (airplay2-receiver.bin) to install it, "
    "or boot one of the existing images.</p>"
    "<input type=file id=f accept=.bin> <button id=u>Upload</button><progress id=p max=1 value=0></progress>"
    "<p><button class=g onclick=\"boot('ota_0')\">Boot ota_0</button><button class=g onclick=\"boot('ota_1')\">Boot ota_1</button></p>"
    "<pre id=i></pre><script>"
    "const $=id=>document.getElementById(id);"
    "fetch('/api/info').then(r=>r.text()).then(t=>$('i').textContent=t);"
    "$('u').onclick=()=>{const f=$('f').files[0];if(!f)return;const x=new XMLHttpRequest();"
    "x.upload.onprogress=e=>$('p').value=e.loaded/e.total;"
    "x.onload=()=>$('i').textContent=x.responseText;x.onerror=()=>$('i').textContent='Upload failed';"
    "x.open('POST','/api/ota/update');x.send(f);};"
    "function boot(p){fetch('/api/boot?part='+p,{method:'POST'}).then(r=>r.text()).then(t=>$('i').textContent=t);}"
    "</script></body></html>";

static void restart_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static esp_err_t page_handler(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, PAGE, sizeof PAGE - 1);
}

static void image_info(const esp_partition_t *p, char *out, size_t n)
{
    esp_app_desc_t d;
    esp_ota_img_states_t st;
    const char *state = "";
    if (esp_ota_get_state_partition(p, &st) == ESP_OK)
        state = st == ESP_OTA_IMG_VALID ? ", valid" : st == ESP_OTA_IMG_INVALID ? ", invalid"
              : st == ESP_OTA_IMG_ABORTED ? ", aborted" : st == ESP_OTA_IMG_PENDING_VERIFY ? ", pending" : "";
    if (esp_ota_get_partition_description(p, &d) == ESP_OK)
        snprintf(out, n, "%s: %s (%s %s%s)\n", p->label, d.version, d.date, d.time, state);
    else
        snprintf(out, n, "%s: no valid image\n", p->label);
}

static esp_err_t info_handler(httpd_req_t *r)
{
    char buf[512], line[128];
    snprintf(buf, sizeof buf, "Recovery mode, IP %s\n%s%s", s_ip, s_msg, s_msg[0] ? "\n" : "");
    for (int i = 0; i < 2; i++) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + i, NULL);
        if (!p) continue;
        image_info(p, line, sizeof line);
        strlcat(buf, line, sizeof buf);
    }
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, buf);
}

// 2 = no image, 1 = image marked invalid/aborted, 0 = otherwise
static int slot_badness(const esp_partition_t *p)
{
    esp_app_desc_t d;
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) return p ? 2 : -1;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK && (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) return 1;
    return 0;
}

static esp_err_t ota_handler(httpd_req_t *r)
{
    // Overwrite the slot least likely to hold a good image: empty, then invalid/aborted, else ota_0
    const esp_partition_t *p0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *p1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *dst = p0;
    if (p1 && slot_badness(p1) > slot_badness(p0)) dst = p1;
    if (!dst) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    ESP_LOGI(TAG, "Receiving %d bytes into %s", r->content_len, dst->label);
    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(dst, OTA_SIZE_UNKNOWN, &h);
    if (e != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
    static char buf[4096];
    int left = r->content_len;
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h);
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive or flash write failed");
        }
        left -= n;
    }
    if ((e = esp_ota_end(h)) != ESP_OK || (e = esp_ota_set_boot_partition(dst)) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
    ESP_LOGI(TAG, "Image written to %s, rebooting into it", dst->label);
    httpd_resp_sendstr(r, "Firmware update complete, rebooting now!\n");
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t boot_handler(httpd_req_t *r)
{
    char q[32] = {0}, part[16] = {0};
    httpd_req_get_url_query_str(r, q, sizeof q);
    httpd_query_key_value(q, "part", part, sizeof part);
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, part);
    esp_app_desc_t d;
    if (!p || p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY || esp_ota_get_partition_description(p, &d) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "No valid image in that partition");
    esp_err_t e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
    ESP_LOGI(TAG, "Booting %s", p->label);
    httpd_resp_sendstr(r, "Rebooting into the selected image\n");
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "ok");
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void start_http(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.recv_wait_timeout = 10;
    httpd_handle_t h;
    ESP_ERROR_CHECK(httpd_start(&h, &c));
    const httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_handler},
        {.uri = "/api/ota/update", .method = HTTP_POST, .handler = ota_handler},
        {.uri = "/api/boot", .method = HTTP_POST, .handler = boot_handler},
        {.uri = "/api/system/restart", .method = HTTP_POST, .handler = restart_handler},
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(h, &u[i]);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = data;
    snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "Got IP %s", s_ip);
    xEventGroupSetBits(s_ev, EV_IP);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();  // keep retrying in STA+AP mode
}

#if CONFIG_RECOVERY_SIM
static void net_start(void)
{
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *nif = esp_netif_new(&nc);
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.autonego_timeout_ms = 100;
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(esp_eth_mac_new_openeth(&mc), esp_eth_phy_new_generic(&pc));
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&ec, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(nif, esp_eth_new_netif_glue(eth)));
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL);
    ESP_ERROR_CHECK(esp_eth_start(eth));
    xEventGroupWaitBits(s_ev, EV_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
}
#else
static void net_start(void)
{
    char ssid[33] = {0}, pass[65] = {0};
    size_t l1 = sizeof ssid, l2 = sizeof pass;
    nvs_handle_t n;
    if (nvs_open("airplay", NVS_READONLY, &n) == ESP_OK) {  // written by the main firmware (settings.c)
        nvs_get_str(n, "wifi_ssid", ssid, &l1);
        nvs_get_str(n, "wifi_pass", pass, &l2);
        nvs_close(n);
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ssid[0]) {
        wifi_config_t sc = {0};
        strlcpy((char *)sc.sta.ssid, ssid, sizeof sc.sta.ssid);
        strlcpy((char *)sc.sta.password, pass, sizeof sc.sta.password);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sc);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_connect();
        ESP_LOGI(TAG, "Joining \"%s\"", ssid);
        if (xEventGroupWaitBits(s_ev, EV_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000)) & EV_IP) return;
        ESP_LOGW(TAG, "Could not join \"%s\", also opening the hotspot", ssid);
    }
    // Hotspot (open network) at 192.168.4.1; keeps trying the saved network in the background
    wifi_config_t ac = {.ap = {.ssid = AP_SSID, .ssid_len = sizeof AP_SSID - 1, .channel = 1, .max_connection = 2,
                               .authmode = WIFI_AUTH_OPEN}};
    esp_wifi_set_mode(ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    esp_wifi_start();
    if (ssid[0]) esp_wifi_connect();
    snprintf(s_msg, sizeof s_msg, "Hotspot \"%s\" is open (192.168.4.1)", AP_SSID);
    ESP_LOGI(TAG, "%s", s_msg);
}
#endif

static void healthy_cb(void *arg) { REG_WRITE(RTC_CNTL_STORE0_REG, BOOT_MAGIC); }

void app_main(void)
{
    ESP_LOGW(TAG, "LiveLab recovery mode");
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "NVS init failed (%s), saved WiFi unavailable", esp_err_to_name(e));
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    net_start();
    start_http();
    ESP_LOGW(TAG, "Recovery ready: http://%s/", s_ip);
    // Clear the boot-failure counter after a while, so a crashing recovery app keeps
    // returning here rather than counting up forever (it cannot go anywhere else anyway)
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = {.callback = healthy_cb, .name = "healthy"};
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 30 * 1000000LL);
}
