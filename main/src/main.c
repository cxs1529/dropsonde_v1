/**
 * ==============================================================================
 * @file    main.c
 * @brief   Application Entry Point & System Operational Overview.
 * 
 * @details
 * DEVICE OPERATIONAL DESCRIPTION:
 * -------------------------------
 * The NOAA/AOML Dropsonde Logger is an industrial-grade embedded telemetry 
 * recorder built around the ESP32-C3 microcontroller.
 * 
 * 1. DATA INPUT & TIMEKEEPING PIPELINE:
 *    - The GPS module streams NMEA 0183 sentences via UART1 (GPIO 6 RX / 7 TX).
 *    - `gps_rx_task` parses GSA, GGA, and RMC sentences using the `minmea` library.
 *    - RMC frames continuously synchronize the ESP32's internal system clock (RTC).
 *    - If GPS fix is lost or UART goes silent, a 1Hz software timer uses the internal 
 *      RTC to generate a fallback telemetry payload, guaranteeing an unbroken 1Hz log.
 *    - Coordinates are natively converted from DDMM.MMMM to Decimal Degrees (dd.ddd).
 * 
 * 2. STORAGE & DEBOUNCE PIPELINE:
 *    - Formatted payloads are sent to `gps_queue`.
 *    - `gps_log_task` reads from the queue and handles SD card storage over SPI DMA.
 *    - Hardware switch GPIO 3 (with 5s debounce) controls logging state.
 *    - File Rotation: Data is saved to `/sdcard/DATA/GPSDATAx.dat`. Pausing for > 60s 
 *      automatically starts a new sequence number file.
 *    - Metadata Caching: Tracks line count, first, and last location records in hidden 
 *      `/.META/GPSDATAx.dat.META` files for instantaneous web server rendering.
 *    - Power Failure Protection: RAM data is buffered in 4KB blocks (`setvbuf`) and 
 *      committed to physical SD flash memory using `fsync()` every 10 seconds.
 * 
 * 3. NETWORKING & POWER MANAGEMENT PIPELINE:
 *    - Hardware switch GPIO 2 (with 10s debounce) controls Wi-Fi AP power.
 *    - When enabled, `wifi_manager_task` boots the Wi-Fi AP and HTTP server.
 *    - The web interface (`index.html`) renders a NOAA-approved telemetry dashboard.
 *    - JavaScript polls `/api/live` every 500ms to update the telemetry grid asynchronously.
 *    - Includes REST endpoints for file deletion (with built-in active file protection).
 * 
 * 4. FreeRTOS TASK PRIORITIES:
 *    - Priority 10 (Highest) : `gps_rx_task` (Must never miss UART bytes)
 *    - Priority 6            : `gps_log_task` (Handles file I/O & SD debouncing)
 *    - Priority 4            : `wifi_manager_task` (Manages Wi-Fi power transitions)
 *    - Priority 2 (Lowest)  : `gps_tail_task` (Verifies SD contents every 60s)
 * ==============================================================================
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// Include modular components
#include "config.h"
#include "gps_parser.h"
#include "sd_logger.h"
#include "web_server.h"

static const char *TAG = "MAIN";

// Instantiate shared global structures declared in config.h exactly once here.
// These variables reside in the ESP32's internal RAM.
QueueHandle_t gps_queue = NULL;
SemaphoreHandle_t live_data_mutex = NULL;
gps_msg_t live_gps_data = {0}; 
bool live_is_logging = false;
char live_active_file[32] = ""; 

/** 
 * @brief Configure physical switch pins with internal pull-ups.
 * 
 * @details
 * Internal pull-ups (`GPIO_PULLUP_ENABLE`) ensure the pins read HIGH (1) 
 * when nothing is connected. When you toggle a physical switch connected 
 * to GND, it pulls the voltage LOW (0), which acts as our "ON" signal.
 */
static void init_switches(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOG_SWITCH_PIN) | (1ULL << WIFI_SWITCH_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

/**
 * @brief The main execution entry point initiated by ESP-IDF.
 */
void app_main(void) {
    ESP_LOGI(TAG, "Booting Industrial GPS Logger...");

    // 1. Initialize Non-Volatile Storage (NVS)
    // Required by the ESP-IDF Wi-Fi driver to store calibration data.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      nvs_flash_init();
    }

    // 2. Setup Hardware and Storage
    init_switches();
    if (init_sd_card() == ESP_OK) {
        cleanup_orphaned_metadata(); // Clean up unused .META files if user manually deleted logs
    }
    init_wifi_base(); 

    // 3. Setup RTOS Synchronization Primitives
    live_data_mutex = xSemaphoreCreateMutex();
    gps_queue = xQueueCreate(10, sizeof(gps_msg_t));

    // 4. Spawn FreeRTOS Tasks
    // Higher numbers = Higher Execution Priority
    if (gps_queue != NULL && live_data_mutex != NULL) {
        
        // Priority 10: Parse GPS instantly to avoid hardware UART FIFO buffer overflow
        xTaskCreate(gps_rx_task, "gps_rx", 4096, NULL, 10, NULL);
        
        // Priority 6: Consumer task writes to SD card when OS time permits
        xTaskCreate(gps_log_task, "gps_log", 4096, NULL, 6, NULL);
        
        // Priority 4: Background network management state machine
        xTaskCreate(wifi_manager_task, "wifi_mng", 4096, NULL, 4, NULL);
        
        // Priority 2: Lowest priority verifier task (runs occasionally)
        xTaskCreate(gps_tail_task, "gps_tail", 4096, NULL, 2, NULL);
        
    } else {
        ESP_LOGE(TAG, "CRITICAL ERROR: Failed to create FreeRTOS objects!");
    }
}