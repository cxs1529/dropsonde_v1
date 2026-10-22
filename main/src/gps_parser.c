/**
 * ==============================================================================
 * @file    gps_parser.c
 * @brief   GPS UART communication, NMEA sentence parsing, and RTC clock sync.
 * 
 * @details
 * Operates as the PRODUCER task in the FreeRTOS architecture.
 * - Reads incoming bytes from UART1 and buffers them into complete NMEA lines.
 * - Validates checksums using minmea and parses GGA, GSA, and RMC sentences.
 * - Starts at 01/01/70 00:00:00 if no GPS time is available, incrementing by 1s.
 * - Synchronizes the internal RTC immediately when valid GPS time is received, 
 *   even if a geographical fix hasn't been acquired yet (via module RTC).
 * - Generates two-line telemetry entries formatted for NOAA requirements.
 * ==============================================================================
 */

#include "gps_parser.h"
#include "config.h"
#include "minmea.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h> 
#include <time.h>     
#include <math.h>     

#define BUF_SIZE (1024)

/** 
 * @brief Internal cache to preserve state across fragmented NMEA sentences.
 * The GPS outputs data across multiple sentences per second. We must cache 
 * precision and fix data until the RMC sentence arrives to compile a full record.
 */
typedef struct {
    float pdop;         ///< Position Dilution of Precision (multiplicative error factor)
    int fix_type;       ///< 0 = No Fix, 2 = 2D Fix, 3 = 3D Fix
    int fix_quality;    ///< 0 = Invalid, 1 = GPS Fix, 2 = DGPS Fix
} gps_state_t;

static gps_state_t current_gps = { .pdop = 0.0, .fix_type = 0, .fix_quality = 0 };

/**
 * @brief Converts raw NMEA DDMM.MMMM format into separated Degrees and Minutes.
 */
static void convert_coordinate(float raw_coord, char pos_char, char neg_char, int *degrees, float *minutes, char *direction) {
    *direction = raw_coord >= 0 ? pos_char : neg_char;
    float abs_coord = raw_coord < 0 ? -raw_coord : raw_coord;
    *degrees = (int)(abs_coord / 100);
    *minutes = abs_coord - (*degrees * 100);
}

/** 
 * @brief Parses GGA (Global Positioning System Fix Data) sentence.
 * Extracts the lock quality indicator (Standard vs DGPS).
 */
static void handle_gga_sentence(const char *line) {
    struct minmea_sentence_gga frame;
    if (minmea_parse_gga(&frame, line)) {
        current_gps.fix_quality = frame.fix_quality;
    }
}

/** 
 * @brief Parses GSA (GPS DOP and Active Satellites) sentence.
 * Extracts 2D/3D status and structural geometric error (PDOP).
 */
static void handle_gsa_sentence(const char *line) {
    struct minmea_sentence_gsa frame;
    if (minmea_parse_gsa(&frame, line)) {
        if (frame.pdop.scale != 0) {
            current_gps.pdop = minmea_tofloat(&frame.pdop);
        }
        current_gps.fix_type = frame.fix_type;
    }
}

/** 
 * @brief Fallback generator triggered when UART is disconnected or completely silent.
 * Generates an uptime-based 1970 timestamp using the RTOS tick count.
 */
static void generate_rtc_dummy_message(void) {
    // Calculate seconds since the ESP32 booted
    time_t boot_sec = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    struct tm fake_tm; 
    gmtime_r(&boot_sec, &fake_tm);
    
    gps_msg_t msg = {0};
    msg.fix_type = 0; 
    msg.pdop = 0.0;
    msg.lat = 0.0;
    msg.lon = 0.0;
    strcpy(msg.fix_desc, "no fix");
    
    // Force date to 1970 (POSIX epoch) and overlay uptime hours/mins/secs
    msg.year = 70;
    msg.month = 1;
    msg.day = fake_tm.tm_mday;
    msg.hour = fake_tm.tm_hour;
    msg.min = fake_tm.tm_min;
    msg.sec = fake_tm.tm_sec;
    
    // Initialize sensor placeholders to 0
    msg.pressure = 0.0;
    msg.temperature = 0.0;
    msg.conductivity = 0.0;

    snprintf(msg.payload, sizeof(msg.payload), 
        "%02d/%02d/%02d  %02d:%02d:%02d  00 00.000000N  000 00.000000E  PDOP: 00.0 FIX TYPE: 0 (no fix)\n"
        "P= %05.0f  T= %05.0f  C= %05.0f",
        msg.month, msg.day, msg.year, msg.hour, msg.min, msg.sec,
        msg.pressure, msg.temperature, msg.conductivity
    );
    
    // Dispatch to SD logger
    xQueueSend(gps_queue, &msg, 0);
}

/** 
 * @brief Parses RMC sentences, syncs the internal RTC, and dispatches the final payload.
 */
