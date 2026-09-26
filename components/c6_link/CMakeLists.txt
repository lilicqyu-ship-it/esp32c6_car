idf_component_register(SRCS "link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_spi esp_driver_gpio esp_timer
                                c6_proto c6_sf)
