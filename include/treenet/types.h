/**
 * @file types.h
 * @brief Fundamental value types, enumerations and small POD structs shared
 *        across the treenity public API.
 */
#ifndef TREENET_TYPES_H
#define TREENET_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Addressing                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * Node address. 32 bit wide to comfortably cover networks of a few thousand
 * nodes and to leave room for vendor-prefixed identifiers (as done by other
 * LoRa meshes which derive the address from a hardware id).
 */
typedef uint32_t treenet_addr_t;

/** Broadcast address: every node within range processes the frame. */
#define TREENET_ADDR_BROADCAST ((treenet_addr_t)0xFFFFFFFFu)

/** Invalid/unset address. A freshly initialised node has this address. */
#define TREENET_ADDR_INVALID ((treenet_addr_t)0x00000000u)

/* ------------------------------------------------------------------------- */
/* Roles                                                                      */
/* ------------------------------------------------------------------------- */

/** Role of a node inside the treenity mesh. */
typedef enum {
    /** Ordinary node. Has a parent and routes traffic towards the Master. */
    TREENET_ROLE_NODE = 0,
    /** Root of the DODAG. Has rank 0, no parent, owns the topology. */
    TREENET_ROLE_MASTER = 1,
    /**
     * Infrastructure repeater. Like a NODE but rebroadcasts with higher
     * priority, mirroring the ROUTER/REPEATER roles known from other meshes.
     */
    TREENET_ROLE_REPEATER = 2,
    /**
     * End device / sensor. It sends and receives its own traffic but is not a
     * router: it never forwards other nodes' frames, never rebroadcasts floods
     * and is never chosen as a parent by other nodes. Leaves therefore sit at
     * the edge of the mesh and may sleep their radio.
     */
    TREENET_ROLE_LEAF = 3
} treenet_role_t;

/* ------------------------------------------------------------------------- */
/* Frame types                                                                */
/* ------------------------------------------------------------------------- */

/** Wire-level frame type, stored in the first nibble of a frame. */
typedef enum {
    TREENET_FRAME_BEACON = 0, /**< link-local hello carrying rank + metrics */
    TREENET_FRAME_DATA   = 1, /**< unicast application datagram */
    TREENET_FRAME_ACK    = 2, /**< hop-by-hop acknowledgement */
    TREENET_FRAME_FLOOD  = 3, /**< broadcast datagram via managed flooding */
    TREENET_FRAME_PROBE  = 4, /**< route-repair solicitation (local repair) */
    TREENET_FRAME_DAO    = 5  /**< destination advertisement (route install) */
} treenet_frame_type_t;

/* ------------------------------------------------------------------------- */
/* Rank                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Rank is an accumulated path cost towards the Master, in the same spirit as
 * RPL. The Master has rank 0; every hop adds the link cost plus a small fixed
 * increment, which guarantees a strictly increasing rank along any path and
 * therefore loop-free parent selection. The fixed part is deliberately small
 * so that link quality (ETX/SNR), not hop count, dominates the choice.
 */
#define TREENET_RANK_STEP 8u

/** Sentinel meaning "no route towards the Master". */
#define TREENET_RANK_INFINITE ((uint16_t)0xFFFFu)

/* ------------------------------------------------------------------------- */
/* Events                                                                     */
/* ------------------------------------------------------------------------- */

/** Asynchronous notifications delivered to the application callback. */
typedef enum {
    TREENET_EV_NETWORK_READY = 0, /**< node joined and has a usable route */
    TREENET_EV_JOINED,            /**< node acquired a parent for the first time */
    TREENET_EV_PARENT_CHANGED,    /**< node switched to a better/other parent */
    TREENET_EV_NEIGHBOR_ADDED,    /**< a new neighbour was discovered */
    TREENET_EV_NEIGHBOR_REMOVED,  /**< a neighbour timed out or left */
    TREENET_EV_ROUTE_LOST,        /**< no usable parent, node is isolated */
    TREENET_EV_TX_DONE,           /**< a reliable frame was acknowledged */
    TREENET_EV_TX_FAILED,         /**< a reliable frame exhausted retries */
    TREENET_EV_DISCONNECTED       /**< lost contact with the Master subtree */
} treenet_event_t;

/* ------------------------------------------------------------------------- */
/* Link quality                                                               */
/* ------------------------------------------------------------------------- */

/**
 * Quality of a single radio link. All values are smoothed with an EWMA and
 * refreshed on every received frame.
 */
typedef struct {
    int16_t rssi_dbm;    /**< last smoothed RSSI in dBm */
    int8_t  snr_db;      /**< last smoothed SNR in dB */
    uint16_t pdr_q8;     /**< packet delivery ratio, Q8 (256 == 100%) */
    uint16_t etx_q8;     /**< expected transmission count, Q8 (256 == 1.0) */
    uint16_t link_cost;  /**< composite link cost used by the objective function */
} treenet_link_quality_t;

/* ------------------------------------------------------------------------- */
/* Statistics                                                                 */
/* ------------------------------------------------------------------------- */

/** Runtime counters, useful for field diagnostics and benchmarking. */
typedef struct {
    uint32_t frames_tx;       /**< frames handed to the port for transmission */
    uint32_t frames_rx;       /**< frames received from the port */
    uint32_t frames_dropped;  /**< frames dropped locally (dup, bad, no route) */
    uint32_t retransmissions; /**< MAC level retransmissions */
    uint32_t beacons_tx;      /**< beacons transmitted */
    uint32_t beacons_rx;      /**< beacons received */
    uint32_t parent_changes;  /**< number of parent switches */
    uint32_t datagrams_tx;    /**< application datagrams accepted for TX */
    uint32_t datagrams_rx;    /**< application datagrams delivered up */
    uint32_t airtime_ms;      /**< accumulated estimated time-on-air */
} treenet_stats_t;

/* ------------------------------------------------------------------------- */
/* Radio configuration (optional port extension)                              */
/* ------------------------------------------------------------------------- */

/** Optional radio parameters a port may expose to the routing layer. */
typedef struct {
    uint32_t frequency_hz; /**< carrier frequency */
    uint8_t  spreading_factor; /**< LoRa SF (ignored by non-LoRa modems) */
    uint32_t bandwidth_hz; /**< LoRa bandwidth */
    uint8_t  coding_rate;  /**< LoRa coding rate */
    int8_t   tx_power_dbm; /**< transmit power */
} treenet_radio_cfg_t;

/* ------------------------------------------------------------------------- */
/* Address helpers                                                            */
/* ------------------------------------------------------------------------- */

/** @return true if @p addr is the broadcast address. */
static inline bool treenet_addr_is_broadcast(treenet_addr_t addr)
{
    return addr == TREENET_ADDR_BROADCAST;
}

#ifdef __cplusplus
}
#endif

#endif /* TREENET_TYPES_H */
