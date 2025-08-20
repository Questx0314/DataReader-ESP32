/*
 * @Description: WiFi历史连接管理实现
 * @Author: ESP32 DataReader
 * @Date: 2025-01-19
 */

#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "wifi_history.h"

static const char *TAG = "wifi_history";

// NVS存储键名
#define NVS_NAMESPACE "wifi_history"
#define NVS_KEY_NETWORKS "networks"
#define NVS_KEY_COUNT "count"
#define NVS_KEY_TIMESTAMP "timestamp"

// 全局WiFi历史管理结构
static wifi_history_t s_wifi_history = {0};
static bool s_initialized = false;

// 内部函数声明
static uint32_t get_timestamp(void);
static int find_network_index(const char* ssid);
static int find_empty_slot(void);
static int find_lowest_priority_slot(void);
static void sort_networks_by_priority(void);

/**
 * @brief 获取当前时间戳
 */
static uint32_t get_timestamp(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000); // 转换为秒
}

/**
 * @brief 查找网络在历史记录中的索引
 */
static int find_network_index(const char* ssid)
{
    if (!ssid) return -1;
    
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (s_wifi_history.networks[i].is_valid && 
            strcmp(s_wifi_history.networks[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 查找空闲槽位
 */
static int find_empty_slot(void)
{
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (!s_wifi_history.networks[i].is_valid) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 查找优先级最低的槽位
 */
static int find_lowest_priority_slot(void)
{
    int lowest_index = 0;
    uint8_t lowest_priority = s_wifi_history.networks[0].priority;
    uint32_t oldest_time = s_wifi_history.networks[0].last_connected;
    
    for (int i = 1; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (!s_wifi_history.networks[i].is_valid) continue;
        
        // 优先级更低，或优先级相同但时间更早
        if (s_wifi_history.networks[i].priority < lowest_priority ||
            (s_wifi_history.networks[i].priority == lowest_priority && 
             s_wifi_history.networks[i].last_connected < oldest_time)) {
            lowest_index = i;
            lowest_priority = s_wifi_history.networks[i].priority;
            oldest_time = s_wifi_history.networks[i].last_connected;
        }
    }
    return lowest_index;
}

/**
 * @brief 按优先级排序网络
 */
static void sort_networks_by_priority(void)
{
    // 简单的冒泡排序，按优先级降序排列
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS - 1; i++) {
        for (int j = 0; j < WIFI_HISTORY_MAX_NETWORKS - 1 - i; j++) {
            if (!s_wifi_history.networks[j].is_valid) continue;
            if (!s_wifi_history.networks[j + 1].is_valid) continue;
            
            // 比较优先级，优先级高的排在前面
            if (s_wifi_history.networks[j].priority < s_wifi_history.networks[j + 1].priority ||
                (s_wifi_history.networks[j].priority == s_wifi_history.networks[j + 1].priority &&
                 s_wifi_history.networks[j].last_connected < s_wifi_history.networks[j + 1].last_connected)) {
                
                // 交换位置
                wifi_history_entry_t temp = s_wifi_history.networks[j];
                s_wifi_history.networks[j] = s_wifi_history.networks[j + 1];
                s_wifi_history.networks[j + 1] = temp;
            }
        }
    }
}

esp_err_t wifi_history_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    
    // 初始化历史记录结构
    memset(&s_wifi_history, 0, sizeof(s_wifi_history));
    
    // 从NVS加载历史记录
    esp_err_t ret = wifi_history_load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "加载WiFi历史记录失败，使用空历史记录");
    }
    
    s_initialized = true;
    ESP_LOGI(TAG, "WiFi历史管理初始化完成");
    
    // 打印当前历史记录
    ESP_LOGI(TAG, "当前WiFi历史记录:");
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (s_wifi_history.networks[i].is_valid) {
            ESP_LOGI(TAG, "  [%d] %s (优先级: %u, 连接次数: %"PRIu32")", 
                     i, s_wifi_history.networks[i].ssid, 
                     (unsigned int)s_wifi_history.networks[i].priority,
                     s_wifi_history.networks[i].connect_count);
        }
    }
    
    return ESP_OK;
}

