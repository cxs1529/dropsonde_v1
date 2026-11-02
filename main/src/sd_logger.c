/**
 * ==============================================================================
 * @file    sd_logger.c
 * @brief   SD Card FATFS storage manager, file rotation, and two-line logging.
 * 
 * @details
 * Operates as the CONSUMER task in the FreeRTOS architecture.
 * - Mounts the FATFS filesystem via SPI using DMA channel auto-allocation.
 * - Generates filenames dynamically, utilizing index suffixes to prevent overwriting.
 * - Handles software debouncing (5 seconds) on the hardware switch.
 * - Executes periodic `fsync()` operations to protect flash memory life.
 * - Defers filename generation until the exact moment logging begins to allow
 *   the parser task to synchronize the system RTC in the background first.
 * ==============================================================================
 */

#include "sd_logger.h"
#include "config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "ff.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "SD_LOG";
static sdmmc_card_t *sd_card = NULL;
static char active_log_filename[64] = "";

/**
 * @brief RAM cache tracking file size, limits, and GPS coordinate locks.
 */
typedef struct {
    long lines;             ///< Number of payload lines in active file
    bool first_valid_found; ///< Prevents first_record from updating once a fix is locked
    char first_record[64];  ///< String representing the deployment drop location
    char last_record[64];   ///< String representing the most recent telemetry
} log_meta_t;

static log_meta_t current_meta = {0, false, "N/A", "N/A"};


static void auto_format_bytes(uint64_t bytes, char *out_str, size_t max_len) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = (double)bytes;

    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(out_str, max_len, "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(out_str, max_len, "%.2f %s", size, units[unit_idx]);
    }
}


/**
 * @brief Directly queries FatFs for cluster allocation (bypasses POSIX VFS path lookup)
 */
static bool get_sd_capacity_strings(char *total_str, char *used_str, char *free_str, size_t str_len) {
    FATFS *fs;
    DWORD fre_clust;

    // FatFs function to get free clusters on default/first physical drive ("0:")
    FRESULT res = f_getfree("0:", &fre_clust, &fs);
    if (res != FR_OK || fs == NULL) {
        // Fallback: try querying without drive number in case drive mapping is default
        res = f_getfree("", &fre_clust, &fs);
    }

    if (res != FR_OK || fs == NULL) {
        snprintf(total_str, str_len, "ERR (%d)", res);
        snprintf(used_str,  str_len, "ERR (%d)", res);
        snprintf(free_str,  str_len, "ERR (%d)", res);
        return false;
    }

    // Total and free sectors
    uint64_t tot_sect = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t fre_sect = (uint64_t)fre_clust * fs->csize;

    // Sector size (typically 512 bytes)
#if FF_MAX_SS != FF_MIN_SS
    uint64_t sector_size = fs->ssize;
#else
    uint64_t sector_size = 512;
#endif

    uint64_t total_bytes = tot_sect * sector_size;
    uint64_t free_bytes  = fre_sect * sector_size;
    uint64_t used_bytes  = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

    auto_format_bytes(total_bytes, total_str, str_len);
    auto_format_bytes(used_bytes,  used_str,  str_len);
    auto_format_bytes(free_bytes,  free_str,  str_len);
    return true;
}


/** 
 * @brief Strips down the full telemetry payload into a compact string for the UI metadata.
 */
static void extract_metadata_record(gps_msg_t *msg, char* dest) {
    snprintf(dest, 64, "%02d/%02d/%02d %02d:%02d:%02d Lat: %.5f Lon: %.5f", 
             msg->month, msg->day, msg->year, msg->hour, msg->min, msg->sec, msg->lat, msg->lon);
}

/** 
 * @brief Commits RAM metadata cache to physical SD `.META` file for rapid Web Server reads.
 */
static void save_metadata(void) {
    if (strlen(active_log_filename) == 0) return;
    char meta_path[128];
    int base_len = strlen(active_log_filename) - 4; // Strip the ".txt"
    snprintf(meta_path, sizeof(meta_path), "%s/%.*s_meta.txt", META_DIR, base_len, active_log_filename);
    FILE *fm = fopen(meta_path, "w");
    if (fm) {
        fprintf(fm, "%ld\n%s\n%s\n", current_meta.lines, current_meta.first_record, current_meta.last_record);
        fclose(fm);
    }
}

/** 
 * @brief Restores metadata into RAM when a file is reopened (appending).
 */
