idf_component_register(SRCS "CH422GController.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_i2c
                    PRIV_REQUIRES log)
