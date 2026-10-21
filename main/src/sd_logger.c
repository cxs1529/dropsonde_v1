/**
 * ==============================================================================
 * @file    sd_logger.c
 * @brief   SD Card FATFS storage manager, file rotation, and debouncing.
 * 
 * @details
 * This module operates as the CONSUMER in our architecture. It receives messages
 * from `gps_queue` and writes them safely to the physical SD card.
 * 
 * CORE FEATURES:
 * 1. SPI DMA: Uses `SPI_DMA_CH_AUTO` for high-speed hardware memory transfers.
 * 2. Hardware Debouncing: Tracks physical switch states via FreeRTOS ticks. 
 *    Requires the pin to be stable for 5 seconds before opening/closing files.
 * 3. 1-Minute Rule: Pausing logging for >60s automatically increments the sequence 
 *    number to open a fresh `GPSDATAx.dat` file upon resuming.
 * 4. O(1) Metadata: Updates a hidden `.META` file with line counts so the web 
 *    server doesn't have to read massive files just to render the UI.
 * 5. Memory Protection: Uses `setvbuf` for 4KB RAM block buffering and `fsync()` 
 *    every 30s to commit data to physical flash without wearing out the card.
 * ==============================================================================
 */

#include "sd_logger.h"
#include "config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "SD_LOG";
static sdmmc_card_t *sd_card = NULL;
static char active_log_filename[32] = "";

/** 
 * @brief Structure caching metadata in internal RAM for the open file.
 * We track total lines to avoid counting `\n` characters on 10MB text files.
 */
typedef struct {
    long lines;             ///< Total count of recorded telemetry lines
    char first_record[64];  ///< Timestamp & position of first log line
    char last_record[64];   ///< Timestamp & position of latest log line
} log_meta_t;

static log_meta_t current_meta = {0, "N/A", "N/A"};

/** 
 * @brief Formats a clean, abbreviated metadata summary string for the Web UI.
 * Extracts just Date, Time, and Decimal Degree coordinates from the full payload.
 */
static void extract_metadata_record(gps_msg_t *msg, char* dest) {
    snprintf(dest, 64, "%02d/%02d/%02d %02d:%02d:%02d Lat: %.5f Lon: %.5f", 
             msg->month, msg->day, msg->year, msg->hour, msg->min, msg->sec, msg->lat, msg->lon);
}

/** 
 * @brief Writes RAM metadata structure into the hidden `.META` cache file.
 */
static void save_metadata(void) {
    if (strlen(active_log_filename) == 0) return;
    char meta_path[128];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.META", META_DIR, active_log_filename);
    FILE *fm = fopen(meta_path, "w");
    if (fm) {
        // Output format: Line 1 = count, Line 2 = initial record, Line 3 = final record
        fprintf(fm, "%ld\n%s\n%s\n", current_meta.lines, current_meta.first_record, current_meta.last_record);
        fclose(fm);
    }
}

/** 
 * @brief Restores metadata into RAM if appending/resuming an existing file.
 */
static void load_metadata(void) {
    current_meta.lines = 0; 
    strcpy(current_meta.first_record, "N/A"); 
    strcpy(current_meta.last_record, "N/A");
    
    if (strlen(active_log_filename) == 0) return;
    char meta_path[128];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.META", META_DIR, active_log_filename);
    FILE *fm = fopen(meta_path, "r");
    if (fm) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fm)) current_meta.lines = atol(buf);
        if (fgets(buf, sizeof(buf), fm)) { buf[strcspn(buf, "\r\n")] = '\0'; strncpy(current_meta.first_record, buf, 63); }
        if (fgets(buf, sizeof(buf), fm)) { buf[strcspn(buf, "\r\n")] = '\0'; strncpy(current_meta.last_record, buf, 63); }
        fclose(fm);
    }
}

/** 
 * @brief Scans /sdcard/DATA for `GPSDATAx.dat` files and increments index x.
 * Ensures that rebooting the system never overwrites past data.
 */
static void get_next_filename(char *filename_out, size_t max_len) {
    DIR *dir = opendir(DATA_DIR);
    int max_index = 0;
    if (dir != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            int current_index = 0;
            // Parse NOAA format: GPSDATA1.dat, GPSDATA2.dat
            if (sscanf(ent->d_name, "GPSDATA%d.dat", &current_index) == 1) {
                if (current_index > max_index) max_index = current_index;
            }
        }
        closedir(dir);
    }
    // Return next filename in sequence
    snprintf(filename_out, max_len, "GPSDATA%d.dat", max_index + 1);
}

/**
 * @brief Mounts the physical SD card via the SPI bus.
 * @return ESP_OK on success, ESP_FAIL on wiring or format errors.
 */
