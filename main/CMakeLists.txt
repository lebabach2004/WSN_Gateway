idf_component_register(SRCS "http_server_app.c" "app_main.c" http_server_app.c json_generator.c json_parser.c wifi_sta.c
                    PRIV_REQUIRES spi_flash driver esp_wifi nvs_flash esp_http_server
                    INCLUDE_DIRS ".")
