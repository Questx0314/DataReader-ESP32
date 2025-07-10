#ifndef WEB_SOCKET_H
#define WEB_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_http_server.h"

// 主动发送 WebSocket 文本消息
esp_err_t websocket_server_send_text(const char *data);

void websocket_start(httpd_handle_t server);

// 设置是否开始发送数据（收到 START/STOP 指令）
void websocket_set_start_flag(bool start);

#ifdef __cplusplus
}
#endif

#endif // WEB_SOCKET_H
