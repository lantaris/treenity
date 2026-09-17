/**
 * @file port.h
 * @brief Hardware abstraction layer (HAL) contract for treenity.
 *
 * The core library never talks to a microcontroller, a radio or an RTOS
 * directly. Everything hardware specific is expressed as a table of function
 * pointers (dependency injection) that the integrator fills in. This is the
 * same pattern used by Contiki-NG's NETSTACK and by the RadioHead driver
 * layer, and it is what makes treenity portable to arbitrary platforms.
 *
 * A minimal port only needs three functions: @ref tx, @ref now_ms and
 * @ref rnd. All remaining callbacks are optional and have sensible defaults.
 */
#ifndef TREENET_PORT_H
#define TREENET_PORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transmit one raw frame.
 *
 * The implementation must hand the bytes to the modem as a single packet. It
 * must be non-blocking or at least bounded in time; long blocking transmits
 * are acceptable for simple ports but a queue based implementation is
 * preferred. treenet performs channel access (CSMA/CA) itself, however the
 * port may still consult a hardware CAD signal via @ref channel_free.
 *
 * @param buf  frame bytes, valid only for the duration of the call
 * @param len  frame length in bytes
 * @return 0 on success, negative on failure
 */
typedef int (*treenet_tx_fn)(const uint8_t *buf, size_t len);

/**
 * @brief Monotonic millisecond clock.
 *
 * Must be free running and wrap around naturally on 32 bit overflow. treenet
 * performs all comparisons wrap-safe, so a 49 day wrap is handled correctly.
 *
 * @return current time in milliseconds
 */
typedef uint32_t (*treenet_now_fn)(void);

/**
 * @brief Pseudo random number source used for backoff jitter.
 *
 * A cheap xorshift/LFSR is sufficient. Must not be cryptographically secure.
 *
 * @return 32 bits of pseudo random data
 */
typedef uint32_t (*treenet_rnd_fn)(void);

/**
 * @brief Optional carrier sense / channel activity detection.
 *
 * @return true when the medium is considered free, false when busy.
 *         If not provided treenet assumes the channel is always free.
 */
typedef bool (*treenet_channel_free_fn)(void);

/**
 * @brief Optional radio reconfiguration hook.
 *
 * Called when the routing layer wants to change radio parameters (for example
 * to trade range for airtime). Ports that do not support it leave it NULL.
 *
 * @return 0 on success, negative on failure
 */
typedef int (*treenet_set_radio_fn)(const treenet_radio_cfg_t *cfg);

/**
 * @brief Optional logging sink.
 *
 * @param level 0=error 1=warn 2=info 3=debug
 * @param msg   NUL terminated message
 */
typedef void (*treenet_log_fn)(int level, const char *msg);

/**
 * @brief Optional wake-timer hook for tickless low-power idle.
 *
 * treenet_poll() calls this at the end of every poll to tell the port when it
 * next needs attention. The port should arm (or re-arm, replacing any pending
 * value) a one-shot timer that fires after @p delay_ms and then calls
 * treenet_poll() again.
 *
 * Semantics:
 *  - the call is a set/replace, not an additional arm;
 *  - @p delay_ms == 0 means work is already due: poll again as soon as possible
 *    (clamp to a small non-zero value if the timer cannot do 0);
 *  - @p delay_ms == UINT32_MAX means nothing is scheduled: do not arm.
 *
 * This hook only covers the library's deadlines. Received frames must still
 * wake the application independently (e.g. the modem's DIO interrupt calling
 * treenet_rx()); the port must then call treenet_poll() as usual.
 */
typedef void (*treenet_timer_arm_fn)(uint32_t delay_ms);

/**
 * @brief Port descriptor.
 *
 * The first three members are mandatory. Zero-initialise the structure and
 * fill in what the platform provides; NULL optionals fall back to defaults.
 */
typedef struct {
    treenet_tx_fn           tx;            /**< mandatory: transmit a frame */
    treenet_now_fn          now_ms;        /**< mandatory: millisecond clock */
    treenet_rnd_fn          rnd;           /**< mandatory: random source */

    treenet_channel_free_fn channel_free;  /**< optional: CAD */
    treenet_set_radio_fn    set_radio;     /**< optional: radio reconfig */
    treenet_log_fn          log;           /**< optional: log sink */
    treenet_timer_arm_fn    timer_arm;     /**< optional: tickless wake timer */
} treenet_port_t;

#ifdef __cplusplus
}
#endif

#endif /* TREENET_PORT_H */
