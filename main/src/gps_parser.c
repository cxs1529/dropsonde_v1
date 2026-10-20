/**
 * ==============================================================================
 * @file    gps_parser.c
 * @brief   Implementation of GPS UART reading, NMEA parsing, and RTC syncing.
 * 
 * @details
 * This file acts as the primary PRODUCER in our system. It continuously reads
 * bytes from UART1, detects sentences ending in \n, and uses minmea to parse them.
 * It also maintains the internal system clock (RTC) using accurate satellite time.
 * ==============================================================================
 */

#include "gps_parser.h"
#include "config.h"
#include "minmea.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h> // For settimeofday() - sets internal clock
#include <time.h>     // For time(), gmtime_r() - reads internal clock
#include <math.h>     // For isnan() - validates floating point parsing

#define BUF_SIZE (1024)

/** 
 * @brief Local state cache preserving values across different NMEA sentences.
 * GPS modules spread data across multiple sentences. We cache PDOP (from GSA) 
 * and Fix Quality (from GGA) here until the RMC sentence arrives with the location.
 */
typedef struct {
    float pdop;         ///< Position Dilution of Precision (multiplicative error factor)
    int fix_type;       ///< 1 = No Fix, 2 = 2D Fix (Flat), 3 = 3D Fix (Includes Altitude)
    int fix_quality;    ///< 0 = Invalid, 1 = GPS Fix, 2 = DGPS Fix
} gps_state_t;

// Initialize state assuming no satellite lock at boot
static gps_state_t current_gps = { .pdop = 0.0, .fix_type = 1, .fix_quality = 0 };

/** 
 * @brief Parses GGA (Global Positioning System Fix Data) sentence.
 * GGA provides info about satellite lock quality (Standard vs DGPS).
 */
static void handle_gga_sentence(const char *line) {
    struct minmea_sentence_gga frame;
    // Attempt to parse line into GGA structure
    if (minmea_parse_gga(&frame, line)) {
        // Save quality: 0=invalid, 1=GPS fix, 2=DGPS fix
        current_gps.fix_quality = frame.fix_quality;
    }
}

/** 
 * @brief Parses GSA (GPS DOP and Active Satellites) sentence.
 * GSA provides 2D/3D status and structural geometric error (PDOP).
 */
static void handle_gsa_sentence(const char *line) {
    struct minmea_sentence_gsa frame;
    // Attempt to parse line into GSA structure
    if (minmea_parse_gsa(&frame, line)) {
        // minmea uses scaled integers for memory safety. Convert to float if scale exists.
        if (frame.pdop.scale != 0) {
            current_gps.pdop = minmea_tofloat(&frame.pdop);
        }
        // Save fix dimension: 1=none, 2=2D, 3=3D
        current_gps.fix_type = frame.fix_type;
    }
}

/** 
 * @brief Fallback generator triggered when GPS signal is lost or disconnected.
 * Uses the ESP32's internal RTC time (previously synchronized) to output a 1Hz log payload.
 */
static void generate_rtc_dummy_message(void) {
    time_t now; 
    time(&now); // Read current UNIX timestamp from ESP32 system clock
    struct tm timeinfo; 
    gmtime_r(&now, &timeinfo); // Convert raw timestamp into readable UTC date/time fields
    
    gps_msg_t msg = {0};
    msg.fix_type = 1;     // Force "No fix" status
    msg.pdop = 0.0;       // Reset error factor
    msg.lat = 0.0;        // Set dummy coordinates
    msg.lon = 0.0;
    strcpy(msg.fix_desc, "No fix");
    
    // Check if system clock has been synced at least once since boot (Year 1970 check)
    if (timeinfo.tm_year > 100) { // tm_year counts years since 1900. 2026 - 1900 = 126
        msg.year = timeinfo.tm_year - 100; // Convert to 2-digit format (126 -> 26)
        msg.month = timeinfo.tm_mon + 1;   // Convert POSIX 0-11 range to 1-12 range
        msg.day = timeinfo.tm_mday;
        msg.hour = timeinfo.tm_hour;
        msg.min = timeinfo.tm_min;
        msg.sec = timeinfo.tm_sec;
    }
    
    // Format payload with 0.000000 coordinates
    snprintf(msg.payload, sizeof(msg.payload), 
        "%02d/%02d/%02d  %02d:%02d:%02d  %.6f  %.6f  PDOP: %.1f FIX TYPE: %d (%s)",
        msg.month, msg.day, msg.year, msg.hour, msg.min, msg.sec,
        msg.lat, msg.lon, msg.pdop, msg.fix_type, msg.fix_desc
    );
    
    // Dispatch to consumer queue. 0 timeout ensures parser never blocks if queue is full.
    xQueueSend(gps_queue, &msg, 0);
}

/** 
 * @brief Parses RMC (Recommended Minimum Navigation Information) sentence.
 * This is the primary driver. It extracts date/time/coordinates, resyncs system RTC,
 * converts coordinates to Decimal Degrees (dd.ddd), and compiles the final payload.
 */
