idf_component_register(
    SRCS "main.c" "usb_drive.c" "wifi_link.c" "file_server.c"
    INCLUDE_DIRS "." "includes" "sprites"
    EMBED_TXTFILES "file_server.html"
    REQUIRES odroid st7701_lcd gt911_touch esp_driver_ledc
             ppa_engine gamepad audio pngaux
             driver esp_lcd esp_psram fatfs vfs sdmmc nvs_flash
             esp_partition esp_app_format esp_event app_update
             psram_app_loader serial_upload hal soc
             tab5_board esp_wifi esp_netif esp_http_server
)

# The original RetroESP32 code uses unsafe sprintf patterns.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-overflow
    -Wno-error=restrict
    -Wno-error=format-truncation
)
