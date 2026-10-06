# M5Stack Tab5 board support (display / touch / IO-expander power rails).
# Compiles to an empty library unless CONFIG_BOARD_M5STACK_TAB5=y, so the
# component is safe to leave in the shared components/ directory.
idf_component_register(
    SRCS "tab5_board.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "panel_data"
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_lcd esp_hw_support
    PRIV_REQUIRES esp_mm
)
