#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "wifi_manager.h"
#include "wifi_history.h"

#include "esp_mdns.h"  // mDNS支持

// WiFi配置参数
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID        // WiFi名称
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD    // WiFi密码
#define EXAMPLE_ESP_WIFI_CHANNEL   CONFIG_ESP_WIFI_CHANNEL     // WiFi信道
#define EXAMPLE_MAX_STA_CONN       CONFIG_ESP_MAX_STA_CONN     // 最大连接数

static const char *TAG = "wifi_manager";  // 日志标签
static int s_retry_num = 0;
static bool mdns_initialized = false;
static bool history_initialized = false;

#define MAX_RETRY_COUNT 5

// 函数声明
static void wifi_auto_connect_task(void *pvParameters);

// WiFi事件处理函数
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED:
                wifi_event_ap_staconnected_t* ap_event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "设备 "MACSTR" 已连接, AID=%d",
                         MAC2STR(ap_event->mac), ap_event->aid);
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                wifi_event_ap_stadisconnected_t* ap_disc_event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "设备 "MACSTR" 已断开连接, AID=%d",
                         MAC2STR(ap_disc_event->mac), ap_disc_event->aid);
                break;
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WIFI_EVENT_STA_START，STA已启动，等待连接命令...");
                // 不在这里调用esp_wifi_connect()，由自动连接任务或手动连接处理
                break;
            case WIFI_EVENT_STA_CONNECTED:
                {
                    wifi_event_sta_connected_t* connected_event = (wifi_event_sta_connected_t*) event_data;
                    ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED，已连接到AP: %s, 信道: %d, BSSID: %02x:%02x:%02x:%02x:%02x:%02x", 
                             connected_event->ssid, connected_event->channel,
                             connected_event->bssid[0], connected_event->bssid[1], connected_event->bssid[2],
                             connected_event->bssid[3], connected_event->bssid[4], connected_event->bssid[5]);
                    s_retry_num = 0; // 重置重试计数
                    
                    // 获取连接的AP信息并更新历史记录
                    wifi_ap_record_t ap_info;
                    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                        wifi_config_t wifi_config;
                        if (esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config) == ESP_OK) {
                            wifi_history_update_success((char*)wifi_config.sta.ssid);
                        }
                    }
                }
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
                
                // 详细断开原因分析
                const char* reason_str = "Unknown";
                switch(event->reason) {
                    case WIFI_REASON_UNSPECIFIED: reason_str = "Unspecified"; break;
                    case WIFI_REASON_AUTH_EXPIRE: reason_str = "Auth Expired"; break;
                    case WIFI_REASON_AUTH_LEAVE: reason_str = "Auth Leave"; break;
                    case WIFI_REASON_ASSOC_EXPIRE: reason_str = "Assoc Expired"; break;
                    case WIFI_REASON_ASSOC_TOOMANY: reason_str = "Too Many Assoc"; break;
                    case WIFI_REASON_NOT_AUTHED: reason_str = "Not Authenticated"; break;
                    case WIFI_REASON_NOT_ASSOCED: reason_str = "Not Associated"; break;
                    case WIFI_REASON_ASSOC_LEAVE: reason_str = "Assoc Leave"; break;
                    case WIFI_REASON_ASSOC_NOT_AUTHED: reason_str = "Assoc Not Auth"; break;
                    case WIFI_REASON_DISASSOC_PWRCAP_BAD: reason_str = "Bad Power Cap"; break;
                    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: reason_str = "Bad Sup Channel"; break;
                    case WIFI_REASON_IE_INVALID: reason_str = "Invalid IE"; break;
                    case WIFI_REASON_MIC_FAILURE: reason_str = "MIC Failure"; break;
                    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: reason_str = "4-Way Handshake Timeout"; break;
                    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: reason_str = "Group Key Timeout"; break;
                    case WIFI_REASON_IE_IN_4WAY_DIFFERS: reason_str = "IE Differs in 4-Way"; break;
                    case WIFI_REASON_GROUP_CIPHER_INVALID: reason_str = "Invalid Group Cipher"; break;
                    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: reason_str = "Invalid Pairwise Cipher"; break;
                    case WIFI_REASON_AKMP_INVALID: reason_str = "Invalid AKMP"; break;
                    case WIFI_REASON_UNSUPP_RSN_IE_VERSION: reason_str = "Unsupported RSN IE"; break;
                    case WIFI_REASON_INVALID_RSN_IE_CAP: reason_str = "Invalid RSN IE Cap"; break;
                    case WIFI_REASON_802_1X_AUTH_FAILED: reason_str = "802.1X Auth Failed"; break;
                    case WIFI_REASON_CIPHER_SUITE_REJECTED: reason_str = "Cipher Suite Rejected"; break;
                    case WIFI_REASON_BEACON_TIMEOUT: reason_str = "Beacon Timeout"; break;
                    case WIFI_REASON_NO_AP_FOUND: reason_str = "No AP Found"; break;
                    case WIFI_REASON_AUTH_FAIL: reason_str = "Auth Failed"; break;
                    case WIFI_REASON_ASSOC_FAIL: reason_str = "Assoc Failed"; break;
                    case WIFI_REASON_HANDSHAKE_TIMEOUT: reason_str = "Handshake Timeout"; break;
                    default: reason_str = "Other"; break;
                }
                
                ESP_LOGW(TAG, "WiFi断开连接，原因:%d (%s)", event->reason, reason_str);
                
                // 对于特定错误，尝试不指定BSSID的连接
                if (event->reason == WIFI_REASON_NO_AP_FOUND) {
                    ESP_LOGW(TAG, "无法找到AP，可能是BSSID问题，尝试不指定BSSID连接");
                    
                    // 获取当前配置并移除BSSID限制
                    wifi_config_t current_config;
                    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &current_config) == ESP_OK) {
                        if (current_config.sta.bssid_set && s_retry_num < 2) {
                            current_config.sta.bssid_set = false;
                            memset(current_config.sta.bssid, 0, 6);
                            esp_wifi_set_config(ESP_IF_WIFI_STA, &current_config);
                            ESP_LOGI(TAG, "移除BSSID限制后重试连接");
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            esp_wifi_connect();
                            s_retry_num++;
                            break;
                        }
                    }
                    s_retry_num = MAX_RETRY_COUNT; // 停止重试，等待自动连接任务
                } else if (event->reason == WIFI_REASON_AUTH_FAIL ||
                          event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                    ESP_LOGW(TAG, "认证失败，等待自动连接任务处理");
                    s_retry_num = MAX_RETRY_COUNT; // 停止重试
                } else if (s_retry_num < MAX_RETRY_COUNT) {
                    ESP_LOGI(TAG, "重试连接到AP... (%d/%d)", s_retry_num + 1, MAX_RETRY_COUNT);
                    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒后重试
                    esp_wifi_connect();
                    s_retry_num++;
                } else {
                    ESP_LOGW(TAG, "WiFi连接失败，达到最大重试次数");
                    // 保存当前状态到NVS
                    nvs_handle_t nvs_handle;
                    esp_err_t err = nvs_open("wifi_state", NVS_READWRITE, &nvs_handle);
                    if (err == ESP_OK) {
                        nvs_set_u8(nvs_handle, "connection_failed", 1);
                        nvs_commit(nvs_handle);
                        nvs_close(nvs_handle);
                    }
                }
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) 
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "✅ 成功获取IP地址:" IPSTR ", 网关:" IPSTR ", 子网掩码:" IPSTR, 
                     IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.netmask));
            s_retry_num = 0; // 重置重试计数
            
            // 保存成功状态到NVS
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open("wifi_state", NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK) {
                nvs_set_u8(nvs_handle, "connection_failed", 0);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
            }
            
            // ✅ 启动 mDNS（只执行一次）
            if (!mdns_initialized)
            {
                esp_mdns_start();
                mdns_initialized = true;
            }
            
            ESP_LOGI(TAG, "🌐 WiFi连接完全成功，网络可用");
        }
    }
}

