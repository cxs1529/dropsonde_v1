/**
 * ==============================================================================
 * @file    sensor_task.h
 * @brief   Header interface for the I2C environmental sensor task.
 * ==============================================================================
 */
#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

/**
 * @brief FreeRTOS Task: Initializes the I2C bus and polls the BME280/BMP280.
 * @param pvParameters Unused task parameter pointer.
 */
void bme280_task(void *pvParameters);

#endif // SENSOR_TASK_H