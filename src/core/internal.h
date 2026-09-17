/**
 * @file internal.h
 * @brief Internal context of a treenity instance and the interfaces between
 *        its modules (MAC, link, routing, API).
 *
 * This header is private to the library. Applications must never include it:
 * the public surface is treenet/treenet.h.
 */
#ifndef TREENET_INTERNAL_H
#define TREENET_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"
#include "treenet/types.h"
#include "treenet/port.h"
#include "treenet/treenet.h"

#include "core/util.h"
#include "core/ewma.h"
#include "core/ringbuf.h"
#include "core/timer.h"
#include "mac/frame.h"
#include "mac/dupcache.h"
#include "mac/airtime.h"
#include "link/neighbor.h"
#include "routing/routing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The reassembly bitmap is 16 bits wide, so the fragment count must fit. */
#if TREENET_MAX_FRAGMENTS > 16
#error "TREENET_MAX_FRAGMENTS must be <= 16 (16 bit fragment bitmask)"
#endif

/* ------------------------------------------------------------------------- */
/* Statistics                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Update a statistics counter only when TREENET_ENABLE_STATS is set.
 *
 * With statistics disabled every update compiles away; the statistics block is
 * still present (zeroed) so treenet_stats() keeps returning a valid pointer.
 */
#if TREENET_ENABLE_STATS
#define TN_STAT_INC(t, field) ((t)->stats.field++)
#define TN_STAT_ADD(t, field, v) ((t)->stats.field += (v))
#else
#define TN_STAT_INC(t, field) ((void)(t))
#define TN_STAT_ADD(t, field, v) ((void)(t), (void)(v))
#endif

/* ------------------------------------------------------------------------- */
/* Transmit slot                                                              */
/* ------------------------------------------------------------------------- */

/**
 * One queued outbound frame. A slot holds the fully encoded frame plus the
 * bookkeeping needed for CSMA/CA scheduling and hop-by-hop retransmission.
 */
typedef struct {
    bool           valid;      /**< slot in use */
    uint16_t       len;        /**< encoded frame length */
    bool           reliable;   /**< waiting for an ACK */
    uint8_t        attempts;   /**< transmissions performed so far */
    uint32_t       next_tx_ms; /**< earliest time to (re)transmit */
    treenet_addr_t ack_dst;    /**< node expected to acknowledge */
    uint16_t       ack_seq;    /**< sequence number being acknowledged */
    uint8_t        buf[TREENET_MTU]; /**< encoded frame bytes */
} tn_tx_slot_t;

/* ------------------------------------------------------------------------- */
/* Event queue                                                                */
/* ------------------------------------------------------------------------- */

/** Small ring of pending application events. */
typedef struct {
    treenet_event_t ev[TREENET_EVENT_QUEUE_SIZE];
    void           *arg[TREENET_EVENT_QUEUE_SIZE];
    uint8_t         head;
    uint8_t         tail;
    uint8_t         count;
} tn_eventq_t;

/* ------------------------------------------------------------------------- */
/* Reassembly slots (fragmentation)                                           */
/* ------------------------------------------------------------------------- */

/** One in-flight inbound fragmented datagram. */
typedef struct {
    bool           valid;      /**< slot in use */
    treenet_addr_t src;        /**< original sender */
    uint16_t       dgram_id;   /**< datagram identifier */
    uint8_t        count;      /**< total fragments */
    uint8_t        received;   /**< fragments received so far */
    uint16_t       total_len;  /**< assembled length */
    uint32_t       last_ms;    /**< time of the last fragment */
    uint16_t       got_mask;   /**< bitmap of fragments already received */
    uint8_t        buf[TREENET_MAX_DATAGRAM]; /**< assembly buffer */
    uint16_t       frag_len[TREENET_MAX_FRAGMENTS]; /**< per-fragment lengths */
} tn_reasm_t;

/* ------------------------------------------------------------------------- */
/* The instance                                                               */
/* ------------------------------------------------------------------------- */

/**
 * Full treenity state. The structure is exposed only to the library; the
 * application receives an opaque pointer and must not touch these fields.
 */
struct treenet {
    /* --- configuration (copied from treenet_init) --- */
    treenet_config_t cfg;
    treenet_port_t   port;

    treenet_addr_t   addr;   /**< own address */
    treenet_role_t   role;   /**< own role */
    uint16_t         net_id; /**< logical network id */
    bool             initialized;

    /* --- time and sequence --- */
    uint32_t now_ms;
    uint16_t seq;

