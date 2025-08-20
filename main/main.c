/*
 * @Author: jxingnian j_xingnian@163.com
 * @Date: 2025-01-01 11:27:58
 * @LastEditors: jxingnian j_xingnian@163.com
 * @LastEditTime: 2025-01-02 00:15:13
 * @FilePath: \EspWifiNetworkConfig\main\main.c
 * @Description: WiFi配网主程序
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "web_socket.h"
#include "usbd_cdc.h"

static const char *TAG = "main";

// 系统监控配置常量
#define SYSTEM_INIT_DELAY_MS     5000
#define SYSTEM_MONITOR_INTERVAL_MS 3000
#define WEBSOCKET_CONNECT_DELAY_MS 1000
#define SYSTEM_MONITOR_TASK_PRIORITY 2
#define SYSTEM_MONITOR_STACK_SIZE 4096

// 简单的状态结构
static struct {
    bool cdc_connected;
    bool ws_connected;
} system_status = {false, false};

// 安全发送WebSocket消息 (非阻塞方式)
static void notify_status_change(const char *event) {
    // 只有在WebSocket连接时才发送
    if (websocket_is_connected() && event != NULL) {
        char msg[32];
        snprintf(msg, sizeof(msg), "{\"event\":\"%s\"}", event);
        websocket_server_send_text(msg);
    }
}

// 初始化SPIFFS
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

// 初始化USB CDC Host
static esp_err_t init_usb_cdc(void)
{
    ESP_LOGI(TAG, "初始化USB CDC Host");
    
    // 初始化USB CDC Host，设置接收回调函数为web_socket.c中的usb_cdc_rx_callback
    esp_err_t ret = usbd_cdc_init(usb_cdc_rx_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化USB CDC Host失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "USB CDC Host初始化成功，等待设备连接...");
    return ESP_OK;
}

// 系统状态监控任务
static void system_monitor_task(void *pvParameters)
{
    // 等待系统初始化稳定
    vTaskDelay(pdMS_TO_TICKS(SYSTEM_INIT_DELAY_MS));
    ESP_LOGI(TAG, "系统监控任务开始运行");

    while (1) {
        // 检查CDC连接状态
        bool cdc_connected = usbd_cdc_is_connected();
        if (cdc_connected != system_status.cdc_connected) {
            ESP_LOGI(TAG, "CDC连接状态变化: %s", cdc_connected ? "已连接" : "已断开");
            system_status.cdc_connected = cdc_connected;
            
            // 状态变化时通知WebSocket (如果连接)
            if (system_status.ws_connected) {
                notify_status_change(cdc_connected ? "cdc_connect" : "cdc_disconnect");
            }
        }
        
        // 检查WebSocket连接状态
        bool ws_connected = websocket_is_connected();
        if (ws_connected != system_status.ws_connected) {
            ESP_LOGI(TAG, "WebSocket连接状态变化: %s", ws_connected ? "已连接" : "已断开");
            system_status.ws_connected = ws_connected;
            
            // 如果WebSocket新连接，并且CDC已连接，发送CDC状态
            if (ws_connected && system_status.cdc_connected) {
                // 等待连接稳定后再发送
                vTaskDelay(pdMS_TO_TICKS(WEBSOCKET_CONNECT_DELAY_MS));
                notify_status_change("cdc_connect");
            }
        }

        // 定期检查系统状态
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_MONITOR_INTERVAL_MS));
    }
}

void app_main(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化SPIFFS
    ESP_ERROR_CHECK(init_spiffs());

    // 初始化并启动WiFi AP (包含智能连接功能)
    ESP_LOGI(TAG, "Starting WiFi in AP mode with smart connect");
    ESP_ERROR_CHECK(wifi_init_softap());

    // 初始化USB CDC Host
    ESP_ERROR_CHECK(init_usb_cdc());

    // 启动HTTP服务器
    ESP_ERROR_CHECK(start_webserver());
    
    // 创建系统监控任务，设置更低的优先级
    xTaskCreate(system_monitor_task, "system_monitor", SYSTEM_MONITOR_STACK_SIZE, NULL, SYSTEM_MONITOR_TASK_PRIORITY, NULL);
    
    ESP_LOGI(TAG, "系统初始化完成");
}