// 初始化WiFi软AP
esp_err_t wifi_init_softap(void)
{
    esp_err_t ret = ESP_OK;
    ESP_ERROR_CHECK(esp_netif_init());  // 初始化底层TCP/IP堆栈
    ESP_ERROR_CHECK(esp_event_loop_create_default());  // 创建默认事件循环
    esp_netif_create_default_wifi_ap();  // 创建默认WIFI AP
    esp_netif_create_default_wifi_sta(); // 创建默认WIFI STA

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // 使用默认WiFi初始化配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));  // 初始化WiFi

    // 注册WiFi事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      NULL));

    // 配置AP参数
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // 如果没有设置密码，使用开放认证
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // 设置WiFi为APSTA模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    // 设置AP配置
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    // 尝试从NVS读取保存的WiFi配置
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
    uint8_t connection_failed = 0;
    
    if (err == ESP_OK) {
        wifi_config_t sta_config;
        size_t size = sizeof(wifi_config_t);
        err = nvs_get_blob(nvs_handle, "sta_config", &sta_config, &size);
        if (err == ESP_OK) {
            // 检查是否之前连接失败
            nvs_get_u8(nvs_handle, "connection_failed", &connection_failed);
            
            if (!connection_failed) {
                ESP_LOGI(TAG, "找到已保存的WiFi配置，SSID: %s", sta_config.sta.ssid);
                ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
            } else {
                ESP_LOGW(TAG, "上次WiFi连接失败，跳过自动连接");
            }
        }
        nvs_close(nvs_handle);
    }

    // 初始化WiFi历史管理
    if (!history_initialized) {
        ret = wifi_history_init();
        if (ret == ESP_OK) {
            history_initialized = true;
            ESP_LOGI(TAG, "WiFi历史管理初始化成功");
        } else {
            ESP_LOGW(TAG, "WiFi历史管理初始化失败: %s", esp_err_to_name(ret));
        }
    }

    // 启动WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成. SSID:%s 密码:%s 信道:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
    
    // 创建自动连接任务
    if (history_initialized) {
        ESP_LOGI(TAG, "创建WiFi自动连接任务...");
        xTaskCreate(wifi_auto_connect_task, "wifi_auto_connect", 4096, NULL, 3, NULL);
    }
    
    return ESP_OK;
}
esp_err_t wifi_reset_connection_retry(void)
{
    // 重置重试计数
    s_retry_num = 0;
    
    // 重置NVS中的连接失败标志
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_state", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, "connection_failed", 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    
    return ESP_OK;
}
#define DEFAULT_SCAN_LIST_SIZE 10  // 默认扫描列表大小

