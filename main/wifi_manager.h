#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "wifi_history.h"

// WiFi初始化函数
esp_err_t wifi_init_softap(void);
esp_err_t wifi_init_ap(void);
esp_err_t wifi_scan_networks(wifi_ap_record_t **ap_records, uint16_t *ap_count);
esp_err_t wifi_smart_connect(void);
esp_err_t wifi_reset_connection_retry(void);

#endif // WIFI_MANAGER_H
