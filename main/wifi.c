#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_types.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>

#include "wifi.h"
#include "config.h"
#include "status_led.h"
#include "uart.h"

static const char *TAG = "WIFI";
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

esp_netif_t *netif_ap = NULL;
esp_netif_t *netif_sta = NULL;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();
    wifi_event_group = xEventGroupCreate();

    netif_ap = esp_netif_create_default_wifi_ap();
    netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    bool ap_active = config_get_bool(KEY_CONFIG_WIFI_AP_ACTIVE);
    bool sta_active = config_get_bool(KEY_CONFIG_WIFI_STA_ACTIVE);

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (ap_active && sta_active) mode = WIFI_MODE_APSTA;
    else if (ap_active) mode = WIFI_MODE_AP;
    else if (sta_active) mode = WIFI_MODE_STA;

    if (mode == WIFI_MODE_NULL) return;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    // --- AP CONFIG ---
    if (ap_active && netif_ap) {
        esp_netif_ip_info_t info_ap;
        // Use helper to set IP safely (avoids "no member" error)
        ip4_addr_set_u32((ip4_addr_t*)&info_ap.ip, config_get_ip(KEY_CONFIG_WIFI_AP_GATEWAY));
        ip4_addr_set_u32((ip4_addr_t*)&info_ap.gw, config_get_ip(KEY_CONFIG_WIFI_AP_GATEWAY));
        
        uint8_t subnet = config_get_u8(KEY_CONFIG_WIFI_AP_SUBNET);
        uint32_t mask = (0xFFFFFFFF << (32 - subnet)) & 0xFFFFFFFF;
        ip4_addr_set_u32((ip4_addr_t*)&info_ap.netmask, __builtin_bswap32(mask));

        esp_netif_dhcps_stop(netif_ap);
        esp_netif_set_ip_info(netif_ap, &info_ap);
        esp_netif_dhcps_start(netif_ap);

        wifi_config_t ap_config = {0};
        char *ssid = config_get_str_alloc(KEY_CONFIG_WIFI_AP_SSID);
        if (strlen(ssid) == 0) {
             uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
             sprintf((char *)ap_config.ap.ssid, "ESP32-XBee %02X%02X%02X", mac[3], mac[4], mac[5]);
        } else strncpy((char *)ap_config.ap.ssid, ssid, 32);
        free(ssid);

        ap_config.ap.max_connection = 4;
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    // --- STA CONFIG ---
    if (sta_active && netif_sta) {
        wifi_config_t sta_config = {0};
        char *ssid = config_get_str_alloc(KEY_CONFIG_WIFI_STA_SSID);
        char *pass = config_get_str_alloc(KEY_CONFIG_WIFI_STA_PASSWORD);
        strncpy((char *)sta_config.sta.ssid, ssid, 32);
        strncpy((char *)sta_config.sta.password, pass, 64);
        free(ssid); free(pass);

        if (config_get_bool(KEY_CONFIG_WIFI_STA_STATIC)) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_ip_info_t info_sta;
            ip4_addr_set_u32((ip4_addr_t*)&info_sta.ip, config_get_ip(KEY_CONFIG_WIFI_STA_IP));
            ip4_addr_set_u32((ip4_addr_t*)&info_sta.gw, config_get_ip(KEY_CONFIG_WIFI_STA_GATEWAY));
            uint8_t subnet = config_get_u8(KEY_CONFIG_WIFI_STA_SUBNET);
            uint32_t mask = (0xFFFFFFFF << (32 - subnet)) & 0xFFFFFFFF;
            ip4_addr_set_u32((ip4_addr_t*)&info_sta.netmask, __builtin_bswap32(mask));
            esp_netif_set_ip_info(netif_sta, &info_sta);
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wait_for_ip() {
    if (config_get_bool(KEY_CONFIG_WIFI_STA_ACTIVE)) {
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    }
}
void net_init() {}
