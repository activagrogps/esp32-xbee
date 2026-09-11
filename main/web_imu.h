#ifndef WEB_IMU_H
#define WEB_IMU_H

#include "esp_http_server.h"

/* Inregistreaza /imu, /imu.json si /imu.csv.
 *
 * Se apeleaza in web_server_start(), INAINTE de inregistrarea
 * wildcard-ului, altfel acesta prinde rutele astea inaintea lor.
 *
 * Necesita config.max_uri_handlers marit: nebkat inregistreaza deja
 * exact 8 handlere, cat aloca HTTPD_DEFAULT_CONFIG().
 */
void web_imu_register(httpd_handle_t server);

#endif /* WEB_IMU_H */