# ==============================================================================
# CMake Build Configuration
# Tells the ESP-IDF compiler which files to build, where to find headers,
# and which driver components to include in the firmware image.
# ==============================================================================

idf_component_register(
    SRCS "src/main.c" "src/minmea.c" "src/gps_parser.c" "src/sd_logger.c" "src/web_server.c" "src/dns_server.c" "src/bmp280.c" "src/sensor_task.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "index.html"
    EMBED_FILES "favicon.ico"
    REQUIRES esp_driver_uart esp_driver_gpio fatfs sdmmc esp_driver_spi esp_wifi esp_http_server nvs_flash esp_driver_i2c driver
)