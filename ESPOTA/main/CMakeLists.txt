idf_component_register(SRCS "ESPOTA.c"
                                 "ota_manager.c"
                                 "stm32_forwarder.c"
                                 "status_led.c"
                                 "led_strip_encoder.c"
                                 "wifi_provisioning/ssid_manager.c"
                                 "wifi_provisioning/dns_server.c"
                                 "wifi_provisioning/wifi_config_ap.c"
                    PRIV_REQUIRES esp_driver_rmt
                                 esp_driver_gpio
                                 esp_driver_spi
                                 esp_driver_uart
                                 nvs_flash
                                 esp_wifi
                                 esp_event
                                 esp_netif
                                 esp_http_server
                                 esp_http_client
                                 esp_timer
                                 app_update
                                 esp_https_ota
                    INCLUDE_DIRS "." "wifi_provisioning"
                    EMBED_TXTFILES "wifi_config.html" "done.html" "dashboard.html"
)