esp_err_t wifi_history_add_network(const char* ssid, const char* password, 
                                   const uint8_t* bssid, uint8_t channel,
                                   wifi_auth_mode_t authmode, int8_t rssi)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "无效的SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strlen(ssid) >= WIFI_HISTORY_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID长度超过限制");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (password && strlen(password) >= WIFI_HISTORY_PASSWORD_MAX_LEN) {
        ESP_LOGE(TAG, "密码长度超过限制");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 查找是否已存在
    int index = find_network_index(ssid);
    if (index >= 0) {
        // 更新现有记录
        wifi_history_entry_t* entry = &s_wifi_history.networks[index];
        
        // 更新密码（如果提供）
        if (password) {
            strlcpy(entry->password, password, sizeof(entry->password));
        }
        
        // 更新其他信息
        if (bssid) {
            memcpy(entry->bssid, bssid, 6);
        }
        entry->channel = channel;
        entry->authmode = authmode;
        entry->rssi = rssi;
        entry->last_connected = get_timestamp();
        
        ESP_LOGI(TAG, "更新WiFi网络: %s", ssid);
    } else {
        // 添加新记录
        index = find_empty_slot();
        if (index < 0) {
            // 没有空闲槽位，替换优先级最低的
            index = find_lowest_priority_slot();
            ESP_LOGW(TAG, "WiFi历史记录已满，替换网络: %s", s_wifi_history.networks[index].ssid);
        } else {
            s_wifi_history.count++;
        }
        
        wifi_history_entry_t* entry = &s_wifi_history.networks[index];
        memset(entry, 0, sizeof(wifi_history_entry_t));
        
        strlcpy(entry->ssid, ssid, sizeof(entry->ssid));
        if (password) {
            strlcpy(entry->password, password, sizeof(entry->password));
        }
        
        if (bssid) {
            memcpy(entry->bssid, bssid, 6);
        }
        entry->channel = channel;
        entry->authmode = authmode;
        entry->rssi = rssi;
        entry->last_connected = get_timestamp();
        entry->connect_count = 1;
        entry->priority = 100; // 默认优先级
        entry->is_valid = true;
        
        ESP_LOGI(TAG, "添加新WiFi网络: %s", ssid);
    }
    
    // 保存到NVS
    return wifi_history_save();
}

esp_err_t wifi_history_update_success(const char* ssid)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int index = find_network_index(ssid);
    if (index < 0) {
        ESP_LOGW(TAG, "未找到WiFi网络: %s", ssid);
        return ESP_ERR_NOT_FOUND;
    }
    
    wifi_history_entry_t* entry = &s_wifi_history.networks[index];
    entry->last_connected = get_timestamp();
    entry->connect_count++;
    
    // 动态调整优先级：连接次数越多，优先级越高
    if (entry->connect_count > 1) {
        entry->priority = 100 + (entry->connect_count - 1) * 10;
        // priority 是uint8_t类型，最大值就是255，不需要检查
        // if (entry->priority > 255) {
        //     entry->priority = 255;
        // }
    }
    
    ESP_LOGI(TAG, "更新WiFi连接成功: %s (连接次数: %"PRIu32", 优先级: %u)", 
             ssid, entry->connect_count, (unsigned int)entry->priority);
    
    // 重新排序
    sort_networks_by_priority();
    
    return wifi_history_save();
}

esp_err_t wifi_history_get_networks(wifi_history_entry_t* networks, uint8_t* count)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!networks || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t max_count = *count;
    *count = 0;
    
    // 先排序确保返回的是按优先级排列的
    sort_networks_by_priority();
    
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS && *count < max_count; i++) {
        if (s_wifi_history.networks[i].is_valid) {
            memcpy(&networks[*count], &s_wifi_history.networks[i], sizeof(wifi_history_entry_t));
            (*count)++;
        }
    }
    
    ESP_LOGI(TAG, "获取WiFi历史网络: %u 个", *count);
    return ESP_OK;
}

