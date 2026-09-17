/**
 * @file neighbor.c
 * @brief Neighbour table maintenance and link quality estimation.
 */
#include "neighbor.h"

#include <string.h>

#include "core/util.h"

void tn_neighbor_table_init(tn_neighbor_table_t *tbl)
{
    memset(tbl->entries, 0, sizeof(tbl->entries));
    tbl->count = 0;
}

tn_neighbor_t *tn_neighbor_find(tn_neighbor_table_t *tbl, treenet_addr_t addr)
{
    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS; i++) {
        if (tbl->entries[i].valid && tbl->entries[i].addr == addr) {
            return &tbl->entries[i];
        }
    }
    return NULL;
}

/* @return index of the entry that has been silent the longest. */
static size_t neighbor_oldest(const tn_neighbor_table_t *tbl, uint32_t now)
{
    size_t oldest = 0;
    uint32_t max_age = 0;
    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS; i++) {
        const tn_neighbor_t *n = &tbl->entries[i];
        if (!n->valid) {
            return i; /* free slot wins immediately */
        }
        uint32_t age = tn_elapsed(n->last_seen_ms, now);
        if (age >= max_age) {
            max_age = age;
            oldest = i;
        }
    }
    return oldest;
}

tn_neighbor_t *tn_neighbor_get_or_create(tn_neighbor_table_t *tbl,
                                         treenet_addr_t addr, uint32_t now)
{
    tn_neighbor_t *n = tn_neighbor_find(tbl, addr);
    if (n != NULL) {
        return n;
    }

    size_t idx = neighbor_oldest(tbl, now);
    n = &tbl->entries[idx];
    if (!n->valid) {
        tbl->count++;
    }
    memset(n, 0, sizeof(*n));
    n->valid = true;
    n->addr = addr;
    n->last_seen_ms = now;
    return n;
}

void tn_neighbor_update_cost(tn_neighbor_t *n)
{
    /* PDR: default to a perfect link until we have beacon samples. */
    int32_t pdr = n->pdr.init ? tn_ewma16_get(&n->pdr) : 256;
    if (pdr < 16) pdr = 16;   /* clamp to keep ETX finite */
    if (pdr > 256) pdr = 256;

    uint32_t etx = (256u * 256u) / (uint32_t)pdr;
    if (etx > 0xFFFFu) etx = 0xFFFFu;
    n->etx_q8 = (uint16_t)etx;

    /* The cost is dominated by the expected number of transmissions (ETX):
     * a perfect link (ETX 1.0) contributes nothing, ETX 4.0 saturates the
     * penalty. SNR adds a secondary term that reacts faster than PDR and
     * captures interference before it shows up as lost packets. */
    uint16_t snr_score = tn_snr_score_q8((int8_t)tn_ewma16_get(&n->snr));

    int32_t etx_pen = (int32_t)n->etx_q8 - 256;
    if (etx_pen < 0) etx_pen = 0;
    if (etx_pen > 768) etx_pen = 768; /* cap at ETX 4.0 */

    uint32_t cost = ((uint32_t)TREENET_OF_W_SNR * snr_score +
                     (uint32_t)TREENET_OF_W_ETX * (uint32_t)etx_pen) >> 8;
    if (cost > 1024u) cost = 1024u;
    n->link_cost = (uint16_t)cost;
}

void tn_neighbor_note_frame(tn_neighbor_table_t *tbl, treenet_addr_t addr,
                            int16_t rssi_dbm, int8_t snr_db, uint32_t now)
{
    tn_neighbor_t *n = tn_neighbor_get_or_create(tbl, addr, now);
    tn_ewma16_push(&n->rssi, rssi_dbm);
    tn_ewma16_push(&n->snr, snr_db);
    n->last_seen_ms = now;
    tn_neighbor_update_cost(n);
}

void tn_neighbor_note_beacon(tn_neighbor_table_t *tbl, treenet_addr_t addr,
                             uint16_t rank, treenet_addr_t parent,
                             uint8_t flags, uint16_t interval_100ms,
                             int16_t rssi_dbm, int8_t snr_db, uint32_t now)
{
    tn_neighbor_t *n = tn_neighbor_get_or_create(tbl, addr, now);

    /* --- PDR estimation ---------------------------------------------------
     * The sender advertises its beacon interval, so we know how many beacons
     * should have arrived between two receptions. Any period beyond the first
     * is counted as a lost sample; the reception itself is a success. Feeding
     * 0/256 samples into the EWMA yields a smooth, self-adapting delivery
     * ratio that is immune to the Trickle interval slowly growing. */
    uint32_t interval = (uint32_t)interval_100ms * 100u;
    if (interval == 0u) interval = 1u;

    if (n->last_beacon_ms != 0 && n->est_interval_ms != 0) {
        uint32_t elapsed = tn_elapsed(n->last_beacon_ms, now);
        uint32_t periods = elapsed / n->est_interval_ms;
        uint32_t missed = (periods > 0u) ? periods - 1u : 0u;
        if (missed > 8u) missed = 8u; /* bound the burst */
        for (uint32_t i = 0; i < missed; i++) {
            tn_ewma16_push(&n->pdr, 0);
        }
    }
    tn_ewma16_push(&n->pdr, 256);

    /* Remember the interval the sender just advertised. */
    n->est_interval_ms = interval;
    n->have_interval = true;

    n->rank = rank;
    n->parent = parent;
    n->flags = flags;
    n->last_beacon_ms = now;
    n->last_seen_ms = now;

    tn_ewma16_push(&n->rssi, rssi_dbm);
    tn_ewma16_push(&n->snr, snr_db);
    tn_neighbor_update_cost(n);
}

uint32_t tn_neighbor_age(tn_neighbor_table_t *tbl, uint32_t now,
                         uint32_t timeout_ms)
{
    uint32_t removed = 0;
    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS; i++) {
        tn_neighbor_t *n = &tbl->entries[i];
        if (!n->valid) continue;
        if (tn_elapsed(n->last_seen_ms, now) > timeout_ms) {
            memset(n, 0, sizeof(*n));
            if (tbl->count > 0) tbl->count--;
            removed++;
        }
    }
    return removed;
}