static void load_metadata(void) {
    current_meta.lines = 0; 
    current_meta.first_valid_found = false;
    strcpy(current_meta.first_record, "N/A"); 
    strcpy(current_meta.last_record, "N/A");
    
    if (strlen(active_log_filename) == 0) return;
    char meta_path[128];
    int base_len = strlen(active_log_filename) - 4; // Strip the ".txt"
    snprintf(meta_path, sizeof(meta_path), "%s/%.*s_meta.txt", META_DIR, base_len, active_log_filename);
    FILE *fm = fopen(meta_path, "r");
    if (fm) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fm)) current_meta.lines = atol(buf);
        if (fgets(buf, sizeof(buf), fm)) { 
            buf[strcspn(buf, "\r\n")] = '\0'; 
            strncpy(current_meta.first_record, buf, 63); 
            if (strstr(current_meta.first_record, "Lat: 0.00000") == NULL && strlen(current_meta.first_record) > 10) {
                current_meta.first_valid_found = true;
            }
        }
        if (fgets(buf, sizeof(buf), fm)) { 
            buf[strcspn(buf, "\r\n")] = '\0'; 
            strncpy(current_meta.last_record, buf, 63); 
        }
        fclose(fm);
    }
}

/** 
 * @brief Creates a chronological filename using system RTC (synced from GPS background).
 * Includes collision protection (appending _1, _2) to prevent accidental data overwrites.
 */
