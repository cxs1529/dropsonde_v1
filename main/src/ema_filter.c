/**
 * ==============================================================================
 * @file    ema_filter.c
 * @brief   Exponential Moving Average (EMA) filter implementation.
 * ==============================================================================
 */
#include "ema_filter.h"

void ema_filter_init(ema_filter_t *filt, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    
    filt->alpha = alpha;
    filt->state = 0.0f;
    filt->initialized = false;
}

float ema_filter_update(ema_filter_t *filt, float input) {
    if (!filt->initialized) {
        // Snap directly to the first reading to eliminate ramp-up lag
        filt->state = input;
        filt->initialized = true;
    } else {
        // Apply the exponential smoothing formula: Y = (a * X) + ((1-a) * Y_prev)
        filt->state = (filt->alpha * input) + ((1.0f - filt->alpha) * filt->state);
    }
    return filt->state;
}
