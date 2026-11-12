/**
 * ==============================================================================
 * @file    config.h
 * @brief   Global hardware configurations, pin definitions, and shared RTOS handles.
 * 
 * @details
 * This file centralizes all hardware mapping for the ESP32-C3 microcontroller,
 * eliminating "magic numbers" scattered throughout the codebase. It defines the 
 * GPIO pins for SPI (SD Card), I2C (BME280 Sensor), UART (GPS), and the physical 
 * switches. It also declares the global Mutexes and structures used to safely 
 * pass data between the various FreeRTOS tasks (e.g., GPS parser to Web Server).
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
#define LOG_SWITCH_PIN  (GPIO_NUM_8)  ///< Controls SD card logging activity (debounced to 5 seconds)
#define WIFI_SWITCH_PIN (GPIO_NUM_2)  ///< Controls Wi-Fi Access Point RF power (debounced to 10 seconds)

/**
 * GPS UART Pins: Routed via the ESP32-C3 GPIO Matrix.
 * Connects to the u-blox NEO-6M or similar NMEA 0183 compatible GPS module.
 */
#define UART1_RXD_PIN   (GPIO_NUM_6)  ///< ESP32 RX <- Connects to GPS Module TX
#define UART1_TXD_PIN   (GPIO_NUM_7)  ///< ESP32 TX -> Connects to GPS Module RX
#define UART1_BAUD      (9600)        ///< Standard default baud rate for legacy GPS modules

/**
 * SD Card SPI Bus Pins:
 * Configured for SPI mode on the ESP32-C3 using auto-allocated DMA.
 */
#define PIN_NUM_CLK     (GPIO_NUM_0)  ///< SPI Clock (SCK)
#define PIN_NUM_MISO    (GPIO_NUM_1)  ///< Master In Slave Out (DO)
#define PIN_NUM_MOSI    (GPIO_NUM_3)  ///< Master Out Slave In (DI)
#define PIN_NUM_CS      (GPIO_NUM_4)  ///< Chip Select (SS)

/**
 * Environmental Sensor I2C Bus Pins:
 * Configured for the internal I2C Master peripheral running at 100kHz.
 */
#define I2C_SDA_PIN     (GPIO_NUM_5)  ///< I2C Data Line (SDA) for BME280/BMP280
#define I2C_SCL_PIN     (GPIO_NUM_10) ///< I2C Clock Line (SCL) for BME280/BMP280

// =================--- NETWORK CONFIGURATION ---=================
#define WIFI_AP_SSID    "DROPSONDE_WIFI"   ///< SSID broadcast by the ESP32 Access Point
#define WIFI_AP_PASS    "dropsonde"        ///< WPA2 Password for the Access Point
#define MAX_STA_CONN    4                  ///< Max simultaneous clients allowed to prevent socket exhaustion

// =================--- FILE SYSTEM PATHS ---=================
#define MOUNT_POINT     "/sdcard"            ///< Virtual root mount for FATFS
#define DATA_DIR        MOUNT_POINT "/DATA"  ///< Target directory for raw .dat and sensor logs
#define META_DIR        MOUNT_POINT "/.META" ///< Hidden folder for system cache, notes, and targets
#define MAX_LINE_LEN    (256)                ///< Maximum character buffer size for two-line payloads

// =================--- SHARED RTOS DATA ---=================

/**
 * @brief Discrete telemetry message passed across FreeRTOS task boundaries.
 * 
 * Contains both the raw pre-formatted two-line string for SD logging and 
 * parsed scalar fields used to populate the web server's JSON API.
 */
typedef struct {
    int year, month, day;      ///< Date fields extracted from RMC/RTC
    int hour, min, sec;        ///< UTC Time fields extracted from RMC/RTC
    float lat, lon;            ///< Coordinates converted to Decimal Degrees (dd.ddd)
    float pdop;                ///< Position Dilution of Precision (geometric error)
    int fix_type;              ///< 0 = No Fix, 2 = 2D Fix, 3 = 3D Fix
    char fix_desc[16];         ///< String description ("3D fix", "2D fix", "no fix")
    
    // --- Legacy / Future Sensor Placeholder Data ---
    float pressure;            
    float temperature;         
    float conductivity;        
    
    char payload[MAX_LINE_LEN];///< Two-line pre-formatted log string sent to the SD writer
} gps_msg_t;

/**
 * @brief Real-time struct holding the latest I2C sensor acquisitions.
 * Protected by `sensor_data_mutex` to allow asynchronous reads by the Web API and SD Logger.
 */
typedef struct {
    float temperature;         ///< Ambient temperature in Celsius (*C)
    float humidity;            ///< Relative Humidity in percentage (%)
    float pressure;            ///< Barometric pressure in hectoPascals (hPa)
    float voltage;             ///< Placeholder for future ADC battery voltage mapping
} sensor_data_t;

/* 
 * -----------------------------------------------------------------------------
 * EXTERN DECLARATIONS
 * These handles are instantiated in main.c but exposed globally here so that
 * the modular C files (web_server.c, sd_logger.c) can access them.
 * -----------------------------------------------------------------------------
 */
extern QueueHandle_t gps_queue;            ///< Queue transferring telemetry from Parser to Logger
extern SemaphoreHandle_t live_data_mutex;  ///< Mutex protecting shared GPS telemetry during web reads
extern gps_msg_t live_gps_data;            ///< Shared global variable holding latest GPS telemetry
extern bool live_is_logging;               ///< Shared global variable holding current SD logging state
extern char live_active_file[64];          ///< Shared global variable holding current active GPS filename
extern bool force_new_file_flag;           ///< Signal flag set by the Web UI to trigger a new deployment file

extern sensor_data_t current_sensor_data;  ///< Shared global variable holding latest BME280 telemetry
extern SemaphoreHandle_t sensor_data_mutex;///< Mutex protecting BME280 telemetry against race conditions

#endif // CONFIG_H
