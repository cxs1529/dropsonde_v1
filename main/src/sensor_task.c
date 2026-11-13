/**
 * ==============================================================================
 * @file    sensor_task.c
 * @brief   I2C Environmental Sensor Task (BME280 / BMP280).
 * 
 * @details
 * Operates as an independent FreeRTOS task.
 * 
 * Uses the modern ESP-IDF v6+ driver/i2c_master.h API to communicate with 
 * Bosch environmental sensors. It polls the sensor at 1Hz, applies the 64-bit 
 * Bosch compensation matrix, and securely writes the floating-point values to 
 * the globally accessible `current_sensor_data` struct via a Mutex lock.
 * 
 * - Initializes the ESP32 hardware I2C master controller on the configured port.
 * - Wakes and configures the Bosch BMP280/BME280 sensor (detects chip automatically).
 * - Continuously reads Temperature, Pressure, and Humidity at 1Hz.
 * - Converts Pascals to standard meteorological hectoPascals (hPa).
 * - Prints output to the terminal for monitoring.
 * ==============================================================================
 */

#include "sensor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bmp280.h"        
#include "config.h"
#include "ema_filter.h"

static const char *TAG = "SENSOR_TASK";

// Define the initial global alpha values
float ema_alpha_temp = 0.15f;
float ema_alpha_rh = 0.15f;
float ema_alpha_press = 0.1f;
float ema_alpha_volt = 0.2f;


void bme280_task(void *pvParameters) {
    // 1. Configure the ESP32 I2C Master Bus
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1, 
        .scl_io_num = I2C_SCL_PIN,
        .sda_io_num = I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle));

    // 2. Add the Target Sensor to the I2C Bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP280_I2C_ADDRESS_0,
        .scl_speed_hz = 100000, 
    };
    
    i2c_master_dev_handle_t dev_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

    // 3. Initialize Sensor Registers
    bmp280_params_t params;
    bmp280_init_default_params(&params);
    
    bmp280_t dev = {0};
    ESP_ERROR_CHECK(bmp280_init_desc(&dev, dev_handle));

    if (bmp280_init(&dev, &params) != ESP_OK) {
        ESP_LOGE(TAG, "BME280/BMP280 initialization failed! Check wiring.");
        vTaskDelete(NULL); 
    }

    bool is_bme280 = dev.id == BME280_CHIP_ID;
    ESP_LOGI(TAG, "I2C Sensor found: %s", is_bme280 ? "BME280" : "BMP280");

    // 4. Initialize Professional EMA Filters
    // Alpha 0.2f provides responsive yet smooth jitter rejection for 1Hz sampling
    ema_filter_t filt_T, filt_RH, filt_P, filt_V;
    ema_filter_init(&filt_T, ema_alpha_temp);
    ema_filter_init(&filt_RH, ema_alpha_rh);
    ema_filter_init(&filt_P, ema_alpha_press);
    ema_filter_init(&filt_V, ema_alpha_volt); // Reserved for future ADC input

    float pressure, temperature, humidity;

    // 5. Infinite Polling Loop (1Hz)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        
        if (bmp280_read_float(&dev, &temperature, &pressure, &humidity) == ESP_OK) {
            float pressure_mbar = pressure / 100.0f; // Pa to mbar

            // Sync filters with global variables to allow live-tuning
            filt_T.alpha = ema_alpha_temp;
            filt_RH.alpha = ema_alpha_rh;
            filt_P.alpha = ema_alpha_press;
            filt_V.alpha = ema_alpha_volt;
            
            // Apply EMA Filtering
            float clean_t = ema_filter_update(&filt_T, temperature);
            float clean_rh = ema_filter_update(&filt_RH, humidity);
            float clean_p = ema_filter_update(&filt_P, pressure_mbar);
            float clean_v = ema_filter_update(&filt_V, 0.0f); // Placeholder for ADC Vin
            
            // Expose to other threads securely
            if (sensor_data_mutex) {
                xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
                current_sensor_data.temperature = clean_t;
                current_sensor_data.humidity = clean_rh;
                current_sensor_data.pressure = clean_p;
                current_sensor_data.voltage = clean_v; 
                xSemaphoreGive(sensor_data_mutex);
            }
            
            if (is_bme280) {
                printf("[BME280] Temp: %.2f *C | RH: %.2f %% | Press: %.2f mbar\n", 
                       clean_t, clean_rh, clean_p);
            } else {
                printf("[BMP280] Temp: %.2f *C | Press: %.2f mbar\n", 
                       clean_t, clean_p);
            }
        } else {
            ESP_LOGE(TAG, "Failed to read BME280/BMP280 sensor data");
        }
    }
}
