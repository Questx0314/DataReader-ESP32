/*
 * @Description: USB CDC Host 虚拟串口通信实现
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usbd_cdc.h"

static const char *TAG = "usbd_cdc";

// 任务配置常量
#define CDC_HOST_TASK_PRIORITY    5
#define CDC_HOST_TASK_STACK_SIZE  4096
#define CDC_DEVICE_CHECK_INTERVAL_MS 500
#define CDC_DATA_BUFFER_SIZE      1024

// STM32 Virtual COM Port VID/PID
#define STM32_USB_DEVICE_VID      (0x0483)
#define STM32_USB_DEVICE_PID      (0x5740)

// 串口通信参数
#define CDC_BAUD_RATE             115200
#define CDC_DATA_BITS             8
#define CDC_STOP_BITS             0  // 1位停止位
#define CDC_PARITY                0  // 无校验

// 超时和重试配置
#define CDC_CONNECTION_TIMEOUT_MS 5000
#define CDC_TX_TIMEOUT_MS         1000
#define CDC_MUTEX_TIMEOUT_MS      100
#define CDC_TASK_EXIT_TIMEOUT_MS  1000

// USB CDC设备状态
typedef enum {
    CDC_DEVICE_STATE_DISCONNECTED = 0,
    CDC_DEVICE_STATE_CONNECTED,
} cdc_device_state_t;

// USB CDC设备上下文
typedef struct {
    cdc_acm_dev_hdl_t cdc_hdl;
    cdc_device_state_t state;
    usbd_cdc_rx_callback_t rx_cb;
    uint8_t rx_buffer[CDC_DATA_BUFFER_SIZE];
    SemaphoreHandle_t mutex;
    TaskHandle_t task_handle;
    bool is_initialized;
} cdc_dev_context_t;

static cdc_dev_context_t s_cdc_dev = {0};
static SemaphoreHandle_t device_disconnected_sem;

// CDC设备事件回调
static void cdc_device_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    cdc_dev_context_t *dev = (cdc_dev_context_t *)user_ctx;
    
    switch (event->type) {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "CDC设备已断开");
            dev->state = CDC_DEVICE_STATE_DISCONNECTED;
            dev->cdc_hdl = NULL;
            if (device_disconnected_sem) {
                xSemaphoreGive(device_disconnected_sem);
            }
            break;
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC设备发生错误: %d", event->data.error);
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "CDC设备串口状态变化: 0x%02x", event->data.serial_state.val);
            break;
        case CDC_ACM_HOST_NETWORK_CONNECTION:
            ESP_LOGI(TAG, "CDC设备网络连接状态变化: %d", event->data.network_connected);
            break;
        default:
            ESP_LOGW(TAG, "未知CDC设备事件: %d", event->type);
            break;
    }
}

// CDC数据接收回调
static bool cdc_data_received_callback(const uint8_t *data, size_t data_len, void *user_ctx)
{
    cdc_dev_context_t *dev = (cdc_dev_context_t *)user_ctx;
    
    ESP_LOGI(TAG, "接收到CDC数据: %d字节", data_len);
    
    if (dev->rx_cb && data_len > 0) {
        // 调用用户注册的回调函数
        dev->rx_cb(data, data_len);
    }
    
    return true;
}

// USB Host库处理任务
static void usb_lib_task(void *arg)
{
    while (1) {
        // 处理系统事件
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: 所有设备已释放");
            // 继续处理USB事件以允许设备重新连接
        }
    }
}

// USB CDC Host任务
static void usb_cdc_host_task(void *arg)
{
    cdc_dev_context_t *dev = (cdc_dev_context_t *)arg;
    cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = CDC_CONNECTION_TIMEOUT_MS,
        .out_buffer_size = CDC_DATA_BUFFER_SIZE,
        .in_buffer_size = CDC_DATA_BUFFER_SIZE,
        .event_cb = cdc_device_event_callback,
        .data_cb = cdc_data_received_callback,
        .user_arg = dev,
    };
    
    int retry_count = 0;
    
    while (dev->is_initialized) {
        if (dev->state == CDC_DEVICE_STATE_DISCONNECTED) {
            // 尝试打开CDC设备
            ESP_LOGI(TAG, "尝试打开CDC设备 (第%d次尝试)...", ++retry_count);
            
            // 尝试使用STM32的VID/PID打开设备
            esp_err_t err = cdc_acm_host_open(STM32_USB_DEVICE_VID, STM32_USB_DEVICE_PID, 0, &dev_config, &dev->cdc_hdl);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "CDC设备已打开成功 (STM32 VCP)");
                dev->state = CDC_DEVICE_STATE_CONNECTED;
                
                // 打印设备描述符信息
                cdc_acm_host_desc_print(dev->cdc_hdl);
                
                // 设置串口参数 (115200 8N1)
                cdc_acm_line_coding_t line_coding = {
                    .dwDTERate = CDC_BAUD_RATE,
                    .bCharFormat = CDC_STOP_BITS,  // 1位停止位
                    .bParityType = CDC_PARITY,     // 无校验
                    .bDataBits = CDC_DATA_BITS     // 8位数据位
                };
                
                err = cdc_acm_host_line_coding_set(dev->cdc_hdl, &line_coding);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "设置串口参数失败: %s", esp_err_to_name(err));
                }
                
                // 设置DTR和RTS信号
                err = cdc_acm_host_set_control_line_state(dev->cdc_hdl, true, true);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "设置控制线状态失败: %s", esp_err_to_name(err));
                }
                
                // // 发送测试数据
                // const char *test_str = "CDC test initialized!";
                // err = cdc_acm_host_data_tx_blocking(dev->cdc_hdl, (const uint8_t *)test_str, strlen(test_str), CDC_TX_TIMEOUT_MS);
                // if (err != ESP_OK) {
                //     ESP_LOGW(TAG, "发送测试数据失败: %s", esp_err_to_name(err));
                // } else {
                //     ESP_LOGI(TAG, "发送测试数据成功");
                // }
                
                retry_count = 0;
            } else {
                if (err == ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "未找到STM32 CDC设备，等待设备连接...");
                } else {
                    ESP_LOGE(TAG, "打开CDC设备失败: %s", esp_err_to_name(err));
                }
                vTaskDelay(pdMS_TO_TICKS(CDC_DEVICE_CHECK_INTERVAL_MS));
            }
        } else {
            // 设备已连接，等待事件
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "CDC设备连接状态: %s", dev->state == CDC_DEVICE_STATE_CONNECTED ? "已连接" : "未连接");
        }
    }
    
    // 清理并退出任务
    if (dev->cdc_hdl) {
        cdc_acm_host_close(dev->cdc_hdl);
        dev->cdc_hdl = NULL;
    }
    
    dev->task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t usbd_cdc_init(usbd_cdc_rx_callback_t rx_cb)
{
    esp_err_t ret = ESP_OK;
    
    if (s_cdc_dev.is_initialized) {
        ESP_LOGW(TAG, "USB CDC Host已初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 创建信号量
    device_disconnected_sem = xSemaphoreCreateBinary();
    if (device_disconnected_sem == NULL) {
        ESP_LOGE(TAG, "创建设备断开信号量失败");
        return ESP_ERR_NO_MEM;
    }
    
    // 初始化互斥锁
    s_cdc_dev.mutex = xSemaphoreCreateMutex();
    if (s_cdc_dev.mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        vSemaphoreDelete(device_disconnected_sem);
        return ESP_ERR_NO_MEM;
    }
    
    // 初始化USB Host
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    
    ESP_LOGI(TAG, "正在安装USB Host...");
    ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "安装USB Host失败: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_cdc_dev.mutex);
        vSemaphoreDelete(device_disconnected_sem);
        return ret;
    }
    
    // 创建USB库处理任务
    TaskHandle_t usb_lib_task_handle;
    BaseType_t task_created = xTaskCreate(
        usb_lib_task,
        "usb_lib",
        4096,
        NULL,
        CDC_HOST_TASK_PRIORITY,
        &usb_lib_task_handle
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "创建USB库任务失败");
        usb_host_uninstall();
        vSemaphoreDelete(s_cdc_dev.mutex);
        vSemaphoreDelete(device_disconnected_sem);
        return ESP_ERR_NO_MEM;
    }
    
    // 初始化CDC ACM Host
    ESP_LOGI(TAG, "正在安装CDC ACM Host驱动...");
    ret = cdc_acm_host_install(NULL);  // 使用默认配置
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "安装CDC ACM Host驱动失败: %s", esp_err_to_name(ret));
        vTaskDelete(usb_lib_task_handle);
        usb_host_uninstall();
        vSemaphoreDelete(s_cdc_dev.mutex);
        vSemaphoreDelete(device_disconnected_sem);
        return ret;
    }
    
    // 初始化设备上下文
    memset(&s_cdc_dev, 0, sizeof(cdc_dev_context_t));
    s_cdc_dev.state = CDC_DEVICE_STATE_DISCONNECTED;
    s_cdc_dev.rx_cb = rx_cb;
    s_cdc_dev.is_initialized = true;
    
    // 创建CDC Host任务
    ESP_LOGI(TAG, "创建USB CDC Host任务...");
    task_created = xTaskCreate(
        usb_cdc_host_task,
        "usb_cdc_host",
        CDC_HOST_TASK_STACK_SIZE,
        &s_cdc_dev,
        CDC_HOST_TASK_PRIORITY - 1,
        &s_cdc_dev.task_handle
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "创建USB CDC Host任务失败");
        cdc_acm_host_uninstall();
        vTaskDelete(usb_lib_task_handle);
        usb_host_uninstall();
        vSemaphoreDelete(s_cdc_dev.mutex);
        vSemaphoreDelete(device_disconnected_sem);
        s_cdc_dev.is_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "USB CDC Host初始化成功");
    return ESP_OK;
}

esp_err_t usbd_cdc_send_data(const uint8_t* data, size_t len)
{
    if (!s_cdc_dev.is_initialized) {
        ESP_LOGW(TAG, "USB CDC Host未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_cdc_dev.state != CDC_DEVICE_STATE_CONNECTED || s_cdc_dev.cdc_hdl == NULL) {
        ESP_LOGW(TAG, "CDC设备未连接");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 获取互斥锁
    if (xSemaphoreTake(s_cdc_dev.mutex, pdMS_TO_TICKS(CDC_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "获取互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "发送数据到CDC设备: %d字节", len);
    esp_err_t ret = cdc_acm_host_data_tx_blocking(s_cdc_dev.cdc_hdl, data, len, CDC_TX_TIMEOUT_MS);
    
    // 释放互斥锁
    xSemaphoreGive(s_cdc_dev.mutex);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "发送数据失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "数据发送成功");
    return ESP_OK;
}

bool usbd_cdc_is_connected(void)
{
    return s_cdc_dev.is_initialized && 
           s_cdc_dev.state == CDC_DEVICE_STATE_CONNECTED && 
           s_cdc_dev.cdc_hdl != NULL;
}

esp_err_t usbd_cdc_deinit(void)
{
    if (!s_cdc_dev.is_initialized) {
        ESP_LOGW(TAG, "USB CDC Host未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 标记为未初始化，通知任务退出
    s_cdc_dev.is_initialized = false;
    
    // 等待任务退出
    int timeout_count = CDC_TASK_EXIT_TIMEOUT_MS / 100; // 转换为100ms的计数
    while (s_cdc_dev.task_handle != NULL && timeout_count > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_count--;
    }
    
    // 如果任务未退出，强制删除
    if (s_cdc_dev.task_handle != NULL) {
        ESP_LOGW(TAG, "任务未在超时时间内退出，强制删除");
        vTaskDelete(s_cdc_dev.task_handle);
        s_cdc_dev.task_handle = NULL;
    }
    
    // 关闭CDC设备
    if (s_cdc_dev.cdc_hdl) {
        cdc_acm_host_close(s_cdc_dev.cdc_hdl);
        s_cdc_dev.cdc_hdl = NULL;
    }
    
    // 卸载CDC ACM Host驱动
    cdc_acm_host_uninstall();
    
    // 卸载USB Host
    usb_host_uninstall();
    
    // 删除互斥锁和信号量
    if (s_cdc_dev.mutex) {
        vSemaphoreDelete(s_cdc_dev.mutex);
        s_cdc_dev.mutex = NULL;
    }
    
    if (device_disconnected_sem) {
        vSemaphoreDelete(device_disconnected_sem);
        device_disconnected_sem = NULL;
    }
    
    ESP_LOGI(TAG, "USB CDC Host已反初始化");
    return ESP_OK;
}
