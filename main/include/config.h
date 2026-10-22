/**
 * ==============================================================================
 * @file    config.h
 * @brief   Global hardware configurations, pin definitions, and shared RTOS handles.
 * 
 * @details
 * Centralizes all hardware pin assignments (tailored for the SparkFun Pro Micro 
 * ESP32-C3), networking credentials, virtual file system mount points, and 
 * shared FreeRTOS structures (Queues, Mutexes, and Flags) across modules.
 * ==============================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdbool.h>

// =================--- HARDWARE PIN MAP ---=================

/**
 * Control Switch Pins: Configured as GPIO inputs with internal pull-ups.
 * - Logic 0 (GND / Switch CLOSED) = Feature ACTIVE.
 * - Logic 1 (Floating / Switch OPEN) = Feature PAUSED or DISABLED.
 */
#define LOG_SWITCH_PIN  (GPIO_NUM_8)  ///< Controls SD card logging activity
#define WIFI_SWITCH_PIN (GPIO_NUM_2)  ///< Controls Wi-Fi Access Point RF power

/**
 * GPS UART Pins: Routed via the ESP32-C3 GPIO Matrix.
 */
#define UART1_RXD_PIN   (GPIO_NUM_6)  ///< ESP32 RX <- Connects to GPS Module TX
#define UART1_TXD_PIN   (GPIO_NUM_7)  ///< ESP32 TX -> Connects to GPS Module RX
#define UART1_BAUD      (9600)        ///< Standard default baud rate for u-blox NEO-6M

/**
 * SD Card SPI Bus Pins:
 * Configured for SPI mode on the ESP32-C3 using auto-allocated DMA.
 */
#define PIN_NUM_CLK     (GPIO_NUM_0)  ///< SPI Clock (SCK)
#define PIN_NUM_MISO    (GPIO_NUM_1)  ///< Master In Slave Out (DO)
#define PIN_NUM_MOSI    (GPIO_NUM_3)  ///< Master Out Slave In (DI)
#define PIN_NUM_CS      (GPIO_NUM_4)  ///< Chip Select (SS)


// =================--- NETWORK CONFIGURATION ---=================

#define WIFI_AP_SSID    "DROPSONDE_WIFI"   ///< SSID broadcast by ESP32 Access Point
#define WIFI_AP_PASS    "dropsonde"        ///< WPA2 Password for the Access Point
#define MAX_STA_CONN    4                  ///< Max simultaneous clients allowed


// =================--- FILE SYSTEM PATHS ---=================

#define MOUNT_POINT     "/sdcard"            ///< Virtual root mount for FATFS
#define DATA_DIR        MOUNT_POINT "/DATA"  ///< Target directory for raw .dat logs
#define META_DIR        MOUNT_POINT "/.META" ///< Hidden folder for system cache files
#define MAX_LINE_LEN    (256)                ///< Buffer size supporting two-line payloads


// =================--- SHARED RTOS DATA ---=================

/**
 * @brief Discrete telemetry message passed across FreeRTOS task boundaries.
 * Contains both the raw pre-formatted two-line string for SD logging and 
 * parsed scalar fields used to populate the web server's JSON API.
 */
typedef struct {
    int year, month, day;      ///< Date fields extracted from RMC/RTC
    int hour, min, sec;        ///< UTC Time fields extracted from RMC/RTC
    float lat, lon;            ///< Coordinates converted to Decimal Degrees (dd.ddd)
    float pdop;                ///< Position Dilution of Precision
    int fix_type;              ///< 0 = No Fix, 2 = 2D Fix, 3 = 3D Fix
    char fix_desc[16];         ///< String description ("3D fix", "2D fix", "no fix")
    
    // --- Future Sensor Integration ---
    float pressure;            ///< Pressure reading placeholder
    float temperature;         ///< Temperature reading placeholder
    float conductivity;        ///< Conductivity reading placeholder
    
    char payload[MAX_LINE_LEN];///< Two-line pre-formatted log string
} gps_msg_t;

// Extern declarations allow cross-module access to RTOS handles
extern QueueHandle_t gps_queue;            ///< Queue transferring telemetry from Parser to Logger
extern SemaphoreHandle_t live_data_mutex;  ///< Mutex protecting shared telemetry during web reads
extern gps_msg_t live_gps_data;            ///< Shared global variable holding latest telemetry
extern bool live_is_logging;               ///< Shared global variable holding logging state
extern char live_active_file[64];          ///< Shared global variable holding current active filename
extern bool force_new_file_flag;           ///< Signal flag to trigger a new deployment file

#endif // CONFIG_H