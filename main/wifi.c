/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/event_groups.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <lwip/dns.h>

#include "wifi.h"
#include "config.h"
#include "status_led.h"
#include "uart.h"

static const char *TAG = "WIFI";

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

// Network Interfaces for S3 (Replaces old tcpip_adapter)
esp_netif_t *netif_ap = NULL;
esp_netif_t *netif_sta = NULL;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    
    // Handle WiFi Events
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi Started. Connecting...");
                esp_wifi_connect();
                break;
            
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi Disconnected. Retrying...");
                xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }
            
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }
        }
    } 
    // Handle IP Events (Got IP)
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

void wifi_init() {
    // 1. Initialize the new network stack (ESP-IDF v5)
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Note: Default event loop is likely created in main.c, but we ensure it here just in case.
    // If main.c already calls it, this will just return ESP_ERR_INVALID_STATE which is fine to ignore.
    esp_event_loop_create_default(); 

    wifi_event_group = xEventGroupCreate();

    // 2. Create Network Interfaces
    // We create both AP and STA interfaces by default, then configure/destroy based on config
    netif_ap = esp_netif_create_default_wifi_ap();
    netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    // 4. Load Configuration
    bool ap_active = config_get_bool(KEY_CONFIG_WIFI_AP_ACTIVE);
    bool sta_active = config_get_bool(KEY_CONFIG_WIFI_STA_ACTIVE);

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (ap_active && sta_active) {
        mode = WIFI_MODE_APSTA;
        ESP_LOGI(TAG, "Mode: AP + STA");
    } else if (ap_active) {
        mode = WIFI_MODE_AP;
        // Destroy STA interface if not used to save memory
        if (netif_sta) { esp_netif_destroy(netif_sta); netif_sta = NULL; }
        ESP_LOGI(TAG, "Mode: AP");
    } else if (sta_active) {
        mode = WIFI_MODE_STA;
        // Destroy AP interface if not used
        if (netif_ap) { esp_netif_destroy(netif_ap); netif_ap = NULL; }
        ESP_LOGI(TAG, "Mode: STA");
    } else {
        ESP_LOGW(TAG, "WiFi disabled in config");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    // --- Access Point Configuration ---
    if (ap_active && netif_ap) {
        // Set Static IP for AP
        esp_netif_ip_info_t info_ap;
        info_ap.ip.addr = config_get_ip(KEY_CONFIG_WIFI_AP_GATEWAY);
        info_ap.gw.addr = config_get_ip(KEY_CONFIG_WIFI_AP_GATEWAY);
        // Calculate netmask from CIDR (e.g. 24 -> 255.255.255.0)
        uint8_t subnet = config_get_u8(KEY_CONFIG_WIFI_AP_SUBNET);
        uint32_t mask = (0xFFFFFFFF << (32 - subnet)) & 0xFFFFFFFF;
        // Swap bytes because ESP expects Big Endian for IP structs
        info_ap.netmask.addr = __builtin_bswap32(mask); 

        // Stop DHCP Server before changing IP
        esp_netif_dhcps_stop(netif_ap);
        esp_netif_set_ip_info(netif_ap, &info_ap);
        esp_netif_dhcps_start(netif_ap);

        wifi_config_t ap_config = {0};
        
        // SSID
        char *ssid = config_get_str_alloc(KEY_CONFIG_WIFI_AP_SSID);
        if (strlen(ssid) == 0) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP); // S3 specific MAC read
            sprintf((char *) ap_config.ap.ssid, "ESP32-XBee %02X%02X%02X", mac[3], mac[4], mac[5]);
        } else {
            strncpy((char *) ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
        }
        free(ssid);

        // Password
        char *password = config_get_str_alloc(KEY_CONFIG_WIFI_AP_PASSWORD);
        if (strlen(password) == 0) {
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
        } else {
            strncpy((char *) ap_config.ap.password, password, sizeof(ap_config.ap.password));
            ap_config.ap.authmode = config_get_u8(KEY_CONFIG_WIFI_AP_AUTH_MODE);
        }
        free(password);

        ap_config.ap.ssid_hidden = config_get_bool(KEY_CONFIG_WIFI_AP_SSID_HIDDEN) ? 1 : 0;
        ap_config.ap.max_connection = 4;
        ap_config.ap.beacon_interval = 100;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    // --- Station Configuration ---
    if (sta_active && netif_sta) {
        wifi_config_t sta_config = {0};

        char *ssid = config_get_str_alloc(KEY_CONFIG_WIFI_STA_SSID);
        char *password = config_get_str_alloc(KEY_CONFIG_WIFI_STA_PASSWORD);

        strncpy((char *) sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *) sta_config.sta.password, password, sizeof(sta_config.sta.password));

        free(ssid);
        free(password);

        if (config_get_bool(KEY_CONFIG_WIFI_STA_SCAN_MODE_ALL)) {
            sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        }

        // Static IP Setup
        if (config_get_bool(KEY_CONFIG_WIFI_STA_STATIC)) {
            // Stop DHCP Client
            esp_netif_dhcpc_stop(netif_sta);

            esp_netif_ip_info_t info_sta;
            info_sta.ip.addr = config_get_ip(KEY_CONFIG_WIFI_STA_IP);
            info_sta.gw.addr = config_get_ip(KEY_CONFIG_WIFI_STA_GATEWAY);
            
            uint8_t subnet = config_get_u8(KEY_CONFIG_WIFI_STA_SUBNET);
            uint32_t mask = (0xFFFFFFFF << (32 - subnet)) & 0xFFFFFFFF;
            info_sta.netmask.addr = __builtin_bswap32(mask);

            esp_netif_set_ip_info(netif_sta, &info_sta);

            // DNS
            esp_netif_dns_info_t dns_info;
            dns_info.ip.u_addr.ip4.addr = config_get_ip(KEY_CONFIG_WIFI_STA_DNS_A);
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(netif_sta, ESP_NETIF_DNS_MAIN, &dns_info);

            dns_info.ip.u_addr.ip4.addr = config_get_ip(KEY_CONFIG_WIFI_STA_DNS_B);
            esp_netif_set_dns_info(netif_sta, ESP_NETIF_DNS_BACKUP, &dns_info);
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wait_for_ip() {
    if (!config_get_bool(KEY_CONFIG_WIFI_STA_ACTIVE)) {
        return; // Don't wait if we aren't trying to connect
    }
    ESP_LOGI(TAG, "Waiting for IP address...");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "IP Address acquired.");
}

void net_init() {
    // This is often empty in new IDF if LwIP is initialized by esp_netif_init()
    // But we keep the function in case other modules call it.
}
