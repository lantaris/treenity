/**
 * @file ewma.h
 * @brief Exponentially weighted moving average for link metrics.
 *
 * Every radio measurement (RSSI, SNR, packet delivery ratio) is noisy. treenity
 * smooths all of them with an EWMA whose factor is 1/2^TREENET_EWMA_SHIFT
 * (1/8 by default). This keeps the parent selection stable while still reacting
 * to genuine link changes within a handful of samples.
 */
#ifndef TREENET_EWMA_H
#define TREENET_EWMA_H

#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Signed 16 bit EWMA state.
 *
 * The first sample is adopted verbatim so the average converges immediately
 * after a neighbour is discovered.
 */
typedef struct {
    int32_t value; /**< current average, scaled by 2^shift for precision */
    bool    init;  /**< false until the first sample is fed */
} tn_ewma16_t;

/** @brief Reset an EWMA to its "no data" state. */
static inline void tn_ewma16_reset(tn_ewma16_t *e)
{
    e->value = 0;
    e->init = false;
}

/**
 * @brief Feed one sample into the average.
 * @param e     average state
 * @param sample new value
 * @return current smoothed value (rounded)
 */
static inline int32_t tn_ewma16_push(tn_ewma16_t *e, int32_t sample)
{
    const int32_t shift = (int32_t)TREENET_EWMA_SHIFT;
    const int32_t div = (int32_t)(1u << TREENET_EWMA_SHIFT);
    if (!e->init) {
        e->value = sample << shift;
        e->init = true;
    } else {
        /* value += (sample - value) / 2^shift, kept in fixed point. */
        e->value += ((sample << shift) - e->value) / div;
    }
    return e->value >> shift;
}

/** @brief Current smoothed value without updating. */
static inline int32_t tn_ewma16_get(const tn_ewma16_t *e)
{
    if (!e->init) return 0;
    return e->value >> (int32_t)TREENET_EWMA_SHIFT;
}

#ifdef __cplusplus
}
#endif

#endif /* TREENET_EWMA_H */
