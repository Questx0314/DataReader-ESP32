idf_component_register(SRCS "web_socket.c" "esp_mdns.c" "main.c" "wifi_manager.c" "http_server.c" "usbd_cdc.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_http_server nvs_flash json spiffs)
