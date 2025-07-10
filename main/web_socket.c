#include "esp_http_server.h" 
#include "esp_log.h"          
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "web_socket.h" 
// ✅ WebSocket 支持所需头文件
#include "esp_http_server.h"

#define WS_URI "/ws"

static const char *TAG = "web_socket";
static httpd_req_t *ws_client_req = NULL; // 保存客户端连接
static bool start_sending_data = false;   // 标志：是否开始推送

// 设置是否开始发送数据
void websocket_set_start_flag(bool start) {
    start_sending_data = start;
}

// 主动发送 WebSocket 消息（给客户端）
esp_err_t websocket_server_send_text(const char *data) {
    if (!ws_client_req) {
        ESP_LOGW(TAG, "❌ 没有 WebSocket 客户端连接");
        return ESP_FAIL;
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = strlen(data)
    };

    esp_err_t ret = httpd_ws_send_frame(ws_client_req, &frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ WebSocket 发送失败，释放客户端");
        ws_client_req = NULL;  // 释放旧连接
    }
    return ret;
}


// WebSocket 消息回调（接收来自客户端的消息）
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "✅ WebSocket 握手成功");
        ws_client_req = req;
        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(httpd_ws_frame_t));
    
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = NULL;
    frame.len = req->content_len;

    // 获取帧头信息，判断类型
    httpd_ws_recv_frame(req, &frame, 0);  // 第一次调用只读取头信息

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGW(TAG, "🛑 WebSocket 客户端断开连接");
        ws_client_req = NULL;
        return ESP_OK;
    }

    // 正常读取 payload
    frame.payload = malloc(frame.len + 1);
    if (!frame.payload) {
        ESP_LOGE(TAG, "❌ 内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    httpd_ws_recv_frame(req, &frame, frame.len);
    frame.payload[frame.len] = '\0';

    ESP_LOGI(TAG, "WS received: %s", (char *)frame.payload);

    if (strcmp((char *)frame.payload, "START") == 0) {
        websocket_set_start_flag(true);
    } else if (strcmp((char *)frame.payload, "STOP") == 0) {
        websocket_set_start_flag(false);
    }

    free(frame.payload);
    return ESP_OK;
}

void websocket_start(httpd_handle_t server) 
{
    if (server == NULL) {
        ESP_LOGE(TAG, "❌ 传入的 server 句柄为 NULL，无法注册 WebSocket URI");
        return;
    }

    static const httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true
    };

    ESP_LOGI(TAG, "🟡 尝试注册 WebSocket URI: %s", ws_uri.uri);

    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ WebSocket URI '%s' 注册成功", ws_uri.uri);
    } else if (ret == ESP_ERR_HTTPD_HANDLERS_FULL) {
        ESP_LOGE(TAG, "❌ URI 注册失败：handler 已满 (max_uri_handlers 配置太小)");
    } else if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGE(TAG, "❌ URI 注册失败：该 URI 已注册（重复注册）");
    } else if (ret == ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "❌ URI 注册失败：参数无效（可能 ws_uri 字段不合法或 server 句柄无效）");
    } else {
        ESP_LOGE(TAG, "❌ URI 注册失败：未预期错误 %s", esp_err_to_name(ret));
    }
}