static bool handle_rmc_sentence(const char *line) {
    struct minmea_sentence_rmc frame;
    // Return false if the line is not a valid RMC sentence
    if (!minmea_parse_rmc(&frame, line)) return false; 
    
    gps_msg_t msg = {0};

    // frame.valid is a boolean flag sent directly by the GPS hardware ('A'=valid, 'V'=invalid)
    if (frame.valid) {
        // --- SATELLITE FIX PRESENT: Sync Internal RTC ---
        struct tm t = {0};
        t.tm_year = frame.date.year + 100; // Convert 2-digit year (26) to POSIX year (126)
        t.tm_mon  = frame.date.month - 1;  // Convert 1-12 range to POSIX 0-11 range
        t.tm_mday = frame.date.day;
        t.tm_hour = frame.time.hours;
        t.tm_min  = frame.time.minutes;
        t.tm_sec  = frame.time.seconds;
        
        // Convert broken-down struct tm into seconds since epoch
        time_t timestamp = mktime(&t);
        struct timeval tv = { .tv_sec = timestamp, .tv_usec = 0 };
        // Instantly write satellite-accurate time to the ESP32 internal hardware clock
        settimeofday(&tv, NULL);
        
        msg.year = frame.date.year; 
        msg.month = frame.date.month; 
        msg.day = frame.date.day;
        msg.hour = frame.time.hours; 
        msg.min = frame.time.minutes; 
        msg.sec = frame.time.seconds;
        
        // Native minmea function converts DDMM.MMMM format directly to Decimal Degrees (dd.ddd)
        msg.lat = minmea_tocoord(&frame.latitude);
        msg.lon = minmea_tocoord(&frame.longitude);
        
        // Safety checks for math failures
        if (isnan(msg.lat)) msg.lat = 0.0;
        if (isnan(msg.lon)) msg.lon = 0.0;
        
    } else {
        // --- NO LOCK (Searching): Fallback to internal system RTC ---
        time_t now; time(&now);
        struct tm timeinfo; gmtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > 100) { 
            msg.year = timeinfo.tm_year - 100; 
            msg.month = timeinfo.tm_mon + 1; 
            msg.day = timeinfo.tm_mday;
            msg.hour = timeinfo.tm_hour; 
            msg.min = timeinfo.tm_min; 
            msg.sec = timeinfo.tm_sec;
        }
        msg.lat = 0.0; 
        msg.lon = 0.0;
        current_gps.fix_type = 1; // Mark fix as invalid
    }

    msg.pdop = current_gps.pdop;
    msg.fix_type = current_gps.fix_type;
    strcpy(msg.fix_desc, current_gps.fix_type == 3 ? "3D fix" : (current_gps.fix_type == 2 ? "2D fix" : "No fix"));
    
    // Assemble human-readable string representation for terminal & SD card file
    snprintf(msg.payload, sizeof(msg.payload), 
        "%02d/%02d/%02d  %02d:%02d:%02d  %.6f  %.6f  PDOP: %.1f FIX TYPE: %d (%s)",
        msg.month, msg.day, msg.year, msg.hour, msg.min, msg.sec,
        msg.lat, msg.lon, msg.pdop, msg.fix_type, msg.fix_desc
    );
    
    // Send discrete telemetry package to consumer task via queue
    xQueueSend(gps_queue, &msg, 0); 
    return true; // Return true to indicate a valid 1Hz cycle completed
}

/** 
 * @brief Validates NMEA checksums (*6A) and routes sentences to sub-parsers.
 */
static bool process_nmea_line(const char *line) {
    // Checksum check: drops sentences corrupted by electrical noise over UART lines
    if (!minmea_check(line, false)) return false; 
    
    // Sentence ID lookup: routes string to correct parser function
    switch (minmea_sentence_id(line, false)) {
        case MINMEA_SENTENCE_GGA: handle_gga_sentence(line); return false;
        case MINMEA_SENTENCE_GSA: handle_gsa_sentence(line); return false;
        case MINMEA_SENTENCE_RMC: return handle_rmc_sentence(line);
        default: return false; // Ignore irrelevant sentences (GSV, VTG, GLL)
    }
}

/** 
 * @brief Configures and installs the ESP-IDF hardware UART driver.
 */
static void init_gps_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = UART1_BAUD, 
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, 
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Allocate 2KB of internal ESP32 RAM for the background UART RX FIFO ring buffer
    uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    // Route UART1 to physical GPIO pins 6 and 7
    uart_set_pin(UART_NUM_1, UART1_TXD_PIN, UART1_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/** 
 * @brief Main FreeRTOS task loop for receiving and parsing GPS data.
 */
void gps_rx_task(void *arg) {
    init_gps_uart();
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    char line_buffer[MAX_LINE_LEN]; 
    int line_len = 0;
    
    TickType_t last_msg_tick = xTaskGetTickCount();
    
    while (1) {
        // Read available bytes from hardware FIFO. Blocks (yields CPU) for up to 100ms.
        int length = uart_read_bytes(UART_NUM_1, data, BUF_SIZE - 1, 100 / portTICK_PERIOD_MS);
        
        for (int i = 0; i < length; i++) {
            char c = (char)data[i];
            // If newline encountered, sentence is complete
            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line_buffer[line_len] = '\0'; // Cap string safely 
                    if (process_nmea_line(line_buffer)) {
                        last_msg_tick = xTaskGetTickCount(); // Reset 1Hz timer on valid RMC
                    }
                    line_len = 0; // Reset length counter for next sentence                 
                }
            } else {
                // Prevent array overflow if sentence exceeds maximum expected length
                if (line_len < MAX_LINE_LEN - 1) line_buffer[line_len++] = c;
            }
        }

        // Guaranteed 1Hz Fallback: If 1000ms passes with no RMC sentence, force an RTC log
        if ((xTaskGetTickCount() - last_msg_tick) >= pdMS_TO_TICKS(1000)) {
            generate_rtc_dummy_message();
            last_msg_tick = xTaskGetTickCount();
        }
    }
}