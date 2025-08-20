/*
 * @Description: WiFi历史连接管理头文件
 * @Author: ESP32 DataReader
 * @Date: 2025-01-19
 */

#ifndef WIFI_HISTORY_H
#define WIFI_HISTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi历史记录配置
#define WIFI_HISTORY_MAX_NETWORKS 10
#define WIFI_HISTORY_SSID_MAX_LEN 32
#define WIFI_HISTORY_PASSWORD_MAX_LEN 64

// WiFi网络信息结构
typedef struct {
    char ssid[WIFI_HISTORY_SSID_MAX_LEN];
    char password[WIFI_HISTORY_PASSWORD_MAX_LEN];
    uint8_t bssid[6];
    uint8_t channel;
    wifi_auth_mode_t authmode;
    int8_t rssi;
    uint32_t last_connected;  // 上次连接时间戳
    uint32_t connect_count;   // 连接次数
    uint8_t priority;         // 优先级 (0-255, 数值越大优先级越高)
    bool is_valid;            // 记录是否有效
} wifi_history_entry_t;

// WiFi历史管理结构
typedef struct {
    wifi_history_entry_t networks[WIFI_HISTORY_MAX_NETWORKS];
    uint8_t count;
    uint32_t next_timestamp;
} wifi_history_t;

/**
 * @brief 初始化WiFi历史管理
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_init(void);

/**
 * @brief 添加WiFi网络到历史记录
 * @param ssid WiFi网络名称
 * @param password WiFi密码
 * @param bssid MAC地址 (可选，传NULL忽略)
 * @param channel 信道
 * @param authmode 认证模式
 * @param rssi 信号强度
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_add_network(const char* ssid, const char* password, 
                                   const uint8_t* bssid, uint8_t channel,
                                   wifi_auth_mode_t authmode, int8_t rssi);

/**
 * @brief 更新网络连接成功记录
 * @param ssid WiFi网络名称
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_update_success(const char* ssid);

/**
 * @brief 获取历史网络列表
 * @param networks 输出的网络列表
 * @param count 输入：数组大小，输出：实际网络数量
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_get_networks(wifi_history_entry_t* networks, uint8_t* count);

/**
 * @brief 删除历史网络记录
 * @param ssid WiFi网络名称
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_remove_network(const char* ssid);

/**
 * @brief 清空所有历史记录
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_clear_all(void);

/**
 * @brief 扫描并尝试自动连接到历史网络
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_auto_connect(void);

/**
 * @brief 根据优先级获取最佳连接网络
 * @param available_networks 可用网络列表
 * @param network_count 网络数量
 * @param best_network 输出最佳网络信息
 * @return esp_err_t ESP_OK找到网络，ESP_ERR_NOT_FOUND未找到
 */
esp_err_t wifi_history_find_best_network(const wifi_ap_record_t* available_networks,
                                         uint16_t network_count,
                                         wifi_history_entry_t* best_network);

/**
 * @brief 保存历史记录到NVS
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_save(void);

/**
 * @brief 从NVS加载历史记录
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t wifi_history_load(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HISTORY_H */
