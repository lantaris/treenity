/**
 * @file airtime.h
 * @brief Time-on-air estimation and airtime accounting.
 *
 * LoRa links are constrained by regional duty-cycle limits (e.g. 1% in the
 * EU868 band). Knowing how long a frame occupies the channel is required both
 * for statistics and for the transmit budget that keeps treenity compliant.
 * The formula follows Semtech application note AN1200.13.
 */
#ifndef TREENET_AIRTIME_H
#define TREENET_AIRTIME_H

#include <stddef.h>
#include <stdint.h>

#include "treenet/config.h"
#include "treenet/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** LoRa modem parameters used for time-on-air computation. */
typedef struct {
    uint8_t  sf;         /**< spreading factor 6..12 */
    uint32_t bw_hz;      /**< bandwidth in Hz (125000, 250000, 500000) */
    uint8_t  cr;         /**< coding rate denominator - 4 (1 == 4/5 .. 4 == 4/8) */
    uint16_t preamble;   /**< preamble length in symbols */
    uint8_t  explicit_hdr;/**< 1 for explicit header (default) */
    uint8_t  crc;        /**< 1 when a payload CRC is present */
} tn_lora_params_t;

/**
 * @brief Estimate time-on-air of a LoRa frame in microseconds.
 *
 * @param p     modem parameters; if p->sf is 0 a generic linear estimate is
 *              used instead (see tn_airtime_estimate_us)
 * @param bytes frame length in bytes
 * @return estimated airtime in microseconds
 */
uint32_t tn_lora_toa_us(const tn_lora_params_t *p, size_t bytes);

/**
 * @brief Estimate airtime for an arbitrary modem.
 *
 * If @p lora is NULL (or has sf == 0) a conservative linear model of
 * 1000 us per byte is used, which is fine for statistics but should be
 * overridden by a port that knows its modem better.
 */
uint32_t tn_airtime_estimate_us(const tn_lora_params_t *lora, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_AIRTIME_H */
