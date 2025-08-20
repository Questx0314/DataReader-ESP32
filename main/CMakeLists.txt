idf_component_register(SRCS "wifi_history.c" "web_socket.c" "esp_mdns.c" "main.c" "wifi_manager.c" "http_server.c" "usbd_cdc.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_http_server nvs_flash json spiffs usb usb_host_cdc_acm esp_timer)
