/**
 * ==============================================================================
 * @file    ema_filter.h
 * @brief   Exponential Moving Average (EMA) filter for noisy sensor data.
 * @details 
 * A robust, lightweight professional filter ideal for embedded telemetry.
 * Memory complexity: O(1). Time complexity: O(1).
 * ==============================================================================
 */
#ifndef EMA_FILTER_H
#define EMA_FILTER_H

#include <stdbool.h>

/**
 * @brief EMA Filter state structure.
 */
typedef struct {
    float alpha;       ///< Smoothing factor [0.0, 1.0]. Lower = smoother.
    float state;       ///< Current filtered output
    bool initialized;  ///< Flag to prevent startup ramp-delay
} ema_filter_t;

/**
 * @brief Initializes the EMA filter with a specific smoothing factor.
 * @param filt Pointer to the filter structure.
 * @param alpha Smoothing factor (e.g., 0.2f for a 1Hz BME280).
 */
void ema_filter_init(ema_filter_t *filt, float alpha);

/**
 * @brief Updates the filter with a new raw reading and returns the smoothed value.
 * @param filt Pointer to the filter structure.
 * @param input Raw sensor measurement.
 * @return Smoothed measurement.
 */
float ema_filter_update(ema_filter_t *filt, float input);

#endif // EMA_FILTER_H
