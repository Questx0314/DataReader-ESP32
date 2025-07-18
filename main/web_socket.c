#include "esp_http_server.h" 
#include "esp_log.h"          
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "web_socket.h" 
#include "usbd_cdc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// WebSocket配置常量
#define WS_URI "/ws"
#define WS_MAX_PAYLOAD_LEN 1024
#define WS_TASK_STACK_SIZE 4096
#define WS_TASK_PRIORITY 2
#define WS_QUEUE_SIZE 10

// 数据处理常量
#define WS_SEND_DELAY_MS 5
#define WS_TEXT_DETECTION_MIN_CHAR 32
#define WS_TEXT_DETECTION_MAX_CHAR 127

static const char *TAG = "web_socket";

// WebSocket消息类型
typedef enum {
    WS_MSG_TEXT,
    WS_MSG_BINARY
} ws_msg_type_t;

// WebSocket消息结构
typedef struct {
    ws_msg_type_t type;
    void *data;
    size_t len;
} ws_msg_t;

// WebSocket上下文
typedef struct {
    httpd_handle_t server;
    int client_fd;
    bool connected;
    QueueHandle_t msg_queue;
    TaskHandle_t task_handle;
} ws_ctx_t;

static ws_ctx_t ws_ctx = {0};

// 辅助函数：检查数据是否为文本格式
static bool is_data_text_format(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        // 检查是否包含非ASCII字符或控制字符(除了\r\n\t)
        bool is_printable = (byte >= WS_TEXT_DETECTION_MIN_CHAR && byte <= WS_TEXT_DETECTION_MAX_CHAR);
        bool is_whitespace = (byte == '\r' || byte == '\n' || byte == '\t');

        if (!is_printable && !is_whitespace) {
            return false;
        }
    }
    return true;
}

// 消息发送任务
static void ws_send_task(void *pvParameters) {
    ws_ctx_t *ctx = (ws_ctx_t *)pvParameters;
    ws_msg_t msg;
    
    ESP_LOGI(TAG, "WebSocket发送任务已启动");
    
    while (1) {
        // 等待队列中的消息
        if (xQueueReceive(ctx->msg_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // 检查连接状态
            if (!ctx->connected || ctx->server == NULL || ctx->client_fd < 0) {
                ESP_LOGW(TAG, "WebSocket未连接，丢弃消息");
                if (msg.data) {
                    free(msg.data);
                }
                continue;
            }
            
            // 准备发送帧
            httpd_ws_frame_t ws_frame = {
                .final = true,
                .fragmented = false,
                .type = (msg.type == WS_MSG_TEXT) ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY,
                .payload = msg.data,
                .len = msg.len
            };
            
            // 尝试发送数据 (修复：使用正确的async函数)
            esp_err_t ret = httpd_ws_send_frame_async(ctx->server, ctx->client_fd, &ws_frame);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "WebSocket发送失败: %s", esp_err_to_name(ret));
                ctx->connected = false;
                ctx->client_fd = -1;
            } else {
                ESP_LOGI(TAG, "%s数据发送成功: %d字节", 
                         (msg.type == WS_MSG_TEXT) ? "文本" : "二进制", msg.len);
            }
            
            // 释放数据
            if (msg.data) {
                free(msg.data);
            }
            
            // 短暂延迟，让系统有时间处理其他任务
            vTaskDelay(pdMS_TO_TICKS(WS_SEND_DELAY_MS));
        }
    }
}

// 初始化WebSocket上下文
static void ws_init_ctx(httpd_handle_t server) {
    // 确保之前的资源被释放
    if (ws_ctx.msg_queue != NULL) {
        vQueueDelete(ws_ctx.msg_queue);
    }
    
    if (ws_ctx.task_handle != NULL) {
        vTaskDelete(ws_ctx.task_handle);
    }
    
    // 初始化上下文
    memset(&ws_ctx, 0, sizeof(ws_ctx_t));
    ws_ctx.server = server;
    ws_ctx.client_fd = -1;
    ws_ctx.connected = false;
    
    // 创建消息队列
    ws_ctx.msg_queue = xQueueCreate(WS_QUEUE_SIZE, sizeof(ws_msg_t));
    if (ws_ctx.msg_queue == NULL) {
        ESP_LOGE(TAG, "创建WebSocket消息队列失败");
        return;
    }
    
    // 创建发送任务
    BaseType_t task_created = xTaskCreate(
        ws_send_task,
        "ws_send_task",
        WS_TASK_STACK_SIZE,
        &ws_ctx,
        WS_TASK_PRIORITY,
        &ws_ctx.task_handle
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "创建WebSocket发送任务失败");
        vQueueDelete(ws_ctx.msg_queue);
        ws_ctx.msg_queue = NULL;
    }
}

// 检查WebSocket连接状态
bool websocket_is_connected(void) {
    return ws_ctx.connected && ws_ctx.client_fd >= 0;
}