esp_err_t wifi_history_remove_network(const char* ssid)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int index = find_network_index(ssid);
    if (index < 0) {
        ESP_LOGW(TAG, "未找到WiFi网络: %s", ssid);
        return ESP_ERR_NOT_FOUND;
    }
    
    // 清除记录
    memset(&s_wifi_history.networks[index], 0, sizeof(wifi_history_entry_t));
    s_wifi_history.count--;
    
    ESP_LOGI(TAG, "删除WiFi网络: %s", ssid);
    
    return wifi_history_save();
}

esp_err_t wifi_history_clear_all(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    memset(&s_wifi_history, 0, sizeof(wifi_history_t));
    s_wifi_history.next_timestamp = 1;
    
    ESP_LOGI(TAG, "清空所有WiFi历史记录");
    
    return wifi_history_save();
}

esp_err_t wifi_history_find_best_network(const wifi_ap_record_t* available_networks,
                                         uint16_t network_count,
                                         wifi_history_entry_t* best_network)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!available_networks || !best_network || network_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 先排序历史网络
    sort_networks_by_priority();
    
    // 按优先级查找匹配的网络
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (!s_wifi_history.networks[i].is_valid) continue;
        
        // 在可用网络中查找匹配的SSID
        for (int j = 0; j < network_count; j++) {
            if (strcmp(s_wifi_history.networks[i].ssid, (char*)available_networks[j].ssid) == 0) {
                // 找到匹配的网络，检查信号强度是否可接受
                if (available_networks[j].rssi > -80) { // 信号强度阈值
                    memcpy(best_network, &s_wifi_history.networks[i], sizeof(wifi_history_entry_t));
                    ESP_LOGI(TAG, "找到最佳网络: %s (优先级: %u, RSSI: %d)", 
                             best_network->ssid, best_network->priority, available_networks[j].rssi);
                    return ESP_OK;
                }
            }
        }
    }
    
    ESP_LOGW(TAG, "未找到合适的历史网络");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_history_auto_connect(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "开始WiFi自动连接...");
    
    // 确保WiFi处于正确状态
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (!(mode & WIFI_MODE_STA)) {
        ESP_LOGE(TAG, "WiFi STA模式未启用");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查WiFi状态并等待合适的扫描时机
    wifi_ap_record_t ap_info;
    bool was_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    
    if (was_connected) {
        ESP_LOGI(TAG, "断开当前WiFi连接以进行扫描");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(2000)); // 等待断开完成
    }
    
    // 停止可能正在进行的扫描
    esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(500)); // 增加等待时间
    
    // 等待WiFi进入空闲状态
    int retry_count = 0;
    while (retry_count < 10) {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode & WIFI_MODE_STA) {
            // 检查STA状态
            wifi_ap_record_t temp_ap;
            if (esp_wifi_sta_get_ap_info(&temp_ap) != ESP_OK) {
                // STA未连接，可以扫描
                break;
            }
        }
        ESP_LOGD(TAG, "等待WiFi进入空闲状态... (%d/10)", retry_count + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        retry_count++;
    }
    
    if (retry_count >= 10) {
        ESP_LOGW(TAG, "WiFi状态等待超时，强制进行扫描");
    }
    
    // 扫描可用网络
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            }
        }
    };
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi扫描失败: %s", esp_err_to_name(ret));
        
        // 如果扫描失败，尝试重置WiFi状态后重试
        if (ret == ESP_ERR_WIFI_STATE) {
            ESP_LOGI(TAG, "WiFi状态错误，尝试重置后重试...");
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            ret = esp_wifi_scan_start(&scan_config, true);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "重试扫描仍然失败: %s", esp_err_to_name(ret));
                return ret;
            }
        } else {
            return ret;
        }
    }
    
    // 获取扫描结果
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    if (ap_count == 0) {
        ESP_LOGW(TAG, "未扫描到任何WiFi网络");
        return ESP_ERR_NOT_FOUND;
    }
    
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        ESP_LOGE(TAG, "分配内存失败");
        return ESP_ERR_NO_MEM;
    }
    
    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取扫描结果失败: %s", esp_err_to_name(ret));
        free(ap_records);
        return ret;
    }
    
    ESP_LOGI(TAG, "扫描到 %u 个WiFi网络", ap_count);
    
    // 打印扫描到的网络
    for (uint16_t i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "扫描结果[%d]: %s (RSSI: %d, BSSID: %02x:%02x:%02x:%02x:%02x:%02x)", 
                 i, ap_records[i].ssid, ap_records[i].rssi,
                 ap_records[i].bssid[0], ap_records[i].bssid[1], ap_records[i].bssid[2],
                 ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5]);
    }
    
    // 打印历史网络
    ESP_LOGI(TAG, "检查历史网络:");
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (s_wifi_history.networks[i].is_valid) {
            ESP_LOGI(TAG, "历史网络[%d]: %s (优先级: %u)", 
                     i, s_wifi_history.networks[i].ssid, (unsigned int)s_wifi_history.networks[i].priority);
        }
    }
    
    // 查找最佳网络（按信号强度排序）
    wifi_history_entry_t best_network;
    int best_rssi = -100;
    bool found_network = false;
    wifi_ap_record_t best_ap_record;
    
    // 遍历历史网络，找到信号最强的可用网络
    for (int i = 0; i < WIFI_HISTORY_MAX_NETWORKS; i++) {
        if (!s_wifi_history.networks[i].is_valid) continue;
        
        // 检查是否在历史记录中
        for (int j = 0; j < ap_count; j++) {
            if (strcmp((char*)ap_records[j].ssid, s_wifi_history.networks[i].ssid) == 0 && 
                ap_records[j].rssi > -85) { // 信号强度阈值
                
                ESP_LOGI(TAG, "发现历史网络: %s (RSSI: %d, 优先级: %d)", 
                         s_wifi_history.networks[i].ssid, ap_records[j].rssi, s_wifi_history.networks[i].priority);
                ESP_LOGI(TAG, "  扫描BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                         ap_records[j].bssid[0], ap_records[j].bssid[1], ap_records[j].bssid[2],
                         ap_records[j].bssid[3], ap_records[j].bssid[4], ap_records[j].bssid[5]);
                ESP_LOGI(TAG, "  历史BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                         s_wifi_history.networks[i].bssid[0], s_wifi_history.networks[i].bssid[1], s_wifi_history.networks[i].bssid[2],
                         s_wifi_history.networks[i].bssid[3], s_wifi_history.networks[i].bssid[4], s_wifi_history.networks[i].bssid[5]);
                
                if (ap_records[j].rssi > best_rssi || 
                    (ap_records[j].rssi == best_rssi && s_wifi_history.networks[i].priority > best_network.priority)) {
                    best_rssi = ap_records[j].rssi;
                    best_network = s_wifi_history.networks[i];
                    best_ap_record = ap_records[j];
                    found_network = true;
                    ESP_LOGI(TAG, "选择此网络作为最佳候选");
                }
                break;
            }
        }
    }
    
    free(ap_records);
    
    if (!found_network) {
        ESP_LOGW(TAG, "未找到合适的历史网络进行连接");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 配置并连接到最佳网络
    wifi_config_t wifi_config = {0};
    strlcpy((char*)wifi_config.sta.ssid, best_network.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, best_network.password, sizeof(wifi_config.sta.password));
    
    // 设置连接参数以提高稳定性
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN; // 允许更宽松的认证模式
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL; // 按信号强度排序
    
    // 设置信道
    wifi_config.sta.channel = best_ap_record.primary;
    ESP_LOGI(TAG, "设置连接信道: %d", wifi_config.sta.channel);
    
    // 检查历史记录中是否有有效的BSSID
    bool has_valid_historical_bssid = false;
    for (int k = 0; k < 6; k++) {
        if (best_network.bssid[k] != 0) {
            has_valid_historical_bssid = true;
            break;
        }
    }
    
    // 只有当历史记录中有有效BSSID且与扫描结果匹配时才使用BSSID连接
    if (has_valid_historical_bssid && memcmp(best_network.bssid, best_ap_record.bssid, 6) == 0) {
        memcpy(wifi_config.sta.bssid, best_ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
        ESP_LOGI(TAG, "✅ 使用匹配的BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                 wifi_config.sta.bssid[0], wifi_config.sta.bssid[1], wifi_config.sta.bssid[2],
                 wifi_config.sta.bssid[3], wifi_config.sta.bssid[4], wifi_config.sta.bssid[5]);
    } else {
        // 不使用BSSID，仅通过SSID连接
        wifi_config.sta.bssid_set = false;
        memset(wifi_config.sta.bssid, 0, 6);
        if (!has_valid_historical_bssid) {
            ESP_LOGI(TAG, "📡 历史记录无BSSID信息，使用SSID连接");
        } else {
            ESP_LOGW(TAG, "⚠️  BSSID不匹配，使用SSID连接");
            ESP_LOGW(TAG, "   历史BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                     best_network.bssid[0], best_network.bssid[1], best_network.bssid[2],
                     best_network.bssid[3], best_network.bssid[4], best_network.bssid[5]);
            ESP_LOGW(TAG, "   扫描BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                     best_ap_record.bssid[0], best_ap_record.bssid[1], best_ap_record.bssid[2],
                     best_ap_record.bssid[3], best_ap_record.bssid[4], best_ap_record.bssid[5]);
        }
    }
    
    ESP_LOGI(TAG, "尝试连接到信号最强的WiFi: %s (RSSI: %d)", best_network.ssid, best_rssi);
    
    ret = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置WiFi配置失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "🔄 开始连接到WiFi: %s", best_network.ssid);
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ WiFi连接启动失败: %s", esp_err_to_name(ret));
        
        // 如果指定BSSID连接失败，尝试不指定BSSID的连接
        if (wifi_config.sta.bssid_set) {
            ESP_LOGW(TAG, "🔄 指定BSSID连接失败，尝试不指定BSSID的连接");
            wifi_config.sta.bssid_set = false;
            memset(wifi_config.sta.bssid, 0, 6);
            
            ret = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
            if (ret == ESP_OK) {
                ret = esp_wifi_connect();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "✅ WiFi自动连接已启动（不指定BSSID）");
                    return ESP_OK;
                } else {
                    ESP_LOGE(TAG, "❌ 不指定BSSID连接也失败: %s", esp_err_to_name(ret));
                }
            }
        }
        return ret;
    }
    
    ESP_LOGI(TAG, "✅ WiFi连接命令已发送，等待连接结果...");
    return ESP_OK;
}

