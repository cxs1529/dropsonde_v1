/**
 * ==============================================================================
 * @file    main.c
 * @brief   Application Entry Point & NOAA Dropsonde Operational Description.
 * 
 * @details
 * DEVICE OPERATIONAL DESCRIPTION:
 * -------------------------------
 * The NOAA/AOML Dropsonde Logger is an industrial-grade telemetry acquisition 
 * instrument for oceanic dropsondes built on the ESP32-C3 microcontroller.
 * 
 * 1. CLOCK SYNCHRONIZATION & 1Hz GUARANTEE:
 *    - The system boot initializes POSIX time to 01/01/70 00:00:00.
 *    - A 1Hz software timer increments this timestamp continuously.
 *    - As soon as the u-blox NEO-6M emits an RMC sentence with valid year data 
 *      (either from internal battery backup or satellite fix), `settimeofday()` 
 *      instantly locks the ESP32 internal clock to UTC.
 *    - Fallback records with `00 00.000000N 000 00.000000E` and `FIX TYPE: 0 (no fix)` 
 *      are generated every second when satellite lock is unavailable.
 * 
 * 2. TWO-LINE DATA LOGGING FORMAT:
 *    - Every 1-second record writes two lines to the SD file:
 *        Line 1: MM/DD/YY  HH:MM:SS  DD MM.MMMMMM[N/S]  DDD MM.MMMMMM[E/W]  PDOP: xx.x FIX TYPE: x (desc)
 *        Line 2: P= 00000  T= 00000  C= 00000
 * 
 * 3. FILE SYSTEM & STORAGE PIPELINE:
 *    - Data is stored in `/sdcard/DATA/GPSDATA_YYMMDDhhmmss.dat`.
 *    - Hardware switch GPIO 8 (with 5-second debounce) pauses/resumes logging.
 *    - Resuming after >60 seconds automatically initiates a new deployment file.
 *    - A web dashboard button allows triggering a new deployment file on demand.
 *    - Initial metadata waits for the first valid fix before locking starting position.
 *    - RAM is buffered in 4KB blocks (`setvbuf`) and synced via `fsync()` every 30 seconds.
 * 
 * 4. FreeRTOS TASK ARCHITECTURE:
 *    - Priority 10: `gps_rx_task` (UART1 reading and NMEA parsing)
 *    - Priority 6 : `gps_log_task` (SD card file management and debouncing)
 *    - Priority 4 : `wifi_manager_task` (10s debounced Wi-Fi power control)
 *    - Priority 2 : `gps_tail_task` (Verification reader running every 60s)
 * ==============================================================================
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <sys/time.h>

#include "config.h"
#include "gps_parser.h"
#include "sd_logger.h"
#include "web_server.h"

static const char *TAG = "MAIN";

// Global handles instantiated once in RAM
QueueHandle_t gps_queue = NULL;
SemaphoreHandle_t live_data_mutex = NULL;
gps_msg_t live_gps_data = {0}; 
bool live_is_logging = false;
char live_active_file[64] = ""; 
bool force_new_file_flag = false; // Flag signaled by Web UI for new deployments

/** 
 * @brief Configures GPIO switches with internal pull-ups (Active LOW).
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

void app_main(void) {
    ESP_LOGI(TAG, "Booting NOAA/AOML Dropsonde Logger...");

    // 1. Initialize System Clock to 01/01/70 00:00:00 (Epoch 0)
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // 2. Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      nvs_flash_init();
    }

    // 3. Initialize Hardware & Storage
    init_switches();
    if (init_sd_card() == ESP_OK) {
        cleanup_orphaned_metadata(); 
    }
    init_wifi_base(); 

    // 4. Create RTOS Primitives
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
        
        // Priority 2: Lowest priority verifier task (Increased stack to 8192 for FATFS/SPI)
        xTaskCreate(gps_tail_task, "gps_tail", 8192, NULL, 2, NULL);
    } else {
        ESP_LOGE(TAG, "CRITICAL ERROR: Failed to create FreeRTOS objects!");
    }
}