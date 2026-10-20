/**
 * ==============================================================================
 * @file    sd_logger.h
 * @brief   Header interface for SD card FATFS filesystem operations.
 * 
 * FUNCTIONS SUMMARY:
 * - init_sd_card(): Initializes SPI DMA and mounts FATFS virtual file system.
 * - cleanup_orphaned_metadata(): Deletes metadata for manually removed files.
 * - gps_log_task(): RTOS Task handling file I/O and switch debouncing.
 * - gps_tail_task(): RTOS Task that prints recent lines for console debugging.
 * ==============================================================================
 */

#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include "esp_err.h"

/**
 * @brief Mounts FATFS filesystem over SPI DMA. Creates /DATA and /.META folders.
 * @return ESP_OK on success, ESP_FAIL on card mount error.
 */
esp_err_t init_sd_card(void);

/**
 * @brief Scans /.META on boot and removes any cache files whose data file is missing.
 */
void cleanup_orphaned_metadata(void);

/**
 * @brief Primary consumer task handling file writes and 5s debounced state machine.
 */
void gps_log_task(void *arg);

/**
 * @brief Debug verification task reading the last 10 lines of active file every 60s.
 */
void gps_tail_task(void *arg);

#endif // SD_LOGGER_H