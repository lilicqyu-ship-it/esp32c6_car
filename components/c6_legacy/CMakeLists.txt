# TCP 8080 legacy bridge - compiled only when CONFIG_C6_LEGACY_TCP is enabled
if(CONFIG_C6_LEGACY_TCP)
    idf_component_register(SRCS "legacy_tcp.c"
                           INCLUDE_DIRS "include"
                           REQUIRES c6_link c6_proto lwip esp_timer)
else()
    idf_component_register()
endif()
