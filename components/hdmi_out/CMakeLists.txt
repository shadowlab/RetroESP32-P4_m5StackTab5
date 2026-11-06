idf_component_register(
    SRCS "hdmi_out.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c
    PRIV_REQUIRES esp_driver_spi esp_driver_i2s esp_driver_gpio esp_timer esp_rom hal soc heap freertos log
)
