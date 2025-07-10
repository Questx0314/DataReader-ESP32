#include "esp_mdns.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "esp_mdns";

void esp_mdns_start(void)
{
    ESP_LOGI(TAG, "Initializing mDNS");

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp32"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 mDNS Device"));

    // 注册 HTTP 服务，默认端口 80
    ESP_ERROR_CHECK(mdns_service_add("ESP Web", "_http", "_tcp", 80, NULL, 0));

    ESP_LOGI(TAG, "mDNS started, access via http://esp32.local/");
}
