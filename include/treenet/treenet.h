/**
 * @file treenet.h
 * @brief Public API of the treenity mesh networking library.
 *
 * Usage overview
 * --------------
 * 1. Implement a @ref treenet_port_t for your hardware (see port.h).
 * 2. Reserve a static context buffer of @ref treenet_context_size() bytes.
 * 3. Call @ref treenet_init with your configuration and the port.
 * 4. Feed received frames into @ref treenet_rx (safe to call from an ISR).
 * 5. Call @ref treenet_poll regularly from your main loop or a timer.
 * 6. Send data with @ref treenet_send / @ref treenet_broadcast and react to
 *    @ref treenet_config_t::on_recv and @ref treenet_config_t::on_event.
 *
 * The library is non-blocking and single threaded from its own perspective:
 * all work happens inside treenet_poll(). The only function that may be called
 * concurrently (from an interrupt) is treenet_rx, which merely enqueues the
 * frame.
 */
#ifndef TREENET_H
#define TREENET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "types.h"
#include "port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque treenity instance. Storage is provided by the caller. */
typedef struct treenet treenet_t;

/**
 * @brief Application configuration passed to treenet_init().
 *
 * Zero-initialise the structure and set the fields you need. The two callbacks
 * are optional but strongly recommended.
 */
typedef struct {
    treenet_addr_t addr;      /**< this node's unique address */
    treenet_role_t role;      /**< node role (MASTER / NODE / REPEATER) */
    uint16_t       net_id;    /**< logical network id; frames from other nets are ignored */

    /**
     * @brief Called when a complete application datagram is received.
     *
     * @param t     library instance
     * @param src   address of the original sender
     * @param data  payload bytes
     * @param len   payload length
     * @param rssi  RSSI of the frame that delivered it, in dBm
     * @param snr   SNR of the frame that delivered it, in dB
     * @param hops  number of hops the datagram travelled
     */
    void (*on_recv)(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                    size_t len, int16_t rssi, int8_t snr, uint8_t hops);

    /**
     * @brief Called for asynchronous network events.
     *
     * @param t   library instance
     * @param ev  event code
     * @param arg event specific argument (for PARENT_CHANGED it points to the
     *            new parent address, may be NULL otherwise)
     */
    void (*on_event)(treenet_t *t, treenet_event_t ev, void *arg);

    /** Arbitrary user pointer echoed back in callbacks. */
    void *user;

    /** When true, unicast sends request hop-by-hop acknowledgement. */
    bool reliable;

    /**
     * Optional radio parameters. When @c spreading_factor is non-zero the
     * library assumes a LoRa modem and uses these values for time-on-air
     * estimation. Leave zeroed for non-LoRa transports.
     */
    treenet_radio_cfg_t radio;
} treenet_config_t;

/** Diagnostic snapshot of one neighbour. */
typedef struct {
    treenet_addr_t        addr;   /**< neighbour address */
    treenet_link_quality_t lq;    /**< smoothed link quality */
    uint16_t              rank;   /**< neighbour's advertised rank */
    uint32_t              age_ms; /**< time since the last beacon */
    bool                  is_parent; /**< true if this is our current parent */
} treenet_neighbor_info_t;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Size of the context object, in bytes.
 *
 * Use this to size a static buffer:
 * @code
 *   static uint8_t mem[512]; // must be >= treenet_context_size()
 *   treenet_t *t = treenet_init(mem, sizeof mem, &cfg, &port);
 * @endcode
 */
size_t treenet_context_size(void);

/**
 * @brief Initialise a treenity instance in caller-provided storage.
 *
 * No dynamic allocation is performed. The storage must remain valid for the
 * whole lifetime of the instance and must be at least treenet_context_size()
 * bytes and suitably aligned (8 bytes is enough).
 *
 * @return pointer to the instance, or NULL on invalid arguments / too small
 *         storage / missing mandatory port callbacks.
 */
treenet_t *treenet_init(void *storage, size_t storage_size,
                        const treenet_config_t *cfg, const treenet_port_t *port);

/* ------------------------------------------------------------------------- */
/* Runtime                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Drive the state machine. Call regularly (e.g. every 10..100 ms).
 *
 * Performs beaconing, neighbour ageing, route maintenance, queued TX and
 * event delivery. Never blocks.
 */
void treenet_poll(treenet_t *t);

/**
 * @brief Deliver a received frame to the library.
 *
 * May be called from an interrupt context; it only copies the frame into an
 * internal ring buffer. @p rssi and @p snr describe the link to the immediate
 * sender and are essential for link quality estimation.
 *
 * @param rssi  received signal strength in dBm
 * @param snr   signal to noise ratio in dB
 * @return 0 if accepted, negative if the buffer was full
 */
int treenet_rx(treenet_t *t, const uint8_t *buf, size_t len,
               int16_t rssi, int8_t snr);

/* ------------------------------------------------------------------------- */
/* Data plane                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Send a datagram to a specific node (routed towards the Master tree).
 * @return 0 if queued, negative on error (no route, too large, queue full).
 */
int treenet_send(treenet_t *t, treenet_addr_t dst, const void *data, size_t len);

/**
 * @brief Broadcast a datagram to the whole mesh using managed flooding.
 * @return 0 if queued, negative on error.
 */
int treenet_broadcast(treenet_t *t, const void *data, size_t len);

/* ------------------------------------------------------------------------- */
/* Introspection                                                              */
/* ------------------------------------------------------------------------- */

/** @return this node's address. */
treenet_addr_t treenet_addr(const treenet_t *t);

/** @return this node's role. */
treenet_role_t treenet_role(const treenet_t *t);

/** @return current parent address, or TREENET_ADDR_INVALID if none. */
treenet_addr_t treenet_parent(const treenet_t *t);

/** @return current rank (0 == Master). */
uint16_t treenet_rank(const treenet_t *t);

/** @return true when the node has a usable path towards the Master. */
bool treenet_is_connected(const treenet_t *t);

/** @return pointer to the live statistics block. */
const treenet_stats_t *treenet_stats(const treenet_t *t);

/**
 * @brief Copy up to @p max neighbour snapshots into @p out.
 * @return number of entries written.
 */
size_t treenet_neighbors(const treenet_t *t, treenet_neighbor_info_t *out,
                         size_t max);

/** @return human readable version string, e.g. "0.1.0". */
const char *treenet_version(void);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_H */