static bool handle_rmc_sentence(const char *line) {
    struct minmea_sentence_rmc frame;
    if (!minmea_parse_rmc(&frame, line)) return false; 
    
    gps_msg_t msg = {0};

    // --- OS RTC SYNC ---
    // Even if satellites haven't triangulated a position yet, the GPS module's 
    // internal battery-backed RTC usually transmits the correct UTC date and time.
    // We immediately lock the ESP32's system clock to this time so the SD file manager 
    // can generate the correct chronological filename.
    if (frame.date.year > 0) {
        struct tm t = {0};
        t.tm_year = frame.date.year + 100; // Convert 2-digit year to POSIX year
        t.tm_mon  = frame.date.month - 1;  // Convert 1-12 range to 0-11 range
        t.tm_mday = frame.date.day;
        t.tm_hour = frame.time.hours;
        t.tm_min  = frame.time.minutes;
        t.tm_sec  = frame.time.seconds;
        
        struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }

    int lat_deg = 0, lon_deg = 0;
    float lat_min = 0.0, lon_min = 0.0;
    char lat_dir = 'N', lon_dir = 'E';

    if (frame.valid) {
        // --- SATELLITE FIX ACQUIRED ---
        // Populate the telemetry message with the actual geographical data.
        msg.year = frame.date.year;
        msg.month = frame.date.month;
        msg.day = frame.date.day;
        msg.hour = frame.time.hours;
        msg.min = frame.time.minutes;
        msg.sec = frame.time.seconds;

        // Convert coordinates for the Web Dashboard JSON
        msg.lat = minmea_tocoord(&frame.latitude);
        msg.lon = minmea_tocoord(&frame.longitude);
        if (isnan(msg.lat)) msg.lat = 0.0;
        if (isnan(msg.lon)) msg.lon = 0.0;

        // Convert coordinates for the physical SD card log string
        convert_coordinate(minmea_tofloat(&frame.latitude), 'N', 'S', &lat_deg, &lat_min, &lat_dir);
        convert_coordinate(minmea_tofloat(&frame.longitude), 'E', 'W', &lon_deg, &lon_min, &lon_dir);
        
        msg.pdop = current_gps.pdop;
        msg.fix_type = current_gps.fix_type;
        strcpy(msg.fix_desc, current_gps.fix_type == 3 ? "3D fix" : (current_gps.fix_type == 2 ? "2D fix" : "no fix"));
    } else {
        // --- NO SATELLITE FIX (SEARCHING) ---
        // NOAA requirements: If no fix is available, fall back to 1970 uptime logic
        // for the log file output (even if the RTC is synced internally).
        time_t boot_sec = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        struct tm fake_tm; 
        gmtime_r(&boot_sec, &fake_tm);

        msg.year = 70;
        msg.month = 1;
        msg.day = fake_tm.tm_mday;
        msg.hour = fake_tm.tm_hour;
        msg.min = fake_tm.tm_min;
        msg.sec = fake_tm.tm_sec;

        msg.lat = 0.0; 
        msg.lon = 0.0;
        msg.pdop = 0.0;
        msg.fix_type = 0;
        strcpy(msg.fix_desc, "no fix");

        // Initialize sensor placeholders to 0 (to be replaced by I2C/SPI sensor reads later)
        msg.pressure = 0.0;
        msg.temperature = 0.0;
        msg.conductivity = 0.0;
    }

    // Assemble the two-line output string
    snprintf(msg.payload, sizeof(msg.payload), 
            "%02d/%02d/%02d  %02d:%02d:%02d  %02d %09.6f%c  %03d %09.6f%c  PDOP: %04.1f FIX TYPE: %d (%s)\n"
            "P= %05.0f  T= %05.0f  C= %05.0f",
            msg.month, msg.day, msg.year, msg.hour, msg.min, msg.sec,
            lat_deg, lat_min, lat_dir, lon_deg, lon_min, lon_dir,
            msg.pdop, msg.fix_type, msg.fix_desc,
            msg.pressure, msg.temperature, msg.conductivity
    );
    
    // Dispatch to the queue for the SD logger to consume
    xQueueSend(gps_queue, &msg, 0); 
    return true; 
}

/** 
 * @brief Validates NMEA checksums and routes sentences to sub-parsers.
 */
static bool process_nmea_line(const char *line) {
    if (!minmea_check(line, false)) return false; 
    switch (minmea_sentence_id(line, false)) {
        case MINMEA_SENTENCE_GGA: handle_gga_sentence(line); return false;
        case MINMEA_SENTENCE_GSA: handle_gsa_sentence(line); return false;
        case MINMEA_SENTENCE_RMC: return handle_rmc_sentence(line);
        default: return false; 
    }
}

/** 
 * @brief Configures ESP-IDF hardware UART driver for standard 9600 baud NMEA.
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
    uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, UART1_TXD_PIN, UART1_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/** 
 * @brief RTOS Task: Constantly polls UART FIFO buffer, parsing data as it arrives.
 */
void gps_rx_task(void *arg) {
    init_gps_uart();
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    char line_buffer[MAX_LINE_LEN]; 
    int line_len = 0;
    TickType_t last_msg_tick = xTaskGetTickCount();
    
    while (1) {
        // Read available bytes (blocking up to 100ms)
        int length = uart_read_bytes(UART_NUM_1, data, BUF_SIZE - 1, 100 / portTICK_PERIOD_MS);
        
        for (int i = 0; i < length; i++) {
            char c = (char)data[i];
            // Split incoming stream by carriage return / newline
            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line_buffer[line_len] = '\0'; 
                    if (process_nmea_line(line_buffer)) {
                        last_msg_tick = xTaskGetTickCount(); // Reset fallback timer on success
                    }
                    line_len = 0;                  
                }
            } else {
                if (line_len < MAX_LINE_LEN - 1) line_buffer[line_len++] = c;
            }
        }
        
        // Software Fallback: Force a dummy 1Hz update if hardware UART goes completely dead
        if ((xTaskGetTickCount() - last_msg_tick) >= pdMS_TO_TICKS(1000)) {
            generate_rtc_dummy_message();
            last_msg_tick = xTaskGetTickCount();
        }
    }
}