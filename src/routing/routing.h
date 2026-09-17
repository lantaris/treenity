/**
 * @file routing.h
 * @brief Downward route table and DODAG parent selection.
 *
 * treenity uses a Master-rooted DODAG (the same shape as RPL). Two pieces of
 * state live here:
 *
 *  - the downward route table (storing mode): for every node in a node's
 *    subtree, the child that leads towards it. It is populated by DAO messages
 *    that travel from a node up to the Master;
 *  - the parent selection logic: choosing the neighbour that offers the best
 *    path to the Master according to the composite objective function, with
 *    hysteresis and dwell time to prevent route flapping.
 */
#ifndef TREENET_ROUTING_H
#define TREENET_ROUTING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"
#include "treenet/types.h"
#include "treenet/treenet.h"
#include "link/neighbor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One downward route entry. */
typedef struct {
    bool           valid;     /**< slot in use */
    treenet_addr_t dst;       /**< final destination */
    treenet_addr_t next_hop;  /**< child to hand the frame to */
    uint16_t       cost;      /**< accumulated cost of the path */
    uint8_t        hops;      /**< hop count to the destination */
    uint32_t       updated_ms;/**< time of the last refresh */
} tn_route_t;

/** Fixed-size downward route table. */
typedef struct {
    tn_route_t entries[TREENET_MAX_ROUTES];
    uint32_t   count; /**< number of valid entries */
} tn_route_table_t;

/* --- Route table operations ---------------------------------------------- */

/** @brief Reset the route table. */
void tn_route_init(tn_route_table_t *t);

/** @brief Look up a fresh route to @p dst, or NULL. */
tn_route_t *tn_route_lookup(tn_route_table_t *t, treenet_addr_t dst,
                            uint32_t now, uint32_t timeout_ms);

/** @brief Install or refresh a route. @return the (new) entry or NULL. */
tn_route_t *tn_route_add(tn_route_table_t *t, treenet_addr_t dst,
                         treenet_addr_t next_hop, uint16_t cost, uint8_t hops,
                         uint32_t now);

/** @brief Remove the route to @p dst. */
void tn_route_remove(tn_route_table_t *t, treenet_addr_t dst);

/** @brief Remove every route whose next hop is @p next_hop. @return count. */
uint32_t tn_route_remove_via(tn_route_table_t *t, treenet_addr_t next_hop);

/** @brief Expire stale routes. @return number of entries removed. */
uint32_t tn_route_expire(tn_route_table_t *t, uint32_t now, uint32_t timeout_ms);

/* --- DODAG / parent selection -------------------------------------------- */

/**
 * @brief Feed a freshly received beacon into the routing decision.
 *
 * The beacon is recorded (neighbour table) and, if it advertises a better path
 * to the Master, may trigger a parent switch. Switching respects hysteresis
 * and the dwell time unless the current parent is dead.
 */
void tn_routing_on_beacon(treenet_t *t, treenet_addr_t from, uint16_t rank,
                          treenet_addr_t parent, uint8_t flags,
                          uint16_t interval_100ms, int16_t rssi, int8_t snr,
                          uint32_t now);

/**
 * @brief Evaluate all candidates and (re)select a parent if beneficial.
 * @return true when the parent changed
 */
bool tn_routing_select_parent(treenet_t *t, uint32_t now);

/** @brief Handle the loss of the current parent (timeout / neighbour gone). */
void tn_routing_on_parent_lost(treenet_t *t, uint32_t now);

/** @brief Periodic routing maintenance: route expiry and refresh. */
void tn_routing_tick(treenet_t *t, uint32_t now);

/**
 * @brief Compute the rank a node would get through a given neighbour.
 * @return the resulting rank, or TREENET_RANK_INFINITE if not viable
 */
uint16_t tn_routing_candidate_rank(const tn_neighbor_t *n);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_ROUTING_H */