esp_err_t wifi_history_save(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 保存网络数据
    ret = nvs_set_blob(nvs_handle, NVS_KEY_NETWORKS, 
                       s_wifi_history.networks, 
                       sizeof(s_wifi_history.networks));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存网络数据失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 保存计数
    ret = nvs_set_u8(nvs_handle, NVS_KEY_COUNT, s_wifi_history.count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存网络计数失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 保存时间戳
    ret = nvs_set_u32(nvs_handle, NVS_KEY_TIMESTAMP, s_wifi_history.next_timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存时间戳失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "提交NVS失败: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi历史记录已保存");
    }
    
    return ret;
}

esp_err_t wifi_history_load(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "打开NVS失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 加载网络数据
    size_t required_size = sizeof(s_wifi_history.networks);
    ret = nvs_get_blob(nvs_handle, NVS_KEY_NETWORKS, 
                       s_wifi_history.networks, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "加载网络数据失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 加载计数
    ret = nvs_get_u8(nvs_handle, NVS_KEY_COUNT, &s_wifi_history.count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "加载网络计数失败: %s", esp_err_to_name(ret));
        s_wifi_history.count = 0;
    }
    
    // 加载时间戳
    ret = nvs_get_u32(nvs_handle, NVS_KEY_TIMESTAMP, &s_wifi_history.next_timestamp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "加载时间戳失败: %s", esp_err_to_name(ret));
        s_wifi_history.next_timestamp = 1;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "WiFi历史记录加载完成，共 %u 个网络", s_wifi_history.count);
    return ESP_OK;
}
