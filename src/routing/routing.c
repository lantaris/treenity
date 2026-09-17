/**
 * @file routing.c
 * @brief Downward route table and Master-rooted DODAG parent selection.
 *
 * The parent selection is the heart of treenity's "best quality" behaviour. A
 * node periodically hears beacons from its neighbours; each beacon advertises
 * the neighbour's rank (accumulated path cost to the Master). The node
 * combines that rank with the measured link cost of the link to that neighbour
 * to obtain a candidate rank, and picks the neighbour with the lowest one.
 *
 * Two safeguards keep the topology stable:
 *   - hysteresis: a new parent must beat the current one by at least
 *     TREENET_PARENT_HYSTERESIS_PCT percent, otherwise the node stays put;
 *   - dwell time: a node will not switch parents more often than every
 *     TREENET_PARENT_DWELL_MS, unless the current parent has actually died.
 */
#include "routing.h"

#include <string.h>

#include "core/internal.h"

/* ========================================================================= */
/* Route table                                                                */
/* ========================================================================= */

void tn_route_init(tn_route_table_t *t)
{
    memset(t->entries, 0, sizeof(t->entries));
    t->count = 0;
}

tn_route_t *tn_route_lookup(tn_route_table_t *t, treenet_addr_t dst,
                            uint32_t now, uint32_t timeout_ms)
{
    for (size_t i = 0; i < TREENET_MAX_ROUTES; i++) {
        tn_route_t *r = &t->entries[i];
        if (!r->valid || r->dst != dst) continue;
        if (tn_elapsed(r->updated_ms, now) > timeout_ms) {
            r->valid = false;
            if (t->count > 0) t->count--;
            return NULL;
        }
        return r;
    }
    return NULL;
}

/* @return a free slot, or the oldest one to recycle. */
static tn_route_t *route_slot(tn_route_table_t *t, uint32_t now)
{
    tn_route_t *oldest = &t->entries[0];
    uint32_t max_age = 0;
    for (size_t i = 0; i < TREENET_MAX_ROUTES; i++) {
        tn_route_t *r = &t->entries[i];
        if (!r->valid) return r;
        uint32_t age = tn_elapsed(r->updated_ms, now);
        if (age >= max_age) {
            max_age = age;
            oldest = r;
        }
    }
    return oldest;
}

tn_route_t *tn_route_add(tn_route_table_t *t, treenet_addr_t dst,
                         treenet_addr_t next_hop, uint16_t cost, uint8_t hops,
                         uint32_t now)
{
    tn_route_t *r = NULL;
    for (size_t i = 0; i < TREENET_MAX_ROUTES; i++) {
        if (t->entries[i].valid && t->entries[i].dst == dst) {
            r = &t->entries[i];
            break;
        }
    }
    if (r == NULL) {
        r = route_slot(t, now);
        if (!r->valid) {
            t->count++;
        }
        r->valid = true;
        r->dst = dst;
    }
    r->next_hop = next_hop;
    r->cost = cost;
    r->hops = hops;
    r->updated_ms = now;
    return r;
}

uint32_t tn_route_remove_via(tn_route_table_t *t, treenet_addr_t next_hop)
{
    uint32_t removed = 0;
    for (size_t i = 0; i < TREENET_MAX_ROUTES; i++) {
        tn_route_t *r = &t->entries[i];
        if (r->valid && r->next_hop == next_hop) {
            r->valid = false;
            if (t->count > 0) t->count--;
            removed++;
        }
    }
    return removed;
}

uint32_t tn_route_expire(tn_route_table_t *t, uint32_t now, uint32_t timeout_ms)
{
    uint32_t removed = 0;
    for (size_t i = 0; i < TREENET_MAX_ROUTES; i++) {
        tn_route_t *r = &t->entries[i];
        if (!r->valid) continue;
        if (tn_elapsed(r->updated_ms, now) > timeout_ms) {
            r->valid = false;
            if (t->count > 0) t->count--;
            removed++;
        }
    }
    return removed;
}

