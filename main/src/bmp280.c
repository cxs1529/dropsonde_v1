/**
 * ==============================================================================
 * @file    bmp280.c
 * @brief   Implementation of the BME280/BMP280 sensor driver.
 *
 * @details This file implements the I2C communication and complex 64-bit 
 *          integer compensation algorithms provided by Bosch Sensortec for 
 *          the BMP280 and BME280 environmental sensors. 
 *          
 *          It handles:
 *          - Reading the unique factory calibration matrix on boot.
 *          - Writing configuration registers (oversampling, IIR filters).
 *          - Burst reading raw ADC values for Temperature, Pressure, and Humidity.
 *          - Applying the official Bosch math to yield human-readable float values 
 *            (Celsius, Pascals, and Relative Humidity %).
 *          
 *          This implementation is designed for the ESP-IDF v6+ framework, utilizing
 *          the modern I2C master API.
 * ==============================================================================
 */

#include "bmp280.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BMP280";

static esp_err_t read_register8(bmp280_t *dev, uint8_t reg, uint8_t *data) {
    return i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, data, 1, 100);
}

static esp_err_t read_register16(bmp280_t *dev, uint8_t reg, uint16_t *data) {
    uint8_t d[2];
    esp_err_t ret = i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, d, 2, 100);
    *data = (d[1] << 8) | d[0]; // LSB first
    return ret;
}

static esp_err_t write_register8(bmp280_t *dev, uint8_t reg, uint8_t data) {
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(dev->i2c_dev, buf, 2, 100);
}

void bmp280_init_default_params(bmp280_params_t *params) {
    params->mode = 3;       
    params->osrs_t = 1;     
    params->osrs_p = 1;     
    params->osrs_h = 1;     
    params->filter = 0;     
    params->standby = 0;    
}

esp_err_t bmp280_init_desc(bmp280_t *dev, i2c_master_dev_handle_t i2c_dev) {
    dev->i2c_dev = i2c_dev;
    return ESP_OK;
}

esp_err_t bmp280_init(bmp280_t *dev, bmp280_params_t *params) {
    ESP_ERROR_CHECK(read_register8(dev, 0xD0, &dev->id));
    if (dev->id != BMP280_CHIP_ID && dev->id != BME280_CHIP_ID) {
        ESP_LOGE(TAG, "Unsupported Chip ID: 0x%02x", dev->id);
        return ESP_FAIL;
    }

    read_register16(dev, 0x88, &dev->calib.dig_T1); read_register16(dev, 0x8A, (uint16_t*)&dev->calib.dig_T2);
    read_register16(dev, 0x8C, (uint16_t*)&dev->calib.dig_T3); read_register16(dev, 0x8E, &dev->calib.dig_P1);
    read_register16(dev, 0x90, (uint16_t*)&dev->calib.dig_P2); read_register16(dev, 0x92, (uint16_t*)&dev->calib.dig_P3);
    read_register16(dev, 0x94, (uint16_t*)&dev->calib.dig_P4); read_register16(dev, 0x96, (uint16_t*)&dev->calib.dig_P5);
    read_register16(dev, 0x98, (uint16_t*)&dev->calib.dig_P6); read_register16(dev, 0x9A, (uint16_t*)&dev->calib.dig_P7);
    read_register16(dev, 0x9C, (uint16_t*)&dev->calib.dig_P8); read_register16(dev, 0x9E, (uint16_t*)&dev->calib.dig_P9);

    if (dev->id == BME280_CHIP_ID) {
        read_register8(dev, 0xA1, &dev->calib.dig_H1); read_register16(dev, 0xE1, (uint16_t*)&dev->calib.dig_H2);
        read_register8(dev, 0xE3, &dev->calib.dig_H3);
        
        uint8_t e4, e5, e6, e7;
        read_register8(dev, 0xE4, &e4); read_register8(dev, 0xE5, &e5);
        read_register8(dev, 0xE6, &e6); read_register8(dev, 0xE7, &e7);
        
        dev->calib.dig_H4 = (e4 << 4) | (e5 & 0x0F);
        dev->calib.dig_H5 = (e6 << 4) | (e5 >> 4);
        dev->calib.dig_H6 = e7;
        
        write_register8(dev, 0xF2, params->osrs_h); 
    }

    write_register8(dev, 0xF5, (params->standby << 5) | (params->filter << 2)); 
    write_register8(dev, 0xF4, (params->osrs_t << 5) | (params->osrs_p << 2) | params->mode); 

    return ESP_OK;
}

esp_err_t bmp280_read_float(bmp280_t *dev, float *temperature, float *pressure, float *humidity) {
    uint8_t reg = 0xF7;
    uint8_t data[8];
    size_t length = (dev->id == BME280_CHIP_ID) ? 8 : 6;
    
    if (i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, data, length, 100) != ESP_OK) {
        return ESP_FAIL;
    }

    int32_t adc_P = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);

    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)dev->calib.dig_T1 << 1))) * ((int32_t)dev->calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dev->calib.dig_T1)) * ((adc_T >> 4) - ((int32_t)dev->calib.dig_T1))) >> 12) * ((int32_t)dev->calib.dig_T3)) >> 14;
    dev->t_fine = var1 + var2;
    *temperature = (float)((dev->t_fine * 5 + 128) >> 8) / 100.0f;

    int64_t p_var1, p_var2, p;
    p_var1 = ((int64_t)dev->t_fine) - 128000;
    p_var2 = p_var1 * p_var1 * (int64_t)dev->calib.dig_P6;
    p_var2 = p_var2 + ((p_var1 * (int64_t)dev->calib.dig_P5) << 17);
    p_var2 = p_var2 + (((int64_t)dev->calib.dig_P4) << 35);
    p_var1 = ((p_var1 * p_var1 * (int64_t)dev->calib.dig_P3) >> 8) + ((p_var1 * (int64_t)dev->calib.dig_P2) << 12);
    p_var1 = (((((int64_t)1) << 47) + p_var1)) * ((int64_t)dev->calib.dig_P1) >> 33;
    
    if (p_var1 == 0) {
        *pressure = 0.0f; 
    } else {
        p = 1048576 - adc_P;
        p = (((p << 31) - p_var2) * 3125) / p_var1;
        p_var1 = (((int64_t)dev->calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
        p_var2 = (((int64_t)dev->calib.dig_P8) * p) >> 19;
        p = ((p + p_var1 + p_var2) >> 8) + (((int64_t)dev->calib.dig_P7) << 4);
        *pressure = (float)p / 256.0f; 
    }

    if (dev->id == BME280_CHIP_ID) {
        int32_t adc_H = (data[6] << 8) | data[7];
        int32_t v_x1_u32r;
        
        v_x1_u32r = (dev->t_fine - ((int32_t)76800));
        v_x1_u32r = (((((adc_H << 14) - (((int32_t)dev->calib.dig_H4) << 20) - (((int32_t)dev->calib.dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15) * (((((((v_x1_u32r * ((int32_t)dev->calib.dig_H6)) >> 10) * (((v_x1_u32r * ((int32_t)dev->calib.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) * ((int32_t)dev->calib.dig_H2) + 8192) >> 14));
        v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)dev->calib.dig_H1)) >> 4));
        
        v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
        v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
        *humidity = (float)(v_x1_u32r >> 12) / 1024.0f;
    } else {
        *humidity = 0.0f;
    }

    return ESP_OK;
}