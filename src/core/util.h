/**
 * @file util.h
 * @brief Small freestanding helpers: wrap-safe time arithmetic, byte order
 *        helpers, fixed point math and link-quality scoring.
 *
 * Everything here is header-only and has no dependencies beyond <stdint.h>, so
 * it can be used from interrupt context.
 */
#ifndef TREENET_UTIL_H
#define TREENET_UTIL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "treenet/config.h"
#include "treenet/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Compiler memory barrier                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Memory barriers for the lock-free receive ring.
 *
 * @c TN_BARRIER() is a pure compiler barrier. The ring additionally uses
 * @c TN_PUBLISH() (release) when the producer publishes a record and
 * @c TN_CONSUME() (acquire) when the consumer reads one. On weakly ordered
 * multi-core targets (for example ESP32 or RP2040) the release/acquire fences
 * emit the required hardware barrier (DMB on ARM); on x86 they are effectively
 * free. Without them a second core could observe an updated index before the
 * payload it refers to.
 */
#if defined(__GNUC__) || defined(__clang__)
#define TN_BARRIER() __asm__ volatile("" ::: "memory")
#define TN_PUBLISH() __atomic_thread_fence(__ATOMIC_RELEASE)
#define TN_CONSUME() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#elif defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>
#define TN_BARRIER() _ReadWriteBarrier()
#define TN_PUBLISH() do { _ReadWriteBarrier(); MemoryBarrier(); } while (0)
#define TN_CONSUME() do { MemoryBarrier(); _ReadWriteBarrier(); } while (0)
#else
#define TN_BARRIER() ((void)0)
#define TN_PUBLISH() ((void)0)
#define TN_CONSUME() ((void)0)
#endif

/* ------------------------------------------------------------------------- */
/* Min / max / clamp                                                          */
/* ------------------------------------------------------------------------- */

#define TN_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define TN_MAX(a, b) (((a) > (b)) ? (a) : (b))

/** Clamp @p v into [@p lo, @p hi]. */
static inline int32_t tn_clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Wrap-safe 32 bit millisecond time                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief True when time @p a is strictly after time @p b.
 *
 * Correct across the 32 bit millisecond wrap (every ~49 days) as long as the
 * two instants are less than 2^31 ms apart, which always holds for the short
 * intervals treenet deals with.
 */
static inline bool tn_time_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

/** @brief Elapsed milliseconds from @p from to @p to (wrap-safe). */
static inline uint32_t tn_elapsed(uint32_t from, uint32_t to)
{
    return (uint32_t)(to - from);
}

/* ------------------------------------------------------------------------- */
/* Little-endian byte order helpers                                           */
/* ------------------------------------------------------------------------- */

/** @brief Write a 16 bit value in little-endian order. */
static inline void tn_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/** @brief Write a 32 bit value in little-endian order. */
static inline void tn_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/** @brief Read a little-endian 16 bit value. */
static inline uint16_t tn_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** @brief Read a little-endian 32 bit value. */
static inline uint32_t tn_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------------- */
/* Fixed point Q8 helpers (256 == 1.0)                                        */
/* ------------------------------------------------------------------------- */

/** @brief Multiply two Q8 values, result is Q8. */
static inline uint16_t tn_q8_mul(uint16_t a, uint16_t b)
{
    return (uint16_t)(((uint32_t)a * (uint32_t)b) >> 8);
}

/* ------------------------------------------------------------------------- */
/* Link quality scoring                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Convert SNR (dB) into a Q8-style "badness" score: 0 == excellent.
 *
 * LoRa demodulation degrades sharply as SNR approaches the modem's limit and
 * becomes comfortable a few dB above it. The score spans 0..1024 so that a
 * marginal link is clearly worse than a merely mediocre one; this is what
 * makes the objective function prefer a good two-hop path over a single bad
 * link. The mapping is linear and can be tuned per modem family.
 *
 * @param snr_db smoothed SNR in dB
 * @return 0..1024 where 0 is a perfect link
 */
static inline uint16_t tn_snr_score_q8(int8_t snr_db)
{
    const int32_t best = 5;    /* dB at or above which the link is "free" */
    const int32_t worst = -20; /* dB at or below which the link is unusable */
    int32_t s = tn_clamp_i32((int32_t)snr_db, worst, best);
    /* worst -> 1024, best -> 0 */
    int32_t score = (best - s) * 1024 / (best - worst);
    return (uint16_t)score;
}

/**
 * @brief Convert an ETX value (Q8, 256 == 1.0) into a Q8-style badness score.
 *
 * ETX 1.0 (perfect) maps to 0, ETX 4.0 and above saturates at 1024. Kept for
 * symmetry with @ref tn_snr_score_q8 and used by diagnostics.
 */
static inline uint16_t tn_etx_score_q8(uint16_t etx_q8)
{
    int32_t etx = (int32_t)etx_q8;
    if (etx <= 256) return 0;
    if (etx >= 1024) return 1024;
    return (uint16_t)((etx - 256) * 1024 / (1024 - 256));
}

/* ------------------------------------------------------------------------- */
/* Random helpers                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Return a pseudo random value in [0, @p bound ).
 * @param rnd_value a fresh value from the port's rnd() callback
 */
static inline uint32_t tn_rand_below(uint32_t rnd_value, uint32_t bound)
{
    if (bound == 0u) return 0u;
    return rnd_value % bound;
}

#ifdef __cplusplus
}
#endif

#endif /* TREENET_UTIL_H */