/* ========================================================================= */
/* DODAG / parent selection                                                   */
/* ========================================================================= */

uint16_t tn_routing_candidate_rank(const tn_neighbor_t *n)
{
    if (n == NULL) return TREENET_RANK_INFINITE;
    /* Only routers may be used as a parent; leaves never advertise this. */
    if ((n->flags & TN_BEACON_ROUTER) == 0) return TREENET_RANK_INFINITE;
    if (n->rank >= TREENET_RANK_INFINITE) return TREENET_RANK_INFINITE;

    /* Rank increase: the link cost plus a small fixed step. The step keeps the
     * rank strictly increasing (loop-free) while link quality dominates. */
    uint32_t inc = (uint32_t)TREENET_RANK_STEP + n->link_cost;
    uint32_t r = (uint32_t)n->rank + inc;
    if (r >= (uint32_t)TREENET_RANK_INFINITE) return TREENET_RANK_INFINITE;
    return (uint16_t)r;
}

/** @return true if the neighbour has been heard from recently enough. */
static bool neighbor_fresh(const tn_neighbor_t *n, uint32_t now)
{
    return n->valid &&
           tn_elapsed(n->last_seen_ms, now) <= TREENET_PARENT_TIMEOUT_MS;
}

/**
 * @brief Whether a neighbour may be used as a parent right now.
 *
 * A link is unusable if it is stale (no beacon) or "suspect" because several
 * reliable frames were not acknowledged (see the fast failure detector in
 * tn_tx_poll).
 */
static bool neighbor_usable(const tn_neighbor_t *n, uint32_t now)
{
    return neighbor_fresh(n, now) &&
           !tn_time_after(n->suspect_until_ms, now);
}

/* Select the neighbour with the lowest candidate rank. */
static tn_neighbor_t *best_candidate(treenet_t *t, uint16_t *out_rank)
{
    tn_neighbor_t *best = NULL;
    uint16_t best_rank = TREENET_RANK_INFINITE;

    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS; i++) {
        tn_neighbor_t *n = &t->neighbors.entries[i];
        if (!neighbor_usable(n, t->now_ms)) continue;
        if (n->addr == t->addr) continue;

        uint16_t r = tn_routing_candidate_rank(n);
        if (r < best_rank) {
            best_rank = r;
            best = n;
        }
    }
    if (out_rank) *out_rank = best_rank;
    return best;
}

/* Apply a parent switch, emitting the right events and advertising the new
 * downward path with a DAO. */
static void routing_set_parent(treenet_t *t, treenet_addr_t new_parent,
                               uint16_t new_rank, uint32_t now)
{
    bool first = !t->joined;
    bool changed = (new_parent != t->parent);

    t->parent = new_parent;
    t->rank = new_rank;
    t->parent_since = now;
    /* Start the liveness timer from the moment we adopt the parent, so a
     * freshly chosen parent is not immediately considered stale because the
     * previous parent's beacon timestamp is older. */
    t->parent_beacon_ms = now;
    t->connected = true;

    if (first) {
        t->joined = true;
        tn_emit_event(t, TREENET_EV_JOINED, NULL);
        tn_emit_event(t, TREENET_EV_NETWORK_READY, NULL);
    } else if (changed) {
        TN_STAT_INC(t, parent_changes);
        treenet_addr_t *arg = &t->parent;
        tn_emit_event(t, TREENET_EV_PARENT_CHANGED, arg);
    }

    if (changed || first) {
        /* Advertise the new parent quickly: reset the Trickle interval so
         * neighbours learn the new rank and route without waiting for the
         * grown interval to elapse. */
        tn_beacon_reset(t, now);
        tn_dao_send(t, now);
    }
}

