idf_component_register(SRCS "app_main.c" "app_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c6_proto c6_factory c6_link c6_net c6_http espcoredump
                                c6_pair c6_bridge c6_ota c6_led nvs_flash esp_timer
                                esp_hw_support esp_wifi)

# optional components stay out of the default build graph until enabled
if(CONFIG_C6_MAINT_BLE)
    idf_component_get_property(lib c6_maint COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${lib})
endif()

if(CONFIG_C6_LEGACY_TCP)
    idf_component_get_property(lib2 c6_legacy COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${lib2})
endif()
