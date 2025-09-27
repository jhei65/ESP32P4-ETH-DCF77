idf_component_register(SRCS "dcf77.c"
                    REQUIRES esp_driver_gptimer esp_driver_gpio esp_netif
                    INCLUDE_DIRS ".")