static void get_timestamp_filename(char *filename_out, size_t max_len) {
    time_t now; 
    time(&now);
    struct tm timeinfo; 
    gmtime_r(&now, &timeinfo);
    
    int year = (timeinfo.tm_year >= 70) ? (timeinfo.tm_year % 100) : 70;
    char base_name[64];
    snprintf(base_name, sizeof(base_name), "GPSDATA_%02d%02d%02d%02d%02d%02d",
             year, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    char candidate[128]; 
    snprintf(candidate, sizeof(candidate), "%s.txt", base_name);
    
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", DATA_DIR, candidate);
    
    struct stat st;
    if (stat(full_path, &st) != 0) {
        strncpy(filename_out, candidate, max_len); 
    } else {
        int suffix = 1;
        while (1) {
            snprintf(candidate, sizeof(candidate), "%s_%d.txt", base_name, suffix);
            snprintf(full_path, sizeof(full_path), "%s/%s", DATA_DIR, candidate);
            if (stat(full_path, &st) != 0) break;
            suffix++;
        }
        strncpy(filename_out, candidate, max_len);
    }
}

/**
 * @brief Mounts the physical FATFS SD card over SPI.
 */
esp_err_t init_sd_card(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // Critical: Never format user's data autonomously
        .max_files = 5, 
        .allocation_unit_size = 16 * 1024
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
    
    if (spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) return ESP_FAIL;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS; 
    slot_config.host_id = host.slot;

    if (esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card. Check wiring and FAT32 format.");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SD Card mounted successfully.");
    mkdir(DATA_DIR, 0777); 
    mkdir(META_DIR, 0777); 
    return ESP_OK;
}

/**
 * @brief Deletes `.META` cache files if the user manually deleted the raw `.txt` file via PC.
 */
void cleanup_orphaned_metadata(void) {
    DIR *dir = opendir(META_DIR);
    if (!dir) return;
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *ext_ptr = strstr(ent->d_name, "_meta.txt");
        if (ext_ptr != NULL) {
            char data_filename[64];
            size_t base_len = ext_ptr - ent->d_name;
            // Reconstruct the raw data filename from the metadata filename
            snprintf(data_filename, sizeof(data_filename), "%.*s.txt", (int)base_len, ent->d_name);
            
            char data_path[300]; 
            snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, data_filename);
            struct stat st;
            
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
 * @brief Main RTOS logger loop (Switch debounce, file I/O, flash protection)
 */
void gps_log_task(void *arg) {
    gps_msg_t msg; 
    FILE *f = NULL;
    
    TickType_t last_sync_time = xTaskGetTickCount();
    TickType_t last_meta_sync_time = xTaskGetTickCount(); 
    TickType_t pause_start_tick = 0, resume_start_tick = 0;   
    TickType_t actual_pause_time = 0, last_reminder_tick = 0;  
    
    bool logical_logging_state = false; 
    bool last_physical_state = (gpio_get_level(LOG_SWITCH_PIN) == 0);
    bool threshold_warned = false; 

    // Note: get_timestamp_filename is intentionally omitted at boot.
    // By waiting until the switch actually turns on to query the time, 
    // the background GPS parser has time to lock onto satellites and sync the OS RTC.

    while (1) {
        BaseType_t received = xQueueReceive(gps_queue, &msg, pdMS_TO_TICKS(1000));
        TickType_t current_time = xTaskGetTickCount();
        bool physical_switch_is_logging = (gpio_get_level(LOG_SWITCH_PIN) == 0);

        // --- DASHBOARD OVERRIDE: FORCE NEW FILE ---
        if (force_new_file_flag) {
            force_new_file_flag = false; // Reset trigger
            if (f != NULL) {
                fflush(f);
                fsync(fileno(f));
                fclose(f);
                f = NULL;
                save_metadata();
            }
            get_timestamp_filename(active_log_filename, sizeof(active_log_filename));
            
            if (logical_logging_state && sd_card != NULL) {
                char data_path[128];
                snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
                f = fopen(data_path, "a");
                if (f != NULL) {
                    setvbuf(f, NULL, _IOFBF, 4096); // Re-establish 4KB buffer on new file
                    load_metadata();
                    printf("\n  [NEW DEPLOYMENT FILE INITIATED : %s]\n\n", active_log_filename);
                    fflush(stdout); 
                }
            }
        }

        // --- EDGE DETECTION TERMINAL OUTPUT ---
        if (physical_switch_is_logging && !last_physical_state) {
            printf("\n>>> LOG SWITCH ACTIVE. Waiting 5s to validate...\n\n");
        } else if (!physical_switch_is_logging && last_physical_state) {
            printf("\n>>> LOG SWITCH PAUSED. Waiting 5s to validate...\n\n");
            threshold_warned = false; 
        }
        last_physical_state = physical_switch_is_logging;

        // --- STATE MACHINE: SWITCH IS OFF (PAUSED) ---
        if (!physical_switch_is_logging) {
            resume_start_tick = 0; 
            if (logical_logging_state) {
                if (pause_start_tick == 0) pause_start_tick = current_time;
                
                // If switch remains physically OFF for 5000ms continuously: Halt Logging
                if ((current_time - pause_start_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_logging_state = false;
                    actual_pause_time = current_time; 
                    last_reminder_tick = current_time; 
                    
                    if (f != NULL) {
                        fflush(f); // Push C-lib RAM cache to driver
                        fsync(fileno(f)); // Commit driver sector to physical flash memory
                        fclose(f); 
                        f = NULL;
                        save_metadata(); 
                    }
                }
            } else {
                // Tracking total paused duration to trigger warnings
                uint32_t elapsed_pause_sec = (current_time - actual_pause_time) * portTICK_PERIOD_MS / 1000;
                
                if (actual_pause_time > 0 && elapsed_pause_sec >= 60 && !threshold_warned) {
                    char next_file[64]; 
                    get_timestamp_filename(next_file, sizeof(next_file));
                    printf("\n*** PAUSE THRESHOLD EXCEEDED (%lu seconds). Resuming logging will create NEW FILE: %s ***\n\n", 
                           (unsigned long)elapsed_pause_sec, next_file);
                    threshold_warned = true;
                }
                
                if ((current_time - last_reminder_tick) >= pdMS_TO_TICKS(30000)) {
                    printf("--- REMINDER: Logging PAUSED for %lu seconds. Flip switch to resume. ---\n", 
                           (unsigned long)elapsed_pause_sec);
                    last_reminder_tick = current_time;
                }
            }
            
        // --- STATE MACHINE: SWITCH IS ON (LOGGING) ---
        } else {
            pause_start_tick = 0; 
            if (!logical_logging_state) {
                if (resume_start_tick == 0) resume_start_tick = current_time;
                
                // If switch remains physically ON for 5000ms continuously: Start Logging
                if ((current_time - resume_start_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_logging_state = true;
                    
                    // File Rotation: Generate new filename if paused > 60s
                    if (actual_pause_time > 0 && (current_time - actual_pause_time) > pdMS_TO_TICKS(60000)) {
                        get_timestamp_filename(active_log_filename, sizeof(active_log_filename));
                    
                    // Boot Condition: Generate first filename of session
                    } else if (strlen(active_log_filename) == 0) {
                        get_timestamp_filename(active_log_filename, sizeof(active_log_filename));
                    }

                    // Open file pointer in Append Mode
                    if (sd_card != NULL) {
                        char data_path[128]; 
                        snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
                        f = fopen(data_path, "a");
                        if (f != NULL) { 
                            setvbuf(f, NULL, _IOFBF, 4096); // Set 4KB block size to match FAT32 clusters
                            load_metadata(); 
                            last_sync_time = current_time; 
                            last_meta_sync_time = current_time;
                            
                            printf("\n  [LOGGING INITIATED : %s]\n\n", active_log_filename);
                            fflush(stdout); 
                        }
                    }
                }
            }
        }

        // --- QUEUE CONSUMER & FILE WRITE LOGIC ---
        if (received == pdTRUE) {
            
            // Print payload to console, appending the inline state tag at the end of the second line
            if (logical_logging_state) {
                printf("%s  [LOGGING]\n", msg.payload);
            } else {
                printf("%s  [PAUSED]\n", msg.payload);
            }
            fflush(stdout);

            // Export to dashboard via Mutex thread lock
            xSemaphoreTake(live_data_mutex, portMAX_DELAY);
            memcpy(&live_gps_data, &msg, sizeof(gps_msg_t));
            live_is_logging = logical_logging_state;
            strncpy(live_active_file, active_log_filename, sizeof(live_active_file));
            xSemaphoreGive(live_data_mutex);

            if (logical_logging_state && f != NULL) {
                // Write pure payload strictly to the SD card (no UI tags)
                fprintf(f, "%s\n", msg.payload);
                
                // Track metadata bounds
                current_meta.lines++;
                extract_metadata_record(&msg, current_meta.last_record); 
                
                // Save first line (usually zeroes)
                if (current_meta.lines == 1) {
                    extract_metadata_record(&msg, current_meta.first_record);
                }

                // If first line was zeroes, lock metadata immediately upon finding a valid fix
                if (!current_meta.first_valid_found && msg.fix_type > 1) {
                    extract_metadata_record(&msg, current_meta.first_record);
                    current_meta.first_valid_found = true;
                    save_metadata(); 
                }

                // --- 30-SECOND HARDWARE FSYNC ALGORITHM ---
                if ((current_time - last_sync_time) > pdMS_TO_TICKS(30000)) {
                    fflush(f); 
                    fsync(fileno(f)); 
                    last_sync_time = current_time;

                    char total_str[32] = "N/A";
                    char used_str[32]  = "N/A";
                    char free_str[32]  = "N/A";

                    get_sd_capacity_strings(total_str, used_str, free_str, sizeof(total_str));

                    printf("\n====================================================\n");
                    printf("[LOGGING BUFFER SYNCED : %s]\n", active_log_filename);
                    printf("Total: %s | Used: %s | Free: %s\n", total_str, used_str, free_str);
                    printf("====================================================\n\n");
                    fflush(stdout); 
                }

                // --- 30-SECOND METADATA CACHE ALGORITHM ---
                if ((current_time - last_meta_sync_time) > pdMS_TO_TICKS(30000)) {
                    save_metadata();
                    last_meta_sync_time = current_time;
                }
            }
        }
    }
}

/** 
 * @brief Debug Verification Task: Validates data is written successfully to physical media.
 */
void gps_tail_task(void *arg) {
    char last_lines[10][MAX_LINE_LEN]; 
    char temp_buf[MAX_LINE_LEN];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); 
        if (sd_card == NULL || strlen(active_log_filename) == 0) continue;

        char data_path[128]; 
        snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
        FILE *f = fopen(data_path, "r");
        if (f == NULL) continue;

        // Jump near end of file to prevent crashing heap on large files
        fseek(f, 0, SEEK_END); 
        long size = ftell(f);
        long offset = size > 2500 ? size - 2500 : 0;
        fseek(f, offset, SEEK_SET);

        if (offset > 0) fgets(temp_buf, sizeof(temp_buf), f);
        
        int idx = 0, count = 0;
        while (fgets(temp_buf, sizeof(temp_buf), f) != NULL) {
            strncpy(last_lines[idx], temp_buf, MAX_LINE_LEN - 1);
            last_lines[idx][MAX_LINE_LEN - 1] = '\0';
            idx = (idx + 1) % 10; 
            count++;
        }
        fclose(f);

        // Print final buffer to console
        printf("\n--- VERIFICATION: LAST %d LINES OF %s ---\n", count > 10 ? 10 : count, active_log_filename);
        int start_idx = count > 10 ? idx : 0;
        int lines_to_print = count > 10 ? 10 : count;
        for (int i = 0; i < lines_to_print; i++) {
            printf("%s", last_lines[(start_idx + i) % 10]);
        }
        printf("--------------------------------------------------\n\n");
    }
}