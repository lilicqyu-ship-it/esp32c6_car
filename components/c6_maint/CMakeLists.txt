# BLE DPT channel - compiled only when CONFIG_C6_MAINT_BLE is enabled
# (also requires CONFIG_BT_ENABLED=y, e.g. sdkconfig with NimBLE)
if(CONFIG_C6_MAINT_BLE)
    idf_component_register(SRCS "maint_ble.c"
                           INCLUDE_DIRS "include"
                           REQUIRES bt nvs_flash c6_link c6_factory)
else()
    idf_component_register()
endif()