esp_err_t init_sd_card(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // Do not auto-format to preserve data
        .max_files = 5, 
        .allocation_unit_size = 16 * 1024 // 16KB clusters optimize FAT access
    };
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI, 
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK, 
        .quadwp_io_num = -1, 
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    // SPI_DMA_CH_AUTO forces ESP32-C3 to automatically assign an available DMA channel
    if (spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) return ESP_FAIL;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS; 
    slot_config.host_id = host.slot;

    // Mount physical SD card to virtual POSIX path /sdcard
    if (esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card. Check wiring and FAT32 format.");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SD Card mounted successfully.");
    mkdir(DATA_DIR, 0777); // Ensure user data folder exists
    mkdir(META_DIR, 0777); // Ensure hidden system metadata folder exists
    return ESP_OK;
}

/**
 * @brief Automatically deletes metadata for files the user removed via PC.
 * Maintains the 1:1 relationship between raw logs and .META cache files.
 */
void cleanup_orphaned_metadata(void) {
    DIR *dir = opendir(META_DIR);
    if (!dir) return;
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *ext_ptr = strstr(ent->d_name, ".META");
        if (ext_ptr != NULL) {
            char base_name[64];
            size_t base_len = ext_ptr - ent->d_name;
            strncpy(base_name, ent->d_name, base_len); 
            base_name[base_len] = '\0';
            
            // Check if corresponding raw log file exists in /DATA
            char data_path[300]; 
            snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, base_name);
            struct stat st;
            
            // stat() fails if the file doesn't exist -> delete the orphan .META file
            if (stat(data_path, &st) != 0) {
                char meta_path[300]; 
                snprintf(meta_path, sizeof(meta_path), "%s/%s", META_DIR, ent->d_name);
                ESP_LOGI(TAG, "Orphaned metadata detected. Deleting: %s", meta_path);
                unlink(meta_path);
            }
        }
    }
    closedir(dir);
}

/** 
 * @brief Main RTOS Task for consuming queue data and writing to the SD Card.
 * 
 * @details
 * This loop manages the 5-second software debouncing. When the pin changes state,
 * it waits 5000 milliseconds to verify the switch isn't bouncing. If validated, 
 * it triggers file system opens/closes and executes 30-second fsync intervals.
 */