void tn_routing_on_beacon(treenet_t *t, treenet_addr_t from, uint16_t rank,
                          treenet_addr_t parent, uint8_t flags,
                          uint16_t interval_100ms, int16_t rssi, int8_t snr,
                          uint32_t now)
{
    /* Refresh the neighbour entry first so the link cost is up to date. */
    tn_neighbor_note_beacon(&t->neighbors, from, rank, parent, flags,
                            interval_100ms, rssi, snr, now);

    /* A beacon from the current parent refreshes its liveness. */
    if (from == t->parent) {
        t->parent_beacon_ms = now;
        t->parent_rank = rank;
    }

    (void)tn_routing_select_parent(t, now);
}

bool tn_routing_select_parent(treenet_t *t, uint32_t now)
{
    /* The Master is the root: rank 0, no parent, always "connected". */
    if (t->role == TREENET_ROLE_MASTER) {
        t->rank = 0;
        t->parent = TREENET_ADDR_INVALID;
        t->connected = true;
        if (!t->joined) {
            t->joined = true;
            tn_emit_event(t, TREENET_EV_JOINED, NULL);
            tn_emit_event(t, TREENET_EV_NETWORK_READY, NULL);
        }
        return false;
    }

    uint16_t best_rank = TREENET_RANK_INFINITE;
    tn_neighbor_t *best = best_candidate(t, &best_rank);

    tn_neighbor_t *cur = (t->parent != TREENET_ADDR_INVALID)
                             ? tn_neighbor_find(&t->neighbors, t->parent)
                             : NULL;
    uint16_t cur_rank = tn_routing_candidate_rank(cur);
    bool cur_alive = (cur != NULL) && neighbor_usable(cur, now) &&
                     (cur_rank != TREENET_RANK_INFINITE);

    if (cur_alive) {
        /* Keep the parent, but keep our rank in sync with its current rank. */
        t->rank = cur_rank;

        if (best != NULL && best->addr != t->parent) {
            uint32_t dwell_ok =
                tn_elapsed(t->parent_since, now) >= TREENET_PARENT_DWELL_MS;
            /* Hysteresis: switch only if the candidate is clearly better. */
            uint32_t threshold =
                ((uint32_t)cur_rank * (100u - TREENET_PARENT_HYSTERESIS_PCT)) / 100u;
            if (dwell_ok && best_rank < threshold) {
                routing_set_parent(t, best->addr, best_rank, now);
                return true;
            }
        }
        return false;
    }

    /* Current parent is gone or unusable. */
    if (best != NULL) {
        bool was_connected = t->connected;
        routing_set_parent(t, best->addr, best_rank, now);
        (void)was_connected;
        return true;
    }

    /* No viable parent at all. */
    if (t->connected || t->parent != TREENET_ADDR_INVALID) {
        t->parent = TREENET_ADDR_INVALID;
        t->rank = TREENET_RANK_INFINITE;
        t->connected = false;
        tn_emit_event(t, TREENET_EV_ROUTE_LOST, NULL);
    }
    return false;
}

void tn_routing_on_parent_lost(treenet_t *t, uint32_t now)
{
    if (t->parent != TREENET_ADDR_INVALID) {
        tn_route_remove_via(&t->routes, t->parent);
    }
    t->parent = TREENET_ADDR_INVALID;
    t->rank = TREENET_RANK_INFINITE;
    t->connected = false;
    tn_emit_event(t, TREENET_EV_DISCONNECTED, NULL);

    /* Try to find an alternative immediately. */
    (void)tn_routing_select_parent(t, now);
}

void tn_routing_tick(treenet_t *t, uint32_t now)
{
    if (t->role == TREENET_ROLE_MASTER) {
        return;
    }

    /* If the parent's neighbour entry disappeared (aged out or evicted from a
     * full table) the parent is unusable even though its beacon timer may not
     * have expired yet. */
    if (t->parent != TREENET_ADDR_INVALID &&
        tn_neighbor_find(&t->neighbors, t->parent) == NULL) {
        tn_log(t, 1, "parent entry gone");
        tn_routing_on_parent_lost(t, now);
        return;
    }

    /* Detect a dead parent. */
    if (t->parent != TREENET_ADDR_INVALID &&
        tn_elapsed(t->parent_beacon_ms, now) > TREENET_PARENT_TIMEOUT_MS) {
        tn_log(t, 1, "parent timeout");
        tn_routing_on_parent_lost(t, now);
    }
}

