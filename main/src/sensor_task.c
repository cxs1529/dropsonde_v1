/**
 * ==============================================================================
 * @file    sensor_task.c
 * @brief   I2C Environmental Sensor Task (BME280 / BMP280).
 * 
 * @details
 * Operates as an independent FreeRTOS task.
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
#include "driver/i2c_master.h" // Modern ESP-IDF v6 I2C Master API
#include "bmp280.h"        
#include "config.h"        

static const char *TAG = "SENSOR_TASK";

void bme280_task(void *pvParameters) {
    
    // 1. Initialize ESP32 I2C Master Bus
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1, // Dynamically allocate a free port
        .scl_io_num = I2C_SCL_PIN,
        .sda_io_num = I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle));

    // 2. Add the BMP280/BME280 Device to the Bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP280_I2C_ADDRESS_0, // 0x76 (SDO tied to GND)
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t dev_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

    // 3. Initialize Sensor Logic
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

    float pressure, temperature, humidity;

    // 4. Continuous Read Loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        
        if (bmp280_read_float(&dev, &temperature, &pressure, &humidity) == ESP_OK) {
            float pressure_hpa = pressure / 100.0;
            
            if (is_bme280) {
                printf("[BME280] Temp: %.2f *C | RH: %.2f %% | Press: %.2f hPa\n", 
                       temperature, humidity, pressure_hpa);
            } else {
                printf("[BMP280] Temp: %.2f *C | Press: %.2f hPa\n", 
                       temperature, pressure_hpa);
            }
        } else {
            ESP_LOGE(TAG, "Failed to read BME280/BMP280 sensor data");
        }
    }
}
