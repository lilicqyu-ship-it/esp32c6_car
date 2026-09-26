idf_component_register(SRCS "link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_timer c6_proto)
