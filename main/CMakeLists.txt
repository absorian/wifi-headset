idf_component_register(SRCS main.c captive_portal.c wifi_handle.c
                    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio
                    esp_wifi esp_netif esp_http_server nvs_flash dns_server
                    INCLUDE_DIRS "."
                    EMBED_FILES captive.html)
