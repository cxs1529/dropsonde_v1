/**
 * ==============================================================================
 * @file    bmp280.h
 * @brief   Standalone ESP-IDF I2C driver for BME280 / BMP280 sensors.
 *          https://github.com/esp-idf-lib/bmp280
 *
 * @details Updated for ESP-IDF v6.0+ using the modern driver/i2c_master.h API.
 * ==============================================================================
 */
#ifndef __BMP280_H__
#define __BMP280_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define BMP280_I2C_ADDRESS_0  0x76  // SDO tied to GND --> using this option
#define BMP280_I2C_ADDRESS_1  0x77  // SDO tied to VDD

#define BME280_CHIP_ID        0x60  
#define BMP280_CHIP_ID        0x58  

typedef struct {
    uint16_t dig_T1; int16_t  dig_T2; int16_t  dig_T3;
    uint16_t dig_P1; int16_t  dig_P2; int16_t  dig_P3;
    int16_t  dig_P4; int16_t  dig_P5; int16_t  dig_P6;
    int16_t  dig_P7; int16_t  dig_P8; int16_t  dig_P9;
    uint8_t  dig_H1; int16_t  dig_H2; uint8_t  dig_H3;
    int16_t  dig_H4; int16_t  dig_H5; int8_t   dig_H6;
} bmp280_calib_data_t;

typedef struct {
    i2c_master_dev_handle_t i2c_dev; // Modern ESP-IDF v6 Device Handle
    uint8_t id;              
    bmp280_calib_data_t calib; 
    int32_t t_fine;          
} bmp280_t;

typedef struct {
    uint8_t mode;            
    uint8_t osrs_t;          
    uint8_t osrs_p;          
    uint8_t osrs_h;          
    uint8_t filter;          
    uint8_t standby;         
} bmp280_params_t;

void bmp280_init_default_params(bmp280_params_t *params);

/**
 * @brief Sets up the device descriptor using the modern I2C master device handle.
 * @param dev Pointer to the device descriptor.
 * @param i2c_dev Modern ESP-IDF v6 I2C device handle.
 */
esp_err_t bmp280_init_desc(bmp280_t *dev, i2c_master_dev_handle_t i2c_dev);

esp_err_t bmp280_init(bmp280_t *dev, bmp280_params_t *params);
esp_err_t bmp280_read_float(bmp280_t *dev, float *temperature, float *pressure, float *humidity);

#endif // __BMP280_H__