    /* --- routing / DODAG state --- */
    uint16_t       rank;            /**< current accumulated rank */
    treenet_addr_t parent;          /**< current parent, INVALID if none */
    uint16_t       parent_rank;     /**< rank advertised by the parent */
    uint32_t       parent_since;    /**< when the current parent was chosen */
    uint32_t       parent_beacon_ms;/**< last beacon from the current parent */
    bool           connected;       /**< a path to the Master exists */
    bool           joined;          /**< emitted JOINED at least once */
    uint32_t       dao_last_ms;     /**< last DAO transmission */
    uint32_t       probe_last_ms;   /**< last PROBE transmission (parent search) */

    tn_neighbor_table_t neighbors;  /**< direct neighbours */
    tn_route_table_t    routes;     /**< downward routes (storing mode) */

    /* --- MAC state --- */
    tn_dupcache_t dup;              /**< duplicate suppression */
    tn_ringbuf_t  rx;               /**< inbound frame queue */
    uint8_t       rx_storage[TREENET_RX_RING_BYTES];

    tn_tx_slot_t  tx[TREENET_TX_QUEUE_SIZE]; /**< outbound frame slots */

    tn_reasm_t    reasm[TREENET_REASSEMBLY_SLOTS]; /**< reassembly state */

    /* --- events --- */
    tn_eventq_t events;

    /* --- timers --- */
    tn_timer_t beacon_timer;  /**< next beacon (Trickle) */

    /* --- Trickle state --- */
    uint32_t trickle_I;       /**< current beacon interval */

    /* --- airtime --- */
    tn_lora_params_t lora;    /**< LoRa parameters, if known */
    bool             has_lora;/**< true when lora is valid */

    /* --- statistics --- */
    treenet_stats_t stats;

    /* --- scratch --- */
    uint8_t work[TREENET_MTU];/**< scratch buffer for TX assembly */
};

/* ------------------------------------------------------------------------- */
/* Cross-module services provided by the core (treenet.c)                     */
/* ------------------------------------------------------------------------- */

/** @brief Queue an application event for delivery in treenet_poll(). */
void tn_emit_event(treenet_t *t, treenet_event_t ev, void *arg);

/** @brief Allocate the next sequence number for this node. */
uint16_t tn_next_seq(treenet_t *t);

/** @brief Emit a log line through the port, if configured. */
void tn_log(treenet_t *t, int level, const char *msg);

/**
 * @brief Queue a frame for transmission.
 *
 * The MAC rewrites the header's "previous hop" field with this node's address,
 * so the receiver can ACK the link and routing can install a route back
 * through us.
 *
 * @param frame    decoded frame; payload is copied
 * @param reliable request a hop-by-hop ACK from @p next_hop
 * @param next_hop node that should receive/forward the frame (used to match
 *                 the ACK); ignored when @p reliable is false
 * @param delay_ms initial CSMA/contention delay
 * @return 0 on success, negative when the queue is full
 */
int tn_tx_submit(treenet_t *t, const tn_frame_t *frame, bool reliable,
                 treenet_addr_t next_hop, uint32_t delay_ms);

/* ------------------------------------------------------------------------- */
/* Link layer (beaconing)                                                     */
/* ------------------------------------------------------------------------- */

/** @brief Build and queue a beacon advertising our rank/parent. */
void tn_beacon_send(treenet_t *t, uint32_t now);

/** @brief Trickle maintenance; may schedule or send beacons. */
void tn_beacon_tick(treenet_t *t, uint32_t now);

/** @brief Reset the Trickle interval to its minimum (topology changed). */
void tn_beacon_reset(treenet_t *t, uint32_t now);

/* ------------------------------------------------------------------------- */
/* Routing helpers implemented in routing.c                                   */
/* ------------------------------------------------------------------------- */

/** @brief Build and queue a DAO towards the Master. */
void tn_dao_send(treenet_t *t, uint32_t now);

/** @brief Handle an inbound DAO frame (route install + forwarding). */
void tn_dao_handle(treenet_t *t, const tn_frame_t *f, uint32_t now);

/* ------------------------------------------------------------------------- */
/* Forwarding (implemented in treenet.c)                                      */
/* ------------------------------------------------------------------------- */

/** @brief Process a decoded inbound frame. */
void tn_process_frame(treenet_t *t, const tn_frame_t *f, int16_t rssi,
                      int8_t snr, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_INTERNAL_H */
