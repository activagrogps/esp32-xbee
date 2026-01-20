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

#include <web_server.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <cJSON.h>
#include <esp_system.h>
#include <esp_chip_info.h> // Required for S3 chip info
#include <esp_wifi.h>
#include <esp_netif.h>     // Required for new Network Stack
#include <nvs_flash.h>
#include <esp_ota_ops.h>
#include <sys/param.h>
#include <esp_vfs_spiffs.h>
#include <config.h>
#include <tasks.h>
#include <stream_stats.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "WEB_SERVER";

// Helper to decode URL-encoded strings
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

// -----------------------------------------------------------------------------------
// API HANDLERS
// -----------------------------------------------------------------------------------

static esp_err_t system_info_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *app_desc = esp_ota_get_app_description();
    cJSON_AddStringToObject(root, "version", app_desc->version);
    cJSON_AddStringToObject(root, "compile_time", app_desc->time);
    cJSON_AddStringToObject(root, "compile_date", app_desc->date);
    cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);

    uint8_t mac[6];
    char mac_str[18];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(mac_str, MACSTR, MAC2STR(mac));
    cJSON_AddStringToObject(root, "mac", mac_str);

    // --- FIX: Use esp_netif instead of tcpip_adapter ---
    esp_netif_ip_info_t info;
    esp_netif_t *netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif_sta) {
        esp_netif_get_ip_info(netif_sta, &info);
        cJSON_AddStringToObject(root, "ip", ip4addr_ntoa((const ip4_addr_t*)&info.ip));
        cJSON_AddStringToObject(root, "netmask", ip4addr_ntoa((const ip4_addr_t*)&info.netmask));
        cJSON_AddStringToObject(root, "gateway", ip4addr_ntoa((const ip4_addr_t*)&info.gw));
    } else {
        cJSON_AddStringToObject(root, "ip", "0.0.0.0");
    }

    esp_netif_t *netif_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif_ap) {
        esp_netif_get_ip_info(netif_ap, &info);
        cJSON_AddStringToObject(root, "ap_ip", ip4addr_ntoa((const ip4_addr_t*)&info.ip));
    } else {
        cJSON_AddStringToObject(root, "ap_ip", "0.0.0.0");
    }
    // --- END FIX ---

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddNumberToObject(root, "chip_rev", chip_info.revision);
    cJSON_AddNumberToObject(root, "chip_cores", chip_info.cores);
    
    // Convert features bitmap to string (simplified for JSON)
    cJSON *features = cJSON_CreateArray();
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH) cJSON_AddItemToArray(features, cJSON_CreateString("EMB_FLASH"));
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN) cJSON_AddItemToArray(features, cJSON_CreateString("WIFI_BGN"));
    if (chip_info.features & CHIP_FEATURE_BLE) cJSON_AddItemToArray(features, cJSON_CreateString("BLE"));
    if (chip_info.features & CHIP_FEATURE_BT) cJSON_AddItemToArray(features, cJSON_CreateString("BT"));
    cJSON_AddItemToObject(root, "chip_features", features);

    const char *response = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    free((void *) response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    
    int count;
    const config_item_t *items = config_items_get(&count);
    
    for (int i = 0; i < count; i++) {
        const config_item_t *item = &items[i];
        
        // Skip secrets
        if (item->secret) continue;

        switch (item->type) {
            case CONFIG_ITEM_TYPE_BOOL:
                cJSON_AddBoolToObject(root, item->key, config_get_bool(item->key));
                break;
            case CONFIG_ITEM_TYPE_INT8:
                cJSON_AddNumberToObject(root, item->key, config_get_i8(item->key));
                break;
            case CONFIG_ITEM_TYPE_INT16:
                cJSON_AddNumberToObject(root, item->key, config_get_i16(item->key));
                break;
            case CONFIG_ITEM_TYPE_INT32:
                cJSON_AddNumberToObject(root, item->key, config_get_i32(item->key));
                break;
            case CONFIG_ITEM_TYPE_INT64:
                cJSON_AddNumberToObject(root, item->key, config_get_i64(item->key));
                break;
            case CONFIG_ITEM_TYPE_UINT8:
                cJSON_AddNumberToObject(root, item->key, config_get_u8(item->key));
                break;
            case CONFIG_ITEM_TYPE_UINT16:
                cJSON_AddNumberToObject(root, item->key, config_get_u16(item->key));
                break;
            case CONFIG_ITEM_TYPE_UINT32:
                cJSON_AddNumberToObject(root, item->key, config_get_u32(item->key));
                break;
            case CONFIG_ITEM_TYPE_UINT64:
                cJSON_AddNumberToObject(root, item->key, config_get_u64(item->key));
                break;
            case CONFIG_ITEM_TYPE_STRING: {
                char *str = config_get_str_alloc(item->key);
                cJSON_AddStringToObject(root, item->key, str);
                free(str);
                break;
            }
            case CONFIG_ITEM_TYPE_IP: {
                uint32_t ip = config_get_u32(item->key);
                // Convert uint32 (Network Byte Order) to string
                struct in_addr addr;
                addr.s_addr = ip;
                cJSON_AddStringToObject(root, item->key, inet_ntoa(addr));
                break;
            }
            case CONFIG_ITEM_TYPE_COLOR:
                cJSON_AddNumberToObject(root, item->key, config_get_color(item->key).rgba);
                break;
            default:
                break;
        }
    }

    const char *response = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    free((void *) response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = malloc(total_len + 1);
    int received = 0;
    
    if (total_len >= 10240) { // Limit payload size
        httpd_resp_send_500(req);
        free(buf);
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int count;
    const config_item_t *items = config_items_get(&count);

    for (int i = 0; i < count; i++) {
        const config_item_t *item = &items[i];
        cJSON *json_item = cJSON_GetObjectItem(root, item->key);
        
        if (!json_item) continue;

        switch (item->type) {
            case CONFIG_ITEM_TYPE_BOOL:
                if (cJSON_IsBool(json_item)) config_set_bool(item->key, cJSON_IsTrue(json_item));
                break;
            case CONFIG_ITEM_TYPE_INT8:
                if (cJSON_IsNumber(json_item)) config_set_i8(item->key, (int8_t)json_item->valueint);
                break;
            case CONFIG_ITEM_TYPE_INT16:
                if (cJSON_IsNumber(json_item)) config_set_i16(item->key, (int16_t)json_item->valueint);
                break;
            case CONFIG_ITEM_TYPE_INT32:
                if (cJSON_IsNumber(json_item)) config_set_i32(item->key, (int32_t)json_item->valueint);
                break;
            case CONFIG_ITEM_TYPE_UINT8:
                if (cJSON_IsNumber(json_item)) config_set_u8(item->key, (uint8_t)json_item->valueint);
                break;
            case CONFIG_ITEM_TYPE_UINT16:
                if (cJSON_IsNumber(json_item)) config_set_u16(item->key, (uint16_t)json_item->valueint);
                break;
            case CONFIG_ITEM_TYPE_UINT32:
                if (cJSON_IsNumber(json_item)) config_set_u32(item->key, (uint32_t)json_item->valueint);
                break;
            case CONFIG_ITEM_TYPE_STRING:
                if (cJSON_IsString(json_item)) config_set_str(item->key, json_item->valuestring);
                break;
            case CONFIG_ITEM_TYPE_IP:
                if (cJSON_IsString(json_item)) {
                    uint32_t ip = ipaddr_addr(json_item->valuestring);
                    config_set_u32(item->key, ip);
                }
                break;
            case CONFIG_ITEM_TYPE_COLOR:
                if (cJSON_IsNumber(json_item)) {
                     config_color_t c; c.rgba = json_item->valueint;
                     config_set_color(item->key, c);
                }
                break;
            default: break;
        }
    }
    
    cJSON_Delete(root);
    config_commit();

    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t restart_post_handler(httpd_req_t *req) {
    config_restart();
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    // Serve index.html from SPIFFS
    FILE *f = fopen("/www/index.html", "r");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// -----------------------------------------------------------------------------------
// INITIALIZATION
// -----------------------------------------------------------------------------------

void web_server_init() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192; // Increased stack for S3

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        
        httpd_uri_t system_info_get = {
            .uri       = "/api/system/info",
            .method    = HTTP_GET,
            .handler   = system_info_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &system_info_get);

        httpd_uri_t config_get = {
            .uri       = "/api/config",
            .method    = HTTP_GET,
            .handler   = config_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &config_get);

        httpd_uri_t config_post = {
            .uri       = "/api/config",
            .method    = HTTP_POST,
            .handler   = config_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &config_post);

        httpd_uri_t restart_post = {
            .uri       = "/api/system/restart",
            .method    = HTTP_POST,
            .handler   = restart_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &restart_post);

        httpd_uri_t index_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &index_uri);
    }
}
