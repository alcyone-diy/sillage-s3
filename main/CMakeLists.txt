idf_component_register(SRCS "main.cpp" "TileEngine.cpp" "lodepng.c" "lodepng_alloc.c"
                    INCLUDE_DIRS "."
                    REQUIRES ch422g esp_lcd lvgl esp_lcd_touch_gt911 esp_driver_i2c esp_driver_spi esp_driver_gpio fatfs sdmmc esp_timer)
