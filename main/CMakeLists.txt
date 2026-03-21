idf_component_register(SRCS "main.cpp" "TileEngine.cpp" "http_fs.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ch422g esp_lcd lvgl esp_lcd_touch_gt911 esp_driver_i2c esp_driver_spi esp_driver_gpio fatfs sdmmc esp_timer esp_wifi nvs_flash esp_http_client esp_netif mbedtls)
