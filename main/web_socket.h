#ifndef WEB_SOCKET_H
#define WEB_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_http_server.h"

// 主动发送 WebSocket 文本消息
esp_err_t websocket_server_send_text(const char *data);

// 主动发送 WebSocket 二进制消息
esp_err_t websocket_server_send_binary(const uint8_t *data, size_t len);

// 启动WebSocket服务
void websocket_start(httpd_handle_t server);

// 检查WebSocket连接状态
bool websocket_is_connected(void);

// USB CDC接收数据回调函数
void usb_cdc_rx_callback(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // WEB_SOCKET_H
