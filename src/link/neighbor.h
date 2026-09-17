/**
 * @file neighbor.h
 * @brief Neighbour table and link quality estimation.
 *
 * Every node keeps a small table of the nodes it can hear directly. For each
 * neighbour treenity maintains:
 *
 *  - smoothed RSSI and SNR (EWMA), taken from the metadata supplied by the
 *    port for every received frame;
 *  - an estimate of the packet delivery ratio (PDR) derived from the beacon
 *    stream: a missed beacon counts as a lost packet;
 *  - the resulting ETX and a composite link cost used by the objective
 *    function in the routing layer.
 *
 * The composite cost blends the SNR score and the ETX score using the weights
 * from config.h. It is deliberately a single 16 bit scalar so that parent
 * selection reduces to a comparison, exactly like RPL's accumulated rank.
 */
#ifndef TREENET_NEIGHBOR_H
#define TREENET_NEIGHBOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"
#include "treenet/types.h"
#include "core/ewma.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One row of the neighbour table. */
typedef struct {
    bool           valid;         /**< slot in use */
    treenet_addr_t addr;          /**< neighbour address */
    uint16_t       rank;          /**< last advertised rank */
    treenet_addr_t parent;        /**< last advertised parent */
    uint8_t        flags;         /**< last advertised beacon flags */

    tn_ewma16_t    rssi;          /**< smoothed RSSI (dBm) */
    tn_ewma16_t    snr;           /**< smoothed SNR (dB) */
    tn_ewma16_t    pdr;           /**< smoothed PDR, Q8 (256 == 100%) */
    uint16_t       etx_q8;        /**< 1/PDR, Q8 (256 == 1.0) */
    uint16_t       link_cost;     /**< composite link cost, 0..512 */

    uint32_t       last_seen_ms;  /**< time of the last frame from neighbour */
    uint32_t       last_beacon_ms;/**< time of the last beacon */
    uint32_t       est_interval_ms;/**< learned beacon interval */
    bool           have_interval; /**< est_interval_ms is valid */

    uint8_t        ack_fail;      /**< reliable frames that got no ACK in a row */
    uint32_t       suspect_until_ms;/**< link excluded from parent selection */
} tn_neighbor_t;

/** Fixed-size neighbour table. */
typedef struct {
    tn_neighbor_t entries[TREENET_MAX_NEIGHBORS];
    uint32_t      count; /**< number of valid entries (for diagnostics) */
} tn_neighbor_table_t;

/** @brief Reset the table. */
void tn_neighbor_table_init(tn_neighbor_table_t *tbl);

/** @brief Find a neighbour by address, or NULL. */
tn_neighbor_t *tn_neighbor_find(tn_neighbor_table_t *tbl, treenet_addr_t addr);

/** @brief Find or allocate a slot (evicting the oldest entry when full). */
tn_neighbor_t *tn_neighbor_get_or_create(tn_neighbor_table_t *tbl,
                                         treenet_addr_t addr, uint32_t now);

/**
 * @brief Update link metrics from any received frame.
 *
 * Refreshes RSSI/SNR averages and the "last seen" timestamp. Called for every
 * frame, not only beacons, so link quality tracks the actual traffic.
 */
void tn_neighbor_note_frame(tn_neighbor_table_t *tbl, treenet_addr_t addr,
                            int16_t rssi_dbm, int8_t snr_db, uint32_t now);

/**
 * @brief Process an incoming beacon and refresh PDR/ETX/link cost.
 *
 * @param rank          sender's advertised rank
 * @param parent        sender's advertised parent
 * @param flags         beacon flags
 * @param interval_100ms sender's advertised beacon interval (100 ms units)
 */
void tn_neighbor_note_beacon(tn_neighbor_table_t *tbl, treenet_addr_t addr,
                             uint16_t rank, treenet_addr_t parent,
                             uint8_t flags, uint16_t interval_100ms,
                             int16_t rssi_dbm, int8_t snr_db, uint32_t now);

/**
 * @brief Age the table, removing neighbours that have not been heard for
 *        @p timeout_ms.
 * @return number of entries removed
 */
uint32_t tn_neighbor_age(tn_neighbor_table_t *tbl, uint32_t now,
                         uint32_t timeout_ms);

/** @brief Recompute ETX and the composite link cost of one neighbour. */
void tn_neighbor_update_cost(tn_neighbor_t *n);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_NEIGHBOR_H */
