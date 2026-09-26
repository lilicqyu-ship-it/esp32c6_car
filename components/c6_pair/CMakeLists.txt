idf_component_register(SRCS "pair.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c6_proto c6_factory mbedtls esp_timer esp_hw_support)
