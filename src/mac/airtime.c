/**
 * @file airtime.c
 * @brief LoRa time-on-air computation (Semtech AN1200.13).
 */
#include "airtime.h"

#include "core/util.h"

/** Integer ceiling division for positive operands. */
static uint32_t ceil_div(uint32_t a, uint32_t b)
{
    return (a + b - 1u) / b;
}

uint32_t tn_lora_toa_us(const tn_lora_params_t *p, size_t bytes)
{
    if (p == NULL || p->sf == 0 || p->bw_hz == 0) {
        return tn_airtime_estimate_us(p, bytes);
    }

    const int32_t sf = (int32_t)p->sf;
    /* Symbol duration in microseconds: 2^SF / BW. */
    const uint32_t tsym_us = (uint32_t)(((uint64_t)1u << sf) * 1000000u / p->bw_hz);

    /* Low data rate optimisation is mandatory for SF11/SF12 at 125 kHz. */
    const int32_t de = (sf >= 11 && p->bw_hz <= 125000u) ? 1 : 0;

    const int32_t pl = (int32_t)bytes;
    const int32_t crc = p->crc ? 1 : 0;
    const int32_t ih = p->explicit_hdr ? 0 : 1;
    const int32_t cr = (int32_t)p->cr; /* 1..4 */

    /* Payload symbol count. */
    int32_t num = 8 * pl - 4 * sf + 28 + 16 * crc - 20 * ih;
    int32_t den = 4 * (sf - 2 * de);
    if (den <= 0) den = 1;
    int32_t n = ceil_div((uint32_t)TN_MAX(num, 0), (uint32_t)den) * (cr + 4);
    if (n < 0) n = 0;
    int32_t payload_sym = 8 + n;

    /* Preamble: (n_preamble + 4.25) symbols. */
    uint32_t t_preamble_us = (uint32_t)(((uint64_t)tsym_us *
                             ((uint32_t)p->preamble * 4u + 17u)) / 4u);
    uint32_t t_payload_us = (uint32_t)payload_sym * tsym_us;

    return t_preamble_us + t_payload_us;
}

uint32_t tn_airtime_estimate_us(const tn_lora_params_t *lora, size_t bytes)
{
    if (lora != NULL && lora->sf != 0) {
        return tn_lora_toa_us(lora, bytes);
    }
    /* Conservative generic fallback: 1 ms per byte. */
    return (uint32_t)bytes * 1000u;
}
