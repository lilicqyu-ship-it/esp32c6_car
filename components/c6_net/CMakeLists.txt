idf_component_register(SRCS "net.c" "captive_dns.c" "mdns_lite.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_event esp_netif lwip nvs_flash)
