/*
 * @Description: USB CDC Host 虚拟串口通信头文件
 */

#ifndef USBD_CDC_H
#define USBD_CDC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 接收数据的回调函数类型
typedef void (*usbd_cdc_rx_callback_t)(const uint8_t* data, size_t len);

/**
 * @brief 初始化USB CDC Host
 * 
 * @param rx_cb 接收数据回调函数
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t usbd_cdc_init(usbd_cdc_rx_callback_t rx_cb);

/**
 * @brief 发送数据到USB CDC设备
 * 
 * @param data 要发送的数据
 * @param len 数据长度
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t usbd_cdc_send_data(const uint8_t* data, size_t len);

/**
 * @brief 检查USB CDC设备是否已连接
 * 
 * @return true 已连接
 * @return false 未连接
 */
bool usbd_cdc_is_connected(void);

/**
 * @brief 反初始化USB CDC Host
 * 
 * @return esp_err_t ESP_OK成功，其他失败
 */
esp_err_t usbd_cdc_deinit(void);

#endif /* USBD_CDC_H */
