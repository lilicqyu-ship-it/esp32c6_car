# ADXL345 3-axis accelerometer (bit-banged 4-wire SPI) - local diag sensor.
# Like c6_led: sources always registered; a disabled config compiles the
# stub branch of adxl345.c so call sites need no conditional compilation.
idf_component_register(SRCS "adxl345.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_timer)
