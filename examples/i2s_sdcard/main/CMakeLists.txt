idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES xc-soundfont-synth esp_driver_i2s esp_driver_sdspi esp_driver_spi fatfs sdmmc esp_timer
)