// 向队列添加文本消息
esp_err_t websocket_server_send_text(const char *data) {
    if (!data || !ws_ctx.msg_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 为消息数据创建副本
    size_t len = strlen(data);
    char *data_copy = malloc(len + 1);
    if (!data_copy) {
        ESP_LOGE(TAG, "无法分配内存");
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(data_copy, data, len);
    data_copy[len] = '\0';
    
    // 创建消息结构
    ws_msg_t msg = {
        .type = WS_MSG_TEXT,
        .data = data_copy,
        .len = len
    };
    
    ESP_LOGI(TAG, "正在发送文本到队列: %s", data_copy);
    
    // 将消息发送到队列
    if (xQueueSend(ws_ctx.msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WebSocket消息队列已满，丢弃消息");
        free(data_copy);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// 向队列添加二进制消息
esp_err_t websocket_server_send_binary(const uint8_t *data, size_t len) {
    if (!data || len == 0 || !ws_ctx.msg_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 打印二进制数据信息
    ESP_LOGI(TAG, "二进制数据(%d字节)添加到发送队列", len);
    
    // 为消息数据创建副本
    uint8_t *data_copy = malloc(len);
    if (!data_copy) {
        ESP_LOGE(TAG, "无法分配内存");
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(data_copy, data, len);
    
    // 创建消息结构
    ws_msg_t msg = {
        .type = WS_MSG_BINARY,
        .data = data_copy,
        .len = len
    };
    
    // 将消息发送到队列
    if (xQueueSend(ws_ctx.msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WebSocket消息队列已满，丢弃消息");
        free(data_copy);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// 从USB CDC接收到数据的回调函数
void usb_cdc_rx_callback(const uint8_t* data, size_t len) {
    if (!websocket_is_connected() || !data || len == 0) {
        ESP_LOGW(TAG, "未转发CDC数据：WebSocket未连接或数据无效");
        return;
    }
    
    // 打印接收到的数据
    ESP_LOGI(TAG, "从CDC接收到数据: %d字节", len);
    
    // 检查数据是否是文本格式
    bool is_text = is_data_text_format(data, len);
    
    // 如果是文本数据，作为文本发送
    if (is_text) {
        // 创建字符串副本（确保以null结尾）
        char *text_copy = malloc(len + 1);
        if (!text_copy) {
            ESP_LOGE(TAG, "内存分配失败");
            return;
        }
        
        memcpy(text_copy, data, len);
        text_copy[len] = '\0';
        
        // 作为文本消息发送
        ESP_LOGI(TAG, "转发文本数据到WebSocket: %s", text_copy);
        esp_err_t ret = websocket_server_send_text(text_copy);
        free(text_copy);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "转发文本数据到WebSocket失败");
        }
    }
    // 否则作为二进制数据发送
    else {
        uint8_t *data_copy = malloc(len);
        if (!data_copy) {
            ESP_LOGE(TAG, "内存分配失败");
            return;
        }
        
        memcpy(data_copy, data, len);
        esp_err_t ret = websocket_server_send_binary(data_copy, len);
        free(data_copy);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "转发二进制数据到WebSocket失败");
        }
    }
}

// WebSocket处理程序
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // 处理WebSocket握手
        ESP_LOGI(TAG, "WebSocket握手成功");
        
        // 更新连接信息
        ws_ctx.client_fd = httpd_req_to_sockfd(req);
        ws_ctx.connected = true;
        
        ESP_LOGI(TAG, "WebSocket客户端已连接，fd=%d", ws_ctx.client_fd);
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_frame;
    memset(&ws_frame, 0, sizeof(httpd_ws_frame_t));
    
    // 获取帧类型
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "接收WebSocket帧失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 处理关闭帧
    if (ws_frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket客户端断开连接");
        ws_ctx.connected = false;
        ws_ctx.client_fd = -1;
        return ESP_OK;
    }
    
    // 接收数据
    if (ws_frame.len) {
        uint8_t *payload = malloc(ws_frame.len + 1);
        if (!payload) {
            ESP_LOGE(TAG, "内存分配失败");
            return ESP_ERR_NO_MEM;
        }
        
        // 接收帧数据
        ws_frame.payload = payload;
        ret = httpd_ws_recv_frame(req, &ws_frame, ws_frame.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "接收WebSocket数据失败: %s", esp_err_to_name(ret));
            free(payload);
            return ret;
        }
        
        // 处理不同类型的消息
        switch (ws_frame.type) {
            case HTTPD_WS_TYPE_TEXT:
                payload[ws_frame.len] = 0; // 确保文本以null结尾
                ESP_LOGI(TAG, "接收到WebSocket文本: %s", (char *)payload);
                
                // 转发到CDC设备
                if (usbd_cdc_is_connected()) {
                    ret = usbd_cdc_send_data(payload, ws_frame.len);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "文本数据已转发到CDC");
                    }
                } else {
                    ESP_LOGW(TAG, "CDC设备未连接，无法发送数据");
                }
                break;
                
            case HTTPD_WS_TYPE_BINARY:
                ESP_LOGI(TAG, "接收到WebSocket二进制数据: %d字节", ws_frame.len);
                
                // 转发到CDC设备
                if (usbd_cdc_is_connected()) {
                    ret = usbd_cdc_send_data(payload, ws_frame.len);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "二进制数据已转发到CDC");
                    }
                }
                break;
                
            default:
                ESP_LOGW(TAG, "未处理的WebSocket帧类型: %d", ws_frame.type);
                break;
        }
        
        free(payload);
    }
    
    return ESP_OK;
}

// 启动WebSocket服务
void websocket_start(httpd_handle_t server) {
    if (!server) {
        ESP_LOGE(TAG, "无效的HTTP服务器句柄");
        return;
    }
    
    // 初始化WebSocket上下文
    ws_init_ctx(server);
    
    // 注册WebSocket处理程序
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    
    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WebSocket处理程序注册成功");
    } else {
        ESP_LOGE(TAG, "WebSocket处理程序注册失败: %s", esp_err_to_name(ret));
    }
}

