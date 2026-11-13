/**
 * ==============================================================================
 * @file    sd_logger.c
 * @brief   SD Card FATFS storage manager, file rotation, and logging loop.
 * 
 * @details
 * This module operates as the CONSUMER task for the GPS Queue.
 * - Implements a 5-second software debounce for the physical logging switch.
 * - Handles automatic chronological filename generation (locking to GPS RTC).
 * - Implements synchronous double-buffering: Writes GPS data to `GPSDATA_*.dat` 
 *   and BME data to `BMEDATA_*.dat` simultaneously.
 * - Executes periodic `fsync()` operations every 30 seconds to flush the C-library 
 *   RAM buffers directly into the non-volatile flash memory to prevent data loss.
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
char force_new_file_note[256] = "N/A";

typedef struct {
    long lines;
    bool first_valid_found;
    char first_record[64];
    char last_record[64];
    
    // Sensor Statistics Tracking
    float t_min, t_sum, t_max;
    float rh_min, rh_sum, rh_max;
    float p_min, p_sum, p_max;
    float v_min, v_sum, v_max;
    long sensor_count;
} log_meta_t;

static log_meta_t current_meta = {0};

static void reset_meta_stats(void) {
    current_meta.lines = 0; 
    current_meta.first_valid_found = false;
    strcpy(current_meta.first_record, "N/A"); 
    strcpy(current_meta.last_record, "N/A");
    
    current_meta.t_min = 9999.0f; current_meta.t_max = -9999.0f; current_meta.t_sum = 0.0f;
    current_meta.rh_min = 9999.0f; current_meta.rh_max = -9999.0f; current_meta.rh_sum = 0.0f;
    current_meta.p_min = 9999.0f; current_meta.p_max = -9999.0f; current_meta.p_sum = 0.0f;
    current_meta.v_min = 9999.0f; current_meta.v_max = -9999.0f; current_meta.v_sum = 0.0f;
    current_meta.sensor_count = 0;
}

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

static bool get_sd_capacity_strings(char *total_str, char *used_str, char *free_str, size_t str_len) {
    FATFS *fs;
    DWORD fre_clust;
    FRESULT res = f_getfree("0:", &fre_clust, &fs);
    if (res != FR_OK || fs == NULL) res = f_getfree("", &fre_clust, &fs);
    if (res != FR_OK || fs == NULL) {
        snprintf(total_str, str_len, "ERR (%d)", res);
        snprintf(used_str,  str_len, "ERR (%d)", res);
        snprintf(free_str,  str_len, "ERR (%d)", res);
        return false;
    }
    uint64_t tot_sect = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t fre_sect = (uint64_t)fre_clust * fs->csize;
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

static void extract_metadata_record(gps_msg_t *msg, char* dest) {
    snprintf(dest, 64, "%02d/%02d/%02d %02d:%02d:%02d Lat: %.5f Lon: %.5f", 
             msg->month, msg->day, msg->year, msg->hour, msg->min, msg->sec, msg->lat, msg->lon);
}

static void save_metadata(void) {
    if (strlen(active_log_filename) == 0) return;
    char meta_path[128];
    int base_len = strlen(active_log_filename) - 4;
    snprintf(meta_path, sizeof(meta_path), "%s/%.*s_meta.txt", META_DIR, base_len, active_log_filename);
    FILE *fm = fopen(meta_path, "w");
    if (fm) {
        fprintf(fm, "%ld\n%s\n%s\n", current_meta.lines, current_meta.first_record, current_meta.last_record);
        fprintf(fm, "%.2f,%.2f,%.2f,%ld\n", current_meta.t_min, current_meta.t_sum, current_meta.t_max, current_meta.sensor_count);
        fprintf(fm, "%.2f,%.2f,%.2f\n", current_meta.rh_min, current_meta.rh_sum, current_meta.rh_max);
        fprintf(fm, "%.2f,%.2f,%.2f\n", current_meta.p_min, current_meta.p_sum, current_meta.p_max);
        fprintf(fm, "%.2f,%.2f,%.2f\n", current_meta.v_min, current_meta.v_sum, current_meta.v_max);
        fclose(fm);
    }
}

static void load_metadata(void) {
    reset_meta_stats();
    
    if (strlen(active_log_filename) == 0) return;
    char meta_path[128];
    int base_len = strlen(active_log_filename) - 4;
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
        if (fgets(buf, sizeof(buf), fm)) sscanf(buf, "%f,%f,%f,%ld", &current_meta.t_min, &current_meta.t_sum, &current_meta.t_max, &current_meta.sensor_count);
        if (fgets(buf, sizeof(buf), fm)) sscanf(buf, "%f,%f,%f", &current_meta.rh_min, &current_meta.rh_sum, &current_meta.rh_max);
        if (fgets(buf, sizeof(buf), fm)) sscanf(buf, "%f,%f,%f", &current_meta.p_min, &current_meta.p_sum, &current_meta.p_max);
        if (fgets(buf, sizeof(buf), fm)) sscanf(buf, "%f,%f,%f", &current_meta.v_min, &current_meta.v_sum, &current_meta.v_max);
        
        fclose(fm);
    }
}

static void get_timestamp_filename(char *filename_out, size_t max_len) {
    time_t now; time(&now);
    struct tm timeinfo; gmtime_r(&now, &timeinfo);
    
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

esp_err_t init_sd_card(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, 
        .max_files = 5, 
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI, .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    if (spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) return ESP_FAIL;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS; slot_config.host_id = host.slot;
    if (esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card. Check wiring and FAT32 format.");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SD Card mounted successfully.");
    mkdir(DATA_DIR, 0777); 
    mkdir(META_DIR, 0777); 
    mkdir(MOUNT_POINT "/.WEB", 0777); 
    
    return ESP_OK;
}

void cleanup_orphaned_metadata(void) {
    DIR *dir = opendir(META_DIR);
    if (!dir) return;
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *ext_ptr = strstr(ent->d_name, "_meta.txt");
        if (ext_ptr != NULL) {
            char data_filename[64];
            size_t base_len = ext_ptr - ent->d_name;
            snprintf(data_filename, sizeof(data_filename), "%.*s.txt", (int)base_len, ent->d_name);
            
            char data_path[300]; snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, data_filename);
            struct stat st;
            if (stat(data_path, &st) != 0) { 
                char meta_path[300]; snprintf(meta_path, sizeof(meta_path), "%s/%s", META_DIR, ent->d_name);
                unlink(meta_path); 
            }
        }
    }
    closedir(dir);
}

void gps_log_task(void *arg) {
    gps_msg_t msg; 
    FILE *f = NULL;      
    FILE *f_bme = NULL;  
    char active_bme_filename[64] = "";
    
    TickType_t last_sync_time = xTaskGetTickCount();
    TickType_t last_meta_sync_time = xTaskGetTickCount(); 
    TickType_t pause_start_tick = 0, resume_start_tick = 0;  
    TickType_t actual_pause_time = 0, last_reminder_tick = 0;      
    
    bool logical_logging_state = false; 
    bool last_physical_state = (gpio_get_level(LOG_SWITCH_PIN) == 0);
    bool threshold_warned = false;

    while (1) {
        BaseType_t received = xQueueReceive(gps_queue, &msg, pdMS_TO_TICKS(1000));
        TickType_t current_time = xTaskGetTickCount();
        bool physical_switch_is_logging = (gpio_get_level(LOG_SWITCH_PIN) == 0);

        if (force_new_file_flag) {
            if (f != NULL) { fflush(f); fsync(fileno(f)); fclose(f); f = NULL; }
            if (f_bme != NULL) { fflush(f_bme); fsync(fileno(f_bme)); fclose(f_bme); f_bme = NULL; }
            
            get_timestamp_filename(active_log_filename, sizeof(active_log_filename));
            
            char base_timestamp[32] = {0};
            sscanf(active_log_filename, "GPSDATA_%[^.]", base_timestamp);
            snprintf(active_bme_filename, sizeof(active_bme_filename), "BMEDATA_%s.txt", base_timestamp);
            
            char data_path[128]; snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
            f = fopen(data_path, "a");
            
            char bme_path[128]; snprintf(bme_path, sizeof(bme_path), "%s/%s", DATA_DIR, active_bme_filename);
            f_bme = fopen(bme_path, "a");
            if (f_bme) {
                fseek(f_bme, 0, SEEK_END);
                if (ftell(f_bme) == 0) {
                    fprintf(f_bme, "Timestamp,Temperature(C),Humidity(%%),Pressure(mbar),Voltage(V)\n");
                }
            }

            char meta_filename[128]; snprintf(meta_filename, sizeof(meta_filename), "%s/GPSDATA_%s_meta.txt", META_DIR, base_timestamp);
            FILE *meta_f = fopen(meta_filename, "w");
            if (meta_f) { fprintf(meta_f, "0\nN/A\nN/A\n"); fclose(meta_f); }
            
            char note_filename[128]; snprintf(note_filename, sizeof(note_filename), "%s/GPSDATA_%s_note.txt", META_DIR, base_timestamp);
            FILE *note_f = fopen(note_filename, "w");
            if (note_f) { fprintf(note_f, "%s\n", force_new_file_note); fclose(note_f); }
            
            reset_meta_stats();
            strcpy(force_new_file_note, "N/A"); 
            
            xSemaphoreTake(live_data_mutex, portMAX_DELAY);
            strncpy(live_active_file, active_log_filename, sizeof(live_active_file));
            force_new_file_flag = false;
            xSemaphoreGive(live_data_mutex);
            
            printf("\n>>> ROTATED TO NEW DEPLOYMENT LOG: %s\n", active_log_filename);
        }

        if (physical_switch_is_logging && !last_physical_state) {
            printf("\n>>> LOG SWITCH ACTIVE. Waiting 5s to validate...\n\n");
        } else if (!physical_switch_is_logging && last_physical_state) {
            printf("\n>>> LOG SWITCH PAUSED. Waiting 5s to validate...\n\n");
            threshold_warned = false; 
        }
        last_physical_state = physical_switch_is_logging;

        if (!physical_switch_is_logging) {
            resume_start_tick = 0; 
            if (logical_logging_state) {
                if (pause_start_tick == 0) pause_start_tick = current_time;
                
                if ((current_time - pause_start_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_logging_state = false;
                    actual_pause_time = current_time; 
                    last_reminder_tick = current_time; 
                    
                    if (f != NULL) { fflush(f); fsync(fileno(f)); fclose(f); f = NULL; save_metadata(); }
                    if (f_bme != NULL) { fflush(f_bme); fsync(fileno(f_bme)); fclose(f_bme); f_bme = NULL; }
                }
            } else {
                uint32_t elapsed_pause_sec = (current_time - actual_pause_time) * portTICK_PERIOD_MS / 1000;
                
                if (actual_pause_time > 0 && elapsed_pause_sec >= 60 && !threshold_warned) {
                    char next_file[64]; get_timestamp_filename(next_file, sizeof(next_file));
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
        } else {
            pause_start_tick = 0; 
            if (!logical_logging_state) {
                if (resume_start_tick == 0) resume_start_tick = current_time;
                
                if ((current_time - resume_start_tick) >= pdMS_TO_TICKS(5000)) {
                    logical_logging_state = true;
                    
                    if (actual_pause_time > 0 && (current_time - actual_pause_time) > pdMS_TO_TICKS(60000)) {
                        get_timestamp_filename(active_log_filename, sizeof(active_log_filename));
                    } else if (strlen(active_log_filename) == 0) {
                        get_timestamp_filename(active_log_filename, sizeof(active_log_filename));
                    }
                    
                    char base_timestamp[32] = {0};
                    sscanf(active_log_filename, "GPSDATA_%[^.]", base_timestamp);
                    snprintf(active_bme_filename, sizeof(active_bme_filename), "BMEDATA_%s.txt", base_timestamp);

                    if (sd_card != NULL) {
                        char data_path[128]; snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
                        f = fopen(data_path, "a"); 
                        
                        char bme_path[128]; snprintf(bme_path, sizeof(bme_path), "%s/%s", DATA_DIR, active_bme_filename);
                        f_bme = fopen(bme_path, "a"); 
                        
                        if (f != NULL) { 
                            setvbuf(f, NULL, _IOFBF, 4096); 
                            load_metadata(); 
                            last_sync_time = current_time; 
                            last_meta_sync_time = current_time; 
                            printf("\n  [LOGGING INITIATED : %s & %s]\n\n", active_log_filename, active_bme_filename);
                            fflush(stdout); 
                        }
                        
                        if (f_bme != NULL) {
                            fseek(f_bme, 0, SEEK_END);
                            if (ftell(f_bme) == 0) {
                                fprintf(f_bme, "Timestamp,Temperature(C),Humidity(%%),Pressure(mbar),Voltage(V)\n");
                            }
                        }
                    }
                }
            }
        }

        if (received == pdTRUE) {
            if (logical_logging_state) { printf("%s  [LOGGING]\n", msg.payload); } 
            else { printf("%s  [PAUSED]\n", msg.payload); }
            fflush(stdout);
            
            xSemaphoreTake(live_data_mutex, portMAX_DELAY);
            memcpy(&live_gps_data, &msg, sizeof(gps_msg_t));
            live_is_logging = logical_logging_state;
            strncpy(live_active_file, active_log_filename, sizeof(live_active_file));
            xSemaphoreGive(live_data_mutex);
            
            if (logical_logging_state) {
                if (f != NULL) {
                    fprintf(f, "%s\n", msg.payload);
                    current_meta.lines++;
                    extract_metadata_record(&msg, current_meta.last_record);
                    if (current_meta.lines == 1) { extract_metadata_record(&msg, current_meta.first_record); }
                    if (!current_meta.first_valid_found && msg.fix_type > 1) {
                        extract_metadata_record(&msg, current_meta.first_record);
                        current_meta.first_valid_found = true;
                        save_metadata(); 
                    }
                }
                
                if (f_bme != NULL) {
                    float temp = 0, rh = 0, press = 0, volt = 0;
                    if (sensor_data_mutex) {
                        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
                        temp = current_sensor_data.temperature;
                        rh = current_sensor_data.humidity;
                        press = current_sensor_data.pressure;
                        volt = current_sensor_data.voltage;
                        xSemaphoreGive(sensor_data_mutex);
                    }
                    
                    // Update Stats
                    if (temp < current_meta.t_min) current_meta.t_min = temp;
                    if (temp > current_meta.t_max) current_meta.t_max = temp;
                    current_meta.t_sum += temp;
                    
                    if (rh < current_meta.rh_min) current_meta.rh_min = rh;
                    if (rh > current_meta.rh_max) current_meta.rh_max = rh;
                    current_meta.rh_sum += rh;
                    
                    if (press < current_meta.p_min) current_meta.p_min = press;
                    if (press > current_meta.p_max) current_meta.p_max = press;
                    current_meta.p_sum += press;
                    
                    if (volt < current_meta.v_min) current_meta.v_min = volt;
                    if (volt > current_meta.v_max) current_meta.v_max = volt;
                    current_meta.v_sum += volt;
                    
                    current_meta.sensor_count++;
                    
                    // Always log continuous internal time, ignoring GPS dropouts
                    time_t now;
                    struct tm timeinfo;
                    time(&now);
                    localtime_r(&now, &timeinfo);

                    int rtc_year  = timeinfo.tm_year % 100; 
                    int rtc_month = timeinfo.tm_mon + 1;
                    int rtc_day   = timeinfo.tm_mday;
                    int rtc_hour  = timeinfo.tm_hour;
                    int rtc_min   = timeinfo.tm_min;
                    int rtc_sec   = timeinfo.tm_sec;
                    
                    fprintf(f_bme, "%02d/%02d/%02d %02d:%02d:%02d,%.2f,%.2f,%.2f,%.2f\n", 
                        rtc_month, rtc_day, rtc_year, rtc_hour, rtc_min, rtc_sec, temp, rh, press, volt);
                }

                if ((current_time - last_sync_time) > pdMS_TO_TICKS(30000)) {
                    if (f) { fflush(f); fsync(fileno(f)); }
                    if (f_bme) { fflush(f_bme); fsync(fileno(f_bme)); }
                    last_sync_time = current_time;
                    
                    char total_str[32] = "N/A"; char used_str[32]  = "N/A"; char free_str[32]  = "N/A";
                    get_sd_capacity_strings(total_str, used_str, free_str, sizeof(total_str));
                    printf("\n====================================================\n");
                    printf("[LOGGING BUFFER SYNCED : %s]\n", active_log_filename);
                    printf("Total: %s | Used: %s | Free: %s\n", total_str, used_str, free_str);
                    printf("====================================================\n\n");
                    fflush(stdout); 
                }
                
                if ((current_time - last_meta_sync_time) > pdMS_TO_TICKS(30000)) {
                    save_metadata();
                    last_meta_sync_time = current_time;
                }
            }
        }
    }
}

void gps_tail_task(void *arg) {
    char last_lines[10][MAX_LINE_LEN]; 
    char temp_buf[MAX_LINE_LEN];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); 
        if (sd_card == NULL || strlen(active_log_filename) == 0) continue;
        char data_path[128]; snprintf(data_path, sizeof(data_path), "%s/%s", DATA_DIR, active_log_filename);
        FILE *f = fopen(data_path, "r");
        if (f == NULL) continue;
        
        fseek(f, 0, SEEK_END); 
        long size = ftell(f);
        long offset = size > 2500 ? size - 2500 : 0;
        fseek(f, offset, SEEK_SET);
        if (offset > 0) fgets(temp_buf, sizeof(temp_buf), f);
        
        int idx = 0, count = 0;
        while (fgets(temp_buf, sizeof(temp_buf), f) != NULL) {
            strncpy(last_lines[idx], temp_buf, MAX_LINE_LEN - 1);
            last_lines[idx][MAX_LINE_LEN - 1] = '\0';
            idx = (idx + 1) % 10; count++;
        }
        fclose(f);
        
        printf("\n--- VERIFICATION: LAST %d LINES OF %s ---\n", count > 10 ? 10 : count, active_log_filename);
        int start_idx = count > 10 ? idx : 0;
        int lines_to_print = count > 10 ? 10 : count;
        for (int i = 0; i < lines_to_print; i++) {
            printf("%s", last_lines[(start_idx + i) % 10]);
        }
        printf("--------------------------------------------------\n\n");
    }
}
