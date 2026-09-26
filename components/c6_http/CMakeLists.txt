idf_component_register(SRCS "http_server.c" "ws_sessions.c" "assets_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server c6_proto c6_pair esp_partition
                                app_update esp_system)