// 扫描周围WiFi网络
esp_err_t wifi_scan_networks(wifi_ap_record_t **ap_records, uint16_t *ap_count)
{
    esp_err_t ret;
    uint16_t number = DEFAULT_SCAN_LIST_SIZE;

    // 分配内存用于存储扫描结果
    *ap_records = malloc(DEFAULT_SCAN_LIST_SIZE * sizeof(wifi_ap_record_t));
    if (*ap_records == NULL) {
        ESP_LOGE(TAG, "为扫描结果分配内存失败");
        return ESP_ERR_NO_MEM;
    }

    // 配置扫描参数
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 150,
    };

    // 开始扫描
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "开始扫描失败");
        free(*ap_records);
        *ap_records = NULL;
        return ret;
    }

    // 获取扫描结果
    ret = esp_wifi_scan_get_ap_records(&number, *ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取扫描结果失败");
        free(*ap_records);
        *ap_records = NULL;
        return ret;
    }

    // 获取找到的AP数量
    ret = esp_wifi_scan_get_ap_num(ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取扫描到的AP数量失败");
        free(*ap_records);
        *ap_records = NULL;
        return ret;
    }

    // 限制AP数量不超过默认扫描列表大小
    if (*ap_count > DEFAULT_SCAN_LIST_SIZE) {
        *ap_count = DEFAULT_SCAN_LIST_SIZE;
    }

    // 打印扫描结果
    ESP_LOGI(TAG, "发现 %d 个接入点:", *ap_count);
    for (int i = 0; i < *ap_count; i++) {
        ESP_LOGI(TAG, "SSID: %s, 信号强度: %d", (*ap_records)[i].ssid, (*ap_records)[i].rssi);
    }

    return ESP_OK;
}

// WiFi自动连接任务
static void wifi_auto_connect_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi自动连接任务启动");
    
    // 等待WiFi初始化完成和系统稳定
    vTaskDelay(pdMS_TO_TICKS(10000)); // 增加等待时间到10秒
    
    int failed_attempts = 0;
    
    while (1) {
        // 检查WiFi连接状态
        wifi_ap_record_t ap_info;
        esp_netif_ip_info_t ip_info;
        bool is_connected = false;
        
        // 检查是否已获取IP（更可靠的连接状态检查）
        if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                is_connected = true;
            }
        }
        
        // 如果已连接，检查AP信息
        if (is_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi已连接到: %s, IP: " IPSTR, ap_info.ssid, IP2STR(&ip_info.ip));
            failed_attempts = 0; // 重置失败计数
            vTaskDelay(pdMS_TO_TICKS(30000)); // 已连接，30秒后再检查
            continue;
        }
        
        // 检查WiFi状态，避免在连接过程中重复尝试
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
            // 检查是否正在连接中
            esp_err_t wifi_state = esp_wifi_connect(); // 测试连接状态
            if (wifi_state == ESP_ERR_WIFI_CONN) {
                ESP_LOGI(TAG, "WiFi正在连接中，跳过此次自动连接");
                vTaskDelay(pdMS_TO_TICKS(10000)); // 10秒后再检查
                continue;
            }
        }
        
        // 检查是否有历史网络可用
        if (failed_attempts < 3) { // 限制重试次数
            ESP_LOGI(TAG, "尝试智能连接到历史WiFi网络... (第%d次)", failed_attempts + 1);
            
            // 等待一段时间确保WiFi系统稳定
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            esp_err_t ret = wifi_history_auto_connect();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "智能连接启动成功");
                failed_attempts = 0;
                // 等待连接结果，给足够时间完成连接
                vTaskDelay(pdMS_TO_TICKS(15000));
            } else {
                ESP_LOGW(TAG, "智能连接失败: %s", esp_err_to_name(ret));
                failed_attempts++;
            }
        } else {
            ESP_LOGW(TAG, "自动连接失败次数过多，暂停尝试");
            vTaskDelay(pdMS_TO_TICKS(60000)); // 1分钟后重置
            failed_attempts = 0;
            continue;
        }
        
        // 等待一段时间后重试
        vTaskDelay(pdMS_TO_TICKS(20000)); // 20秒后重试
    }
}

esp_err_t wifi_smart_connect(void)
{
    if (!history_initialized) {
        ESP_LOGW(TAG, "WiFi历史管理未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "开始智能WiFi连接...");
    return wifi_history_auto_connect();
}
