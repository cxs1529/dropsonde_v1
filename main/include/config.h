/**
 * ==============================================================================
 * @file    config.h
 * @brief   Global hardware configurations, pin definitions, and RTOS handles.
 * 
 * @details
 * This header acts as the central configuration hub for the entire project.
 * It defines:
 * 1. Physical GPIO assignments tailored for the SparkFun Pro Micro ESP32-C3.
 * 2. UART hardware parameters (Baud rate, Buffer allocation sizes).
 * 3. SPI Bus assignments for the SD card module.
 * 4. Wi-Fi Access Point credentials (SSID, Password, connection caps).
 * 5. Virtual Filesystem (VFS) POSIX directory paths on the SD Card.
 * 6. FreeRTOS cross-file handles using `extern` declarations to prevent 
 *    circular dependencies between modular .c files.
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
 * Control Switch Pins: Configured as inputs with internal pull-up resistors.
 * - Logic 0 (GND / Switch CLOSED) = Feature ACTIVE.
 * - Logic 1 (Floating / Switch OPEN) = Feature PAUSED or DISABLED.
 */
#define LOG_SWITCH_PIN  (GPIO_NUM_8)  ///< Controls SD card logging activity
#define WIFI_SWITCH_PIN (GPIO_NUM_2)  ///< Controls Wi-Fi Access Point RF power

/**
 * GPS UART Pins: Using ESP32-C3 GPIO Matrix flexibility.
 * Pins 6 & 7 avoid default hardware strapping pins and onboard LEDs.
 */
#define UART1_RXD_PIN   (GPIO_NUM_6)  ///< ESP32 RX <- Connects to GPS Module TX
#define UART1_TXD_PIN   (GPIO_NUM_7)  ///< ESP32 TX -> Connects to GPS Module RX
#define UART1_BAUD      (9600)        ///< Standard default baud rate for u-blox/NMEA

/**
 * SD Card SPI Bus Pins:
 * The ESP32-C3 lacks a native 4-bit SDMMC host controller, requiring SPI mode.
 */
#define PIN_NUM_CLK     (GPIO_NUM_0)  ///< SPI Clock (SCK)
#define PIN_NUM_MISO    (GPIO_NUM_1)  ///< Master In Slave Out (DO)
#define PIN_NUM_MOSI    (GPIO_NUM_3)  ///< Master Out Slave In (DI)
#define PIN_NUM_CS      (GPIO_NUM_4)  ///< Chip Select (SS)


// =================--- NETWORK CONFIGURATION ---=================

#define WIFI_AP_SSID    "DROPSONDE_WIFI"
#define WIFI_AP_PASS    "dropsonde"      ///< WPA2 Password for the Access Point
#define MAX_STA_CONN    4                  ///< Max simultaneous devices connected


// =================--- FILE SYSTEM PATHS ---=================

#define MOUNT_POINT     "/sdcard"            ///< Virtual root for the FAT system
#define DATA_DIR        MOUNT_POINT "/DATA"  ///< Target directory for user GPS logs
#define META_DIR        MOUNT_POINT "/.META" ///< Hidden folder for system cache files
#define MAX_LINE_LEN    (128)              ///< Max byte length for formatted string


// =================--- SHARED RTOS DATA ---=================

/**
 * @brief Discrete telemetry message passed across FreeRTOS task boundaries.
 * Contains both the raw pre-formatted string for the SD card and split scalar 
 * fields used to populate the web server's JSON API.
 */
typedef struct {
    int year, month, day;      ///< Date fields extracted from RMC/RTC
    int hour, min, sec;        ///< UTC Time fields extracted from RMC/RTC
    float lat, lon;            ///< Coordinates converted to Decimal Degrees (dd.ddd)
    float pdop;                ///< Position Dilution of Precision (lower = higher accuracy)
    int fix_type;              ///< 1 = No Fix, 2 = 2D Fix, 3 = 3D Fix
    char fix_desc[16];         ///< String description ("3D fix", "2D fix", "No fix")
    char payload[MAX_LINE_LEN];///< Complete pre-formatted log string
} gps_msg_t;

// Extern declarations allow the actual memory (instantiated in main.c) 
// to be referenced globally by any file that includes config.h
extern QueueHandle_t gps_queue;            ///< Queue handle transferring telemetry from Parser to Logger
extern SemaphoreHandle_t live_data_mutex;   ///< Mutex locking live_gps_data during cross-task reads
extern gps_msg_t live_gps_data;            ///< Shared global variable holding latest telemetry
extern bool live_is_logging;               ///< Shared global variable holding active logging status
extern char live_active_file[32];          ///< Shared global variable holding current active filename

#endif // CONFIG_H