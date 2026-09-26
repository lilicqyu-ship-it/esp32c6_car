idf_component_register(SRCS "sha512.c" "ed25519v.c" "bundle.c" "ota_self.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_partition app_update esp_timer esp_system c6_proto
                       EMBED_FILES "keys/pub_ed25519_dev.bin")
