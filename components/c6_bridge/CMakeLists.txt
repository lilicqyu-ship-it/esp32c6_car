idf_component_register(SRCS "bridge.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c6_proto c6_link c6_http c6_pair esp_timer)
