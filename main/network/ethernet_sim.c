// QEMU simulator-only Ethernet (OpenCores virtual NIC), same interface as ethernet.c (W5500).
#include "sdkconfig.h"

#ifdef CONFIG_LIVELAB_SIM

#include "ethernet.h"

#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "eth_sim";
static esp_netif_t *s_netif;
static bool s_connected, s_link_up;

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (id == ETHERNET_EVENT_CONNECTED) {
    s_link_up = true;
  } else if (id == ETHERNET_EVENT_DISCONNECTED) {
    s_link_up = false;
    s_connected = false;
  }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
  ip_event_got_ip_t *e = data;
  ESP_LOGI(TAG, "Simulated NIC got IP: " IPSTR, IP2STR(&e->ip_info.ip));
  s_connected = true;
}

esp_err_t ethernet_init(void) {
  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    return ret;
  }
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    return ret;
  }
  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL);
  esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
  s_netif = esp_netif_new(&ncfg);
  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.autonego_timeout_ms = 100;
  esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(esp_eth_mac_new_openeth(&mac_cfg),
                                             esp_eth_phy_new_generic(&phy_cfg));
  esp_eth_handle_t eth;
  ESP_ERROR_CHECK(esp_eth_driver_install(&ecfg, &eth));
  ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)));
  ESP_ERROR_CHECK(esp_eth_start(eth));
  ESP_LOGI(TAG, "QEMU simulated NIC started");
  return ESP_OK;
}

bool ethernet_is_connected(void) { return s_connected; }
bool ethernet_is_link_up(void) { return s_link_up; }

esp_err_t ethernet_get_ip_str(char *ip_str, size_t len) {
  esp_netif_ip_info_t ip;
  if (!s_connected || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
    if (len) ip_str[0] = '\0';
    return ESP_FAIL;
  }
  snprintf(ip_str, len, IPSTR, IP2STR(&ip.ip));
  return ESP_OK;
}

void ethernet_get_mac_str(char *mac_str, size_t len) {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_ETH);
  snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void ethernet_set_hostname(const char *device_name) {
  if (s_netif) esp_netif_set_hostname(s_netif, device_name);
}

#endif // CONFIG_LIVELAB_SIM