/* ========================================================================= */
/* DAO (destination advertisement)                                            */
/* ========================================================================= */

void tn_dao_send(treenet_t *t, uint32_t now)
{
    if (t->role == TREENET_ROLE_MASTER) return;
    if (t->parent == TREENET_ADDR_INVALID) return;

    uint8_t payload[TN_DAO_PAYLOAD_LEN];
    tn_dao_payload_t d;
    d.origin = t->addr;
    d.hops = 0;
    tn_dao_encode(payload, &d);

    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DAO;
    f.flags = TN_FLAG_WANT_ACK; /* routes must not be lost to fading */
    f.src = t->addr;
    f.dst = t->parent;
    f.seq = tn_next_seq(t);
    f.net_id = t->net_id;
    f.hop_limit = TREENET_MAX_HOPS;
    f.payload = payload;
    f.payload_len = TN_DAO_PAYLOAD_LEN;

    if (tn_tx_submit(t, &f, true, t->parent, 0) == 0) {
        t->dao_last_ms = now;
    }
}

void tn_dao_handle(treenet_t *t, const tn_frame_t *f, uint32_t now)
{
    /* Leaves are not routers: they never hold or forward downward routes. */
    if (t->role == TREENET_ROLE_LEAF) {
        return;
    }

    tn_dao_payload_t d;
    if (!tn_dao_decode(f->payload, f->payload_len, &d)) {
        return;
    }
    /* Reject nonsensical routes: a corrupted DAO must not install a bogus
     * entry or make us forward garbage towards the Master. */
    if (d.origin == TREENET_ADDR_INVALID ||
        d.origin == TREENET_ADDR_BROADCAST ||
        d.hops > TREENET_MAX_HOPS ||
        f->prev == TREENET_ADDR_INVALID ||
        f->prev == TREENET_ADDR_BROADCAST) {
        return;
    }
    if (d.origin == t->addr) {
        return; /* our own DAO came back; ignore */
    }

    /* Install/refresh a downward route to the origin through the node the
     * frame actually arrived from (the previous hop). */
    tn_neighbor_t *via = tn_neighbor_find(&t->neighbors, f->prev);
    uint16_t cost = via ? via->link_cost : 0;
    tn_route_add(&t->routes, d.origin, f->prev, cost,
                 (uint8_t)(d.hops + 1u), now);

    /* The Master is the root: it stops here and uses the route table. */
    if (t->role == TREENET_ROLE_MASTER) {
        return;
    }
    if (t->parent == TREENET_ADDR_INVALID) {
        return;
    }
    /* Never send a DAO back the way it came. */
    if (f->prev == t->parent) {
        return;
    }

    uint8_t payload[TN_DAO_PAYLOAD_LEN];
    tn_dao_payload_t up;
    up.origin = d.origin;
    up.hops = (uint8_t)(d.hops + 1u);
    tn_dao_encode(payload, &up);

    tn_frame_t out;
    memset(&out, 0, sizeof(out));
    out.version = TREENET_PROTOCOL_VERSION;
    out.type = TREENET_FRAME_DAO;
    out.flags = TN_FLAG_WANT_ACK;
    out.src = d.origin;             /* origin is preserved end to end */
    out.dst = t->parent;
    out.seq = tn_next_seq(t);
    out.net_id = t->net_id;
    out.hop_limit = TREENET_MAX_HOPS;
    out.payload = payload;
    out.payload_len = TN_DAO_PAYLOAD_LEN;

    (void)tn_tx_submit(t, &out, true, t->parent, 0);
}

