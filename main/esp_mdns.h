#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 mDNS 服务，并注册 http 服务
 * esp32.local:80 => Web server
 */
void esp_mdns_start(void);

#ifdef __cplusplus
}
#endif
