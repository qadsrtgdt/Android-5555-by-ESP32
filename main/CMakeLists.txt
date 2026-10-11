idf_component_register(SRCS "main.c" "adb_packet.c" "adb_auth.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES usb mbedtls driver esp_timer)