void gps_log_task(void *arg) {
    gps_msg_t msg; 
    FILE *f = NULL;
    
    // FreeRTOS timers for state machines
    TickType_t last_sync_time = xTaskGetTickCount();
    TickType_t pause_start_tick = 0, resume_start_tick = 0;   
    TickType_t actual_pause_time = 0, last_reminder_tick = 0;  
    
    bool logical_logging_state = false; 
    bool last_physical_state = (gpio_get_level(LOG_SWITCH_PIN) == 0);
    bool threshold_warned = false; // Tracks if threshold warning was already printed

    if (sd_card != NULL) get_next_filename(active_log_filename, sizeof(active_log_filename));

    while (1) {
        // Wait up to 1000ms for queue messages. The timeout ensures our software timers 
        // keep executing even if the GPS loses lock and the queue runs dry.
        BaseType_t received = xQueueReceive(gps_queue, &msg, pdMS_TO_TICKS(1000));
        TickType_t current_time = xTaskGetTickCount();
        
        // Read raw switch state (Internal pull-up means LOW = Switch CLOSED/ON)
        bool physical_switch_is_logging = (gpio_get_level(LOG_SWITCH_PIN) == 0);

        // --- Edge Detection Terminal Output ---
        if (physical_switch_is_logging && !last_physical_state) {
            printf("\n>>> LOG SWITCH ACTIVE. Waiting 5s to validate...\n\n");
        } else if (!physical_switch_is_logging && last_physical_state) {
            printf("\n>>> LOG SWITCH PAUSED. Waiting 5s to validate...\n\n");
            threshold_warned = false; // Reset 60-second warning flag
        }
        last_physical_state = physical_switch_is_logging;

        // --- 5-Second Debouncing & State Machine ---
        if (!physical_switch_is_logging) {
            resume_start_tick = 0; // Reset opposite timer
            
            if (logical_logging_state) {
                if (pause_start_tick == 0) pause_start_tick = current_time;
                
                // Validate switch remained physically OFF for 5000ms continuously
                if ((current_time - pause_start_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_logging_state = false;
                    actual_pause_time = current_time; 
                    last_reminder_tick = current_time; 
                    
                    if (f != NULL) {
                        fflush(f);        // Flush C-library RAM cache
                        fsync(fileno(f)); // Force physical SD Flash sync
                        fclose(f); 
                        f = NULL;
                        save_metadata();  // Save line counts to disk
                        ESP_LOGI(TAG, "Logging stopped and secured.");
                    }
                }
            } else {
                // Currently in PAUSED state. Evaluate elapsed duration.
                uint32_t elapsed_pause_sec = (current_time - actual_pause_time) * portTICK_PERIOD_MS / 1000;
                
                // Print explicit warning if the 60-second threshold is crossed
                if (actual_pause_time > 0 && elapsed_pause_sec >= 60 && !threshold_warned) {
                    char next_file[32]; 
                    get_next_filename(next_file, sizeof(next_file));
                    printf("\n*** PAUSE THRESHOLD EXCEEDED (%lu seconds). Resuming logging will create NEW FILE: %s ***\n\n", 
                           (unsigned long)elapsed_pause_sec, next_file);
                    threshold_warned = true;
                }

                // Print console reminder every 30 seconds with total elapsed pause duration
                if ((current_time - last_reminder_tick) >= pdMS_TO_TICKS(30000)) {
                    printf("--- REMINDER: Logging PAUSED for %lu seconds. Flip switch to resume. ---\n", 
                           (unsigned long)elapsed_pause_sec);
                    last_reminder_tick = current_time;
                }
            }
        } else {
            pause_start_tick = 0; 
            
            if (!logical_logging_state) {
                if (resume_start_tick == 0) resume_start_tick = current_time;
                
                // Validate switch remained physically ON for 5000ms continuously
                if ((current_time - resume_start_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_logging_state = true;
                    
                    // 1-Minute Rule: If paused > 60 seconds, create new file
                    if (actual_pause_time > 0 && (current_time - actual_pause_time) > pdMS_TO_TICKS(60000)) {
                        get_next_filename(active_log_filename, sizeof(active_log_filename));
                        ESP_LOGI(TAG, "Paused > 1 min. Starting NEW file: %s", active_log_filename);
                    } else if (actual_pause_time > 0) {
                        ESP_LOGI(TAG, "Paused < 1 min. Resuming EXISTING file: %s", active_log_filename);
                    }
                    
                    if (sd_card != NULL) {
                        char data_path[128]; 
                        snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
                        f = fopen(data_path, "a"); // Append mode
                        
                        if (f != NULL) { 
                            setvbuf(f, NULL, _IOFBF, 4096); // Expand C buffer to 4KB block size
                            load_metadata(); 
                            last_sync_time = current_time; 
                        }
                    }
                }
            }
        }

        // --- Output Processing & Telemetry Export ---
        if (received == pdTRUE) {
            
            // Console Output Formatting
            if (logical_logging_state) {
                printf("%s  [LOGGING : %s]\n", msg.payload, active_log_filename);
            } else {
                printf("%s  [PAUSED]\n", msg.payload);
            }
            fflush(stdout);

            // Export telemetry to shared memory.
            // Mutex ensures Web Server never reads half-written data.
            xSemaphoreTake(live_data_mutex, portMAX_DELAY);
            memcpy(&live_gps_data, &msg, sizeof(gps_msg_t));
            live_is_logging = logical_logging_state;
            strncpy(live_active_file, active_log_filename, sizeof(live_active_file)); // Share active filename
            xSemaphoreGive(live_data_mutex);

            if (logical_logging_state && f != NULL) {
                fprintf(f, "%s\n", msg.payload);
                
                // Update metadata tracking dynamically
                current_meta.lines++;
                extract_metadata_record(&msg, current_meta.last_record);
                if (current_meta.lines == 1) {
                    extract_metadata_record(&msg, current_meta.first_record);
                }

                // Execute fsync every 30 seconds to protect flash memory
                if ((current_time - last_sync_time) > pdMS_TO_TICKS(30000)) {
                    fflush(f); 
                    fsync(fileno(f)); 
                    save_metadata(); 
                    last_sync_time = current_time;
                }
            }
        }
    }
}

/** 
 * @brief Debugging Task: Reads the final lines of the file directly from the SD card.
 * @details
 * Seeks to the end of the active file to prove that the FAT filesystem is 
 * successfully recording data to the physical flash memory.
 */
void gps_tail_task(void *arg) {
    char last_lines[10][MAX_LINE_LEN]; 
    char temp_buf[MAX_LINE_LEN];
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Run once every 60 seconds
        if (sd_card == NULL || strlen(active_log_filename) == 0) continue;

        char data_path[128]; 
        snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
        FILE *f = fopen(data_path, "r");
        if (f == NULL) continue;

        // Seek near end of file to avoid large memory allocations
        fseek(f, 0, SEEK_END); 
        long size = ftell(f);
        long offset = size > 1500 ? size - 1500 : 0;
        fseek(f, offset, SEEK_SET);

        if (offset > 0) fgets(temp_buf, sizeof(temp_buf), f); // Discard partial fragmented line
        
        // Read remaining lines into a circular buffer
        int idx = 0, count = 0;
        while (fgets(temp_buf, sizeof(temp_buf), f) != NULL) {
            strncpy(last_lines[idx], temp_buf, MAX_LINE_LEN - 1);
            last_lines[idx][MAX_LINE_LEN - 1] = '\0';
            idx = (idx + 1) % 10; 
            count++;
        }
        fclose(f);

        printf("\n--- VERIFICATION: LAST %d LINES OF %s ---\n", count > 10 ? 10 : count, active_log_filename);
        int start_idx = count > 10 ? idx : 0;
        int lines_to_print = count > 10 ? 10 : count;
        
        for (int i = 0; i < lines_to_print; i++) {
            // fgets preserves the newline character, so no '\n' is needed in printf
            printf("%s", last_lines[(start_idx + i) % 10]);
        }
        printf("--------------------------------------------------\n\n");
    }
}