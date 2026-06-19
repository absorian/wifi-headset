idf_component_register(SRCS main.c i2s_periph.c nvs_storage.c
                    wifi_handle.c captive_portal.c discovery.c control_transport.c
                    periph.c
                    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio
                    esp_wifi esp_netif esp_http_server nvs_flash dns_server audio
                    INCLUDE_DIRS "."
                    EMBED_FILES captive.html)
