/**
 * @file treenet.c
 * @brief Core of the treenity library: instance lifecycle, the cooperative
 *        poll loop, the receive path, the transmit scheduler and frame
 *        forwarding.
 *
 * Threading model
 * ---------------
 * treenet is single threaded from its own point of view. All state is mutated
 * inside treenet_poll(); the only function that may be called from an
 * interrupt is treenet_rx(), which merely copies the frame into a ring buffer
 * (guarded by the optional critical-section hooks of the port).
 */
#include "core/internal.h"

#include <string.h>

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

const char *treenet_version(void)
{
    return "0.1.0";
}

size_t treenet_context_size(void)
{
    return sizeof(treenet_t);
}

void tn_log(treenet_t *t, int level, const char *msg)
{
#if TREENET_ENABLE_LOG
    if (t != NULL && t->port.log != NULL) {
        t->port.log(level, msg);
    }
#else
    (void)t;
    (void)level;
    (void)msg;
#endif
}

uint16_t tn_next_seq(treenet_t *t)
{
    return t->seq++;
}

void tn_emit_event(treenet_t *t, treenet_event_t ev, void *arg)
{
    if (t->events.count >= TREENET_EVENT_QUEUE_SIZE) {
        /* Drop the oldest event to make room. */
        t->events.head = (uint8_t)((t->events.head + 1u) % TREENET_EVENT_QUEUE_SIZE);
        t->events.count--;
    }
    t->events.ev[t->events.tail] = ev;
    t->events.arg[t->events.tail] = arg;
    t->events.tail = (uint8_t)((t->events.tail + 1u) % TREENET_EVENT_QUEUE_SIZE);
    t->events.count++;
}

/* ========================================================================= */
/* Transmit path                                                              */
/* ========================================================================= */

int tn_tx_submit(treenet_t *t, const tn_frame_t *frame, bool reliable,
                 treenet_addr_t next_hop, uint32_t delay_ms)
{
    tn_tx_slot_t *s = NULL;
    for (size_t i = 0; i < TREENET_TX_QUEUE_SIZE; i++) {
        if (!t->tx[i].valid) {
            s = &t->tx[i];
            break;
        }
    }
    if (s == NULL) {
        t->stats.frames_dropped++;
        return -1;
    }

    /* The MAC stamps the previous-hop field with our own address. */
    tn_frame_t stamped = *frame;
    stamped.prev = t->addr;

    size_t len = tn_frame_encode(s->buf, sizeof(s->buf), &stamped);
    if (len == 0) {
        return -2;
    }

    s->valid = true;
    s->len = (uint16_t)len;
    s->reliable = reliable;
    s->attempts = 0;
    s->ack_dst = reliable ? next_hop : TREENET_ADDR_INVALID;
    s->ack_seq = stamped.seq;

    /* CSMA/CA: a random backoff from the contention window plus any caller
     * supplied contention delay (used by managed flooding). */
    uint32_t backoff = tn_rand_below(t->port.rnd(), TREENET_CW_MAX) * TREENET_SLOT_MS;
    s->next_tx_ms = t->now_ms + delay_ms + backoff;
    return 0;
}

/** Send a hop-by-hop acknowledgement for a received frame. */
static void tn_send_ack(treenet_t *t, treenet_addr_t to, uint16_t seq)
{
    if (to == TREENET_ADDR_INVALID || to == TREENET_ADDR_BROADCAST) {
        return;
    }
    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_ACK;
    f.src = t->addr;
    f.dst = to;
    f.seq = seq; /* the ACK reuses seq to carry the acknowledged sequence */
    f.net_id = t->net_id;
    f.hop_limit = 1;
    (void)tn_tx_submit(t, &f, false, to, 0);
}

/**
 * @brief Update a link's ACK-based failure state.
 *
 * @param dst          the intended next hop of the frame
 * @param acked_by_dst true when @p dst itself acknowledged (link healthy)
 *
 * A next hop that never acknowledges (even if other neighbours opportunistically
 * forward and acknowledge the frame) accumulates failures; after
 * TREENET_ACK_FAIL_THRESHOLD it is marked "suspect" and excluded from parent
 * selection, and if it is the parent the node re-selects immediately. This is
 * the fast failure detector that complements the beacon timeout.
 */
static void tn_link_ack_result(treenet_t *t, treenet_addr_t dst,
                               bool acked_by_dst)
{
    tn_neighbor_t *n = tn_neighbor_find(&t->neighbors, dst);
    if (n == NULL) return;

    if (acked_by_dst) {
        n->ack_fail = 0;
        n->suspect_until_ms = 0;
        return;
    }
    if (n->ack_fail < 0xFFu) n->ack_fail++;
    if (n->ack_fail >= TREENET_ACK_FAIL_THRESHOLD) {
        n->suspect_until_ms = t->now_ms + TREENET_LINK_SUSPECT_MS;
        n->ack_fail = 0;
        if (n->addr == t->parent) {
            /* The parent is now excluded: re-select at once. */
            (void)tn_routing_select_parent(t, t->now_ms);
        }
    }
}

/**
 * @brief Match an inbound ACK against a pending reliable frame.
 *
 * Frames are physically broadcast, so a neighbour other than the intended next
 * hop may accept (and forward) the frame and answer with an ACK. Accepting any
 * known neighbour's ACK with the matching sequence number recognises such
 * opportunistic forwarding instead of retransmitting into a dead next hop. The
 * intended next hop still has to answer for the link to be considered healthy.
 */
static void tn_handle_ack(treenet_t *t, const tn_frame_t *f)
{
    tn_neighbor_t *from = tn_neighbor_find(&t->neighbors, f->src);
    if (from == NULL) {
        return; /* ignore ACKs from unknown nodes */
    }
    for (size_t i = 0; i < TREENET_TX_QUEUE_SIZE; i++) {
        tn_tx_slot_t *s = &t->tx[i];
        if (s->valid && s->reliable && s->ack_seq == f->seq) {
            treenet_addr_t dst = s->ack_dst;
            s->valid = false;
            tn_link_ack_result(t, dst, f->src == dst);
            tn_emit_event(t, TREENET_EV_TX_DONE, NULL);
            return;
        }
    }
}

/** Drain the transmit slots: channel access, (re)transmission, retry logic. */
static void tn_tx_poll(treenet_t *t)
{
    for (size_t i = 0; i < TREENET_TX_QUEUE_SIZE; i++) {
        tn_tx_slot_t *s = &t->tx[i];
        if (!s->valid) continue;
        if (tn_time_after(s->next_tx_ms, t->now_ms)) continue;

        if (s->reliable && s->attempts >= TREENET_MAX_RETRIES) {
            treenet_addr_t dst = s->ack_dst;
            s->valid = false;
            tn_link_ack_result(t, dst, false);
            tn_emit_event(t, TREENET_EV_TX_FAILED, NULL);
            continue;
        }

        /* Carrier sense. If a port exposes CAD and the channel is busy, wait
         * one slot and try again later. */
        if (t->port.channel_free != NULL && !t->port.channel_free()) {
            s->next_tx_ms = t->now_ms + TREENET_SLOT_MS;
            continue;
        }

        bool was_retry = s->attempts > 0;
        int rc = t->port.tx(s->buf, s->len);
        if (rc == 0) {
            t->stats.frames_tx++;
            uint32_t us = tn_airtime_estimate_us(t->has_lora ? &t->lora : NULL,
                                                 s->len);
            t->stats.airtime_ms += (us + 999u) / 1000u;
            if (was_retry) t->stats.retransmissions++;
        }

        if (s->reliable) {
            s->attempts++;
            s->next_tx_ms = t->now_ms + TREENET_ACK_TIMEOUT_MS;
        } else {
            s->valid = false;
        }
    }
}

/* ========================================================================= */
/* Routing helper used by the data plane                                      */
/* ========================================================================= */

/**
 * @brief Resolve the next hop towards @p dst.
 *
 * Preference order:
 *   1. the destination itself if it is a direct neighbour (one hop, no loop
 *      because the destination delivers rather than forwards);
 *   2. an explicit downward route learned via DAO;
 *   3. the parent (upward), which reaches any ancestor including the Master.
 */
static treenet_addr_t tn_route_next_hop(treenet_t *t, treenet_addr_t dst)
{
    if (dst == t->addr) return t->addr;
    if (tn_neighbor_find(&t->neighbors, dst) != NULL) return dst;

    tn_route_t *r = tn_route_lookup(&t->routes, dst, t->now_ms,
                                    TREENET_ROUTE_TIMEOUT_MS);
    if (r != NULL) return r->next_hop;

    if (t->parent != TREENET_ADDR_INVALID) return t->parent;
    return TREENET_ADDR_INVALID;
}

/* ========================================================================= */
/* Reassembly (fragmentation)                                                 */
/* ========================================================================= */

#if TREENET_ENABLE_FRAGMENTATION
/** Maximum application bytes carried by a single fragment. */
#define TN_FRAG_CHUNK (TREENET_MTU - TN_FRAME_OVERHEAD - TN_FRAG_HDR_LEN)

static tn_reasm_t *reasm_slot(treenet_t *t, treenet_addr_t src,
                              uint16_t dgram_id, uint8_t count, uint32_t now)
{
    tn_reasm_t *free_slot = NULL;
    for (size_t i = 0; i < TREENET_REASSEMBLY_SLOTS; i++) {
        tn_reasm_t *r = &t->reasm[i];
        if (r->valid && r->src == src && r->dgram_id == dgram_id) {
            return r;
        }
        if (!r->valid && free_slot == NULL) {
            free_slot = r;
        }
    }
    if (free_slot == NULL) {
        /* Recycle the oldest slot. */
        uint32_t max_age = 0;
        for (size_t i = 0; i < TREENET_REASSEMBLY_SLOTS; i++) {
            uint32_t age = tn_elapsed(t->reasm[i].last_ms, now);
            if (age >= max_age) {
                max_age = age;
                free_slot = &t->reasm[i];
            }
        }
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->valid = true;
    free_slot->src = src;
    free_slot->dgram_id = dgram_id;
    free_slot->count = count;
    free_slot->last_ms = now;
    return free_slot;
}

/** Feed one fragment; deliver the datagram when it is complete. */
static void tn_reasm_input(treenet_t *t, const tn_frame_t *f, int16_t rssi,
                           int8_t snr, uint32_t now)
{
    tn_frag_hdr_t fh;
    if (!tn_frag_decode(f->payload, f->payload_len, &fh)) {
        return;
    }
    if (fh.count == 0 || fh.count > TREENET_MAX_FRAGMENTS ||
        fh.index >= fh.count) {
        return;
    }

    size_t chunk_len = f->payload_len - TN_FRAG_HDR_LEN;

    /* Every fragment except the last must be exactly TN_FRAG_CHUNK bytes,
     * otherwise the reassembled datagram would contain a hole. */
    if ((size_t)fh.index + 1u < (size_t)fh.count &&
        chunk_len != TN_FRAG_CHUNK) {
        return;
    }

    /* All fragments of one datagram must agree on the total count; a mismatch
     * means a corrupted or forged fragment, so drop it without disturbing the
     * in-flight reassembly. */
    for (size_t i = 0; i < TREENET_REASSEMBLY_SLOTS; i++) {
        const tn_reasm_t *r = &t->reasm[i];
        if (r->valid && r->src == f->src && r->dgram_id == fh.dgram_id &&
            r->count != fh.count) {
            return;
        }
    }

    size_t offset = (size_t)fh.index * TN_FRAG_CHUNK;
    if (offset + chunk_len > TREENET_MAX_DATAGRAM) {
        return;
    }

    tn_reasm_t *r = reasm_slot(t, f->src, fh.dgram_id, fh.count, now);
    r->last_ms = now;

    uint16_t bit = (uint16_t)(1u << fh.index);
    if ((r->got_mask & bit) == 0) {
        memcpy(&r->buf[offset], f->payload + TN_FRAG_HDR_LEN, chunk_len);
        r->frag_len[fh.index] = (uint16_t)chunk_len;
        r->got_mask |= bit;
        uint8_t received = 0;
        for (uint8_t i = 0; i < fh.count; i++) {
            if (r->got_mask & (uint16_t)(1u << i)) received++;
        }
        if (received == fh.count) {
            /* Compute total length from the fragment lengths. */
            uint16_t total = 0;
            for (uint8_t i = 0; i < fh.count; i++) {
                total = (uint16_t)(total + r->frag_len[i]);
            }
            t->stats.datagrams_rx++;
            if (t->cfg.on_recv != NULL) {
                uint8_t hops = (uint8_t)(TREENET_MAX_HOPS - f->hop_limit);
                t->cfg.on_recv(t, f->src, r->buf, total, rssi, snr, hops);
            }
            r->valid = false;
        }
    }
}
#endif /* TREENET_ENABLE_FRAGMENTATION */

/* ========================================================================= */
/* Frame processing                                                           */
/* ========================================================================= */

/** Deliver a fully received datagram to the application. */
static void tn_deliver(treenet_t *t, const tn_frame_t *f, int16_t rssi,
                       int8_t snr)
{
#if TREENET_ENABLE_FRAGMENTATION
    if (f->flags & TN_FLAG_FRAG) {
        tn_reasm_input(t, f, rssi, snr, t->now_ms);
        return;
    }
#endif
    t->stats.datagrams_rx++;
    if (t->cfg.on_recv != NULL) {
        uint8_t hops = (uint8_t)(TREENET_MAX_HOPS - f->hop_limit);
        t->cfg.on_recv(t, f->src, f->payload, f->payload_len, rssi, snr, hops);
    }
}

/** Handle a unicast DATA frame: deliver locally or forward towards the dst. */
static void tn_handle_data(treenet_t *t, const tn_frame_t *f, int16_t rssi,
                           int8_t snr, uint32_t now)
{
    bool want_ack = (f->flags & TN_FLAG_WANT_ACK) != 0;

    if (f->dst == t->addr) {
        if (want_ack) tn_send_ack(t, f->prev, f->seq);
        tn_deliver(t, f, rssi, snr);
        return;
    }

    /* Leaves are not routers: never forward other nodes' traffic. */
    if (t->role == TREENET_ROLE_LEAF) {
        return;
    }

    /* Forwarding: suppress duplicates but always re-ACK the link. */
    if (tn_dupcache_seen(&t->dup, f->src, f->seq, now)) {
        if (want_ack) tn_send_ack(t, f->prev, f->seq);
        return;
    }
    if (f->hop_limit <= 1) {
        t->stats.frames_dropped++;
        return;
    }

    treenet_addr_t nh = tn_route_next_hop(t, f->dst);
    if (nh == TREENET_ADDR_INVALID) {
        t->stats.frames_dropped++;
        return;
    }

    if (want_ack) tn_send_ack(t, f->prev, f->seq);

    tn_frame_t out = *f;
    out.hop_limit = (uint8_t)(f->hop_limit - 1u);
    (void)tn_tx_submit(t, &out, want_ack, nh, 0);
}

/**
 * @brief Managed flooding of a broadcast frame.
 *
 * The rebroadcast is delayed by a contention window that shrinks as the SNR of
 * the received frame drops: a distant (weakly heard) node forwards quickly so
 * the message spreads outwards, while a close (strongly heard) node defers and
 * stays silent once it hears the rebroadcast. This is the managed-flooding
 * scheme popularised by other LoRa meshes and keeps a dense mesh from echoing
 * the same frame endlessly.
 */
static void tn_handle_flood(treenet_t *t, const tn_frame_t *f, int16_t rssi,
                            int8_t snr, uint32_t now)
{
    if (tn_dupcache_seen(&t->dup, f->src, f->seq, now)) {
        return;
    }

    tn_deliver(t, f, rssi, snr);

    /* Leaves deliver broadcasts but never rebroadcast them. */
    if (t->role == TREENET_ROLE_LEAF) {
        return;
    }

    if (f->hop_limit <= 1) {
        return;
    }

    uint32_t delay;
    if (t->role == TREENET_ROLE_REPEATER) {
        delay = 0; /* infrastructure repeaters jump the queue */
    } else {
        /* Map SNR [-20..+10] dB to a window [0..CW]: weaker links wait less. */
        int32_t s = tn_clamp_i32((int32_t)snr, -20, 10);
        uint32_t win = (uint32_t)((10 - s) * (int32_t)TREENET_FLOOD_CW_MS) / 30u;
        delay = tn_rand_below(t->port.rnd(), TREENET_SLOT_MS) + win;
    }

    tn_frame_t out = *f;
    out.hop_limit = (uint8_t)(f->hop_limit - 1u);
    (void)tn_tx_submit(t, &out, false, TREENET_ADDR_BROADCAST, delay);
}

void tn_process_frame(treenet_t *t, const tn_frame_t *f, int16_t rssi,
                      int8_t snr, uint32_t now)
{
    switch (f->type) {
    case TREENET_FRAME_BEACON: {
        tn_beacon_payload_t b;
        if (!tn_beacon_decode(f->payload, f->payload_len, &b)) {
            t->stats.frames_dropped++;
            break;
        }
        /* Semantic sanity. A corrupted beacon must never be able to attract
         * nodes to a bogus parent or poison the link-quality estimate, so
         * reject impossible intervals, self-parenting and inconsistent
         * rank/flag combinations. */
        uint32_t interval_ms = (uint32_t)b.interval_100ms * 100u;
        bool bad_interval = (interval_ms < (TREENET_BEACON_MIN_MS / 2u)) ||
                            (interval_ms > TREENET_BEACON_MAX_MS);
        bool bad_parent = (b.parent == f->src);
        bool bad_rank = ((b.flags & TN_BEACON_HAS_PARENT) != 0) &&
                        (b.rank >= TREENET_RANK_INFINITE);
        if (bad_interval || bad_parent || bad_rank) {
            t->stats.frames_dropped++;
            break;
        }
        t->stats.beacons_rx++;
        tn_routing_on_beacon(t, f->src, b.rank, b.parent, b.flags,
                             b.interval_100ms, rssi, snr, now);
        break;
    }
    case TREENET_FRAME_DAO:
        if (f->dst == t->addr || f->dst == TREENET_ADDR_BROADCAST) {
            if ((f->flags & TN_FLAG_WANT_ACK) != 0) {
                tn_send_ack(t, f->prev, f->seq);
            }
            tn_dao_handle(t, f, now);
        }
        break;
    case TREENET_FRAME_ACK:
        if (f->dst == t->addr) {
            tn_handle_ack(t, f);
        }
        break;
    case TREENET_FRAME_PROBE:
        /* A neighbour is looking for a parent: answer with a fresh beacon. */
        tn_beacon_send(t, now);
        break;
    case TREENET_FRAME_DATA:
        tn_handle_data(t, f, rssi, snr, now);
        break;
    case TREENET_FRAME_FLOOD:
        tn_handle_flood(t, f, rssi, snr, now);
        break;
    default:
        t->stats.frames_dropped++;
        break;
    }
}

/* ========================================================================= */
/* Receive path                                                               */
/* ========================================================================= */

static void tn_rx_drain(treenet_t *t)
{
    tn_rx_meta_t meta;
    uint8_t buf[TREENET_MTU];

    while (tn_ringbuf_pop(&t->rx, &meta, buf, sizeof(buf))) {
        t->stats.frames_rx++;

        tn_frame_t f;
        if (!tn_frame_decode(buf, meta.len, &f)) {
            t->stats.frames_dropped++;
            continue;
        }
        if (f.net_id != t->net_id || f.src == t->addr) {
            continue; /* other network, or our own frame echoed back */
        }

        /* Link quality is a property of the link to the *immediate*
         * transmitter, not of the original source: a relayed frame tells us
         * nothing about our link to the origin. Beacons are link-local so for
         * them prev == src. */
        treenet_addr_t hop = f.prev;
        if (hop != t->addr && hop != TREENET_ADDR_INVALID) {
            bool known = tn_neighbor_find(&t->neighbors, hop) != NULL;
            tn_neighbor_note_frame(&t->neighbors, hop, meta.rssi_dbm,
                                   meta.snr_db, t->now_ms);
            if (!known) {
                tn_emit_event(t, TREENET_EV_NEIGHBOR_ADDED, NULL);
            }
        }

        tn_process_frame(t, &f, meta.rssi_dbm, meta.snr_db, t->now_ms);
    }
}

/* ========================================================================= */
/* Housekeeping                                                               */
/* ========================================================================= */

static void tn_age_neighbors(treenet_t *t)
{
    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS; i++) {
        tn_neighbor_t *n = &t->neighbors.entries[i];
        if (!n->valid) continue;
        if (tn_elapsed(n->last_seen_ms, t->now_ms) <=
            TREENET_NEIGHBOR_TIMEOUT_MS) {
            continue;
        }
        treenet_addr_t gone = n->addr;
        tn_route_remove_via(&t->routes, gone);
        memset(n, 0, sizeof(*n));
        if (t->neighbors.count > 0) t->neighbors.count--;
        tn_emit_event(t, TREENET_EV_NEIGHBOR_REMOVED, NULL);
        if (gone == t->parent) {
            tn_routing_on_parent_lost(t, t->now_ms);
        }
    }
}

static void tn_expire_reasm(treenet_t *t)
{
#if TREENET_ENABLE_FRAGMENTATION
    for (size_t i = 0; i < TREENET_REASSEMBLY_SLOTS; i++) {
        tn_reasm_t *r = &t->reasm[i];
        if (r->valid &&
            tn_elapsed(r->last_ms, t->now_ms) > TREENET_REASM_TIMEOUT_MS) {
            r->valid = false;
        }
    }
#endif
}

static void tn_probe_send(treenet_t *t)
{
    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_PROBE;
    f.src = t->addr;
    f.dst = TREENET_ADDR_BROADCAST;
    f.seq = tn_next_seq(t);
    f.net_id = t->net_id;
    f.hop_limit = 1;
    (void)tn_tx_submit(t, &f, false, TREENET_ADDR_BROADCAST, 0);
    /* Rate-limit probes on the attempt, not on success, so a momentarily full
     * transmit queue cannot pin the next deadline at zero. */
    t->probe_last_ms = t->now_ms;
}

static void tn_events_flush(treenet_t *t)
{
    if (t->cfg.on_event == NULL) {
        t->events.head = 0;
        t->events.tail = 0;
        t->events.count = 0;
        return;
    }
    while (t->events.count > 0) {
        treenet_event_t ev = t->events.ev[t->events.head];
        void *arg = t->events.arg[t->events.head];
        t->events.head = (uint8_t)((t->events.head + 1u) % TREENET_EVENT_QUEUE_SIZE);
        t->events.count--;
        t->cfg.on_event(t, ev, arg);
    }
}

/* ========================================================================= */
/* Lifecycle                                                                  */
/* ========================================================================= */

treenet_t *treenet_init(void *storage, size_t storage_size,
                        const treenet_config_t *cfg, const treenet_port_t *port)
{
    if (storage == NULL || cfg == NULL || port == NULL) return NULL;
    if (storage_size < sizeof(treenet_t)) return NULL;
    if (port->tx == NULL || port->now_ms == NULL || port->rnd == NULL) return NULL;
    if (cfg->addr == TREENET_ADDR_INVALID ||
        cfg->addr == TREENET_ADDR_BROADCAST) {
        return NULL;
    }

    treenet_t *t = (treenet_t *)storage;
    memset(t, 0, sizeof(*t));

    t->cfg = *cfg;
    t->port = *port;
    t->addr = cfg->addr;
    t->role = cfg->role;
    t->net_id = cfg->net_id;
    t->seq = (uint16_t)port->rnd();

    t->parent = TREENET_ADDR_INVALID;
    t->rank = (cfg->role == TREENET_ROLE_MASTER) ? 0u : TREENET_RANK_INFINITE;
    t->connected = (cfg->role == TREENET_ROLE_MASTER);
    t->joined = (cfg->role == TREENET_ROLE_MASTER);
    t->trickle_I = TREENET_BEACON_MIN_MS;
    t->now_ms = port->now_ms();

    tn_neighbor_table_init(&t->neighbors);
    tn_route_init(&t->routes);
    tn_dupcache_init(&t->dup, TREENET_DUP_TTL_MS);
    tn_ringbuf_init(&t->rx, t->rx_storage, TREENET_RX_RING_BYTES);
    memset(t->tx, 0, sizeof(t->tx));
    memset(t->reasm, 0, sizeof(t->reasm));
    memset(&t->events, 0, sizeof(t->events));

    if (cfg->radio.spreading_factor != 0) {
        t->has_lora = true;
        t->lora.sf = cfg->radio.spreading_factor;
        t->lora.bw_hz = cfg->radio.bandwidth_hz ? cfg->radio.bandwidth_hz : 125000u;
        t->lora.cr = cfg->radio.coding_rate ? cfg->radio.coding_rate : 1u;
        t->lora.preamble = 16;
        t->lora.explicit_hdr = 1;
        t->lora.crc = 1;
    }

    /* First beacon after a random fraction of the minimum interval, so a
     * freshly powered group does not beacon in lockstep. */
    tn_timer_start(&t->beacon_timer, t->now_ms,
                   tn_rand_below(port->rnd(), TREENET_BEACON_MIN_MS));

    t->initialized = true;

    /* Give the Master an immediate presence; a node solicits its neighbours. */
    if (t->role == TREENET_ROLE_MASTER) {
        tn_routing_select_parent(t, t->now_ms);
    } else {
        tn_probe_send(t);
    }

    return t;
}

/* ========================================================================= */
/* Tickless deadline computation                                              */
/* ========================================================================= */

/** Remaining milliseconds until @p due, 0 if already due (wrap-safe). */
static uint32_t deadline_in(uint32_t due, uint32_t now)
{
    return tn_time_after(now, due) ? 0u : (uint32_t)(due - now);
}

/**
 * @brief Earliest maintenance deadline.
 *
 * Covers route expiry, neighbour ageing, reassembly cleanup, DAO refresh and
 * the parent-search PROBE. All periodic housekeeping is expressed this way
 * instead of a fixed tick, so the node can sleep until the next action is
 * actually due.
 *
 * @return milliseconds until the next maintenance action (0 if due now)
 */
static uint32_t next_maint_deadline(const treenet_t *t, uint32_t now)
{
    uint32_t best = UINT32_MAX;

    for (size_t i = 0; i < TREENET_MAX_ROUTES; i++) {
        const tn_route_t *r = &t->routes.entries[i];
        if (!r->valid) continue;
        uint32_t d = deadline_in(r->updated_ms + TREENET_ROUTE_TIMEOUT_MS, now);
        if (d < best) best = d;
    }
    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS; i++) {
        const tn_neighbor_t *n = &t->neighbors.entries[i];
        if (!n->valid) continue;
        uint32_t d = deadline_in(n->last_seen_ms + TREENET_NEIGHBOR_TIMEOUT_MS,
                                 now);
        if (d < best) best = d;
    }
    for (size_t i = 0; i < TREENET_REASSEMBLY_SLOTS; i++) {
        const tn_reasm_t *r = &t->reasm[i];
        if (!r->valid) continue;
        uint32_t d = deadline_in(r->last_ms + TREENET_REASM_TIMEOUT_MS, now);
        if (d < best) best = d;
    }

    if (t->role != TREENET_ROLE_MASTER) {
        if (!t->connected) {
            uint32_t d = deadline_in(t->probe_last_ms + TREENET_PROBE_INTERVAL_MS,
                                     now);
            if (d < best) best = d;
        } else {
            uint32_t d = deadline_in(t->dao_last_ms + TREENET_ROUTE_REFRESH_MS,
                                     now);
            if (d < best) best = d;
        }
    }
    return best;
}

/**
 * @brief Earliest deadline of any kind.
 *
 * Combines beacons, maintenance, queued transmissions (including ACK
 * retransmissions) and parent liveness. treenet_poll() arms the port's wake
 * timer with this value at the end of every poll.
 *
 * @return milliseconds until treenet_poll() next has work (0 if due now,
 *         UINT32_MAX if nothing is scheduled)
 */
static uint32_t next_deadline_ms(const treenet_t *t, uint32_t now)
{
    uint32_t best = tn_timer_remaining(&t->beacon_timer, now);

    uint32_t d = next_maint_deadline(t, now);
    if (d < best) best = d;

    for (size_t i = 0; i < TREENET_TX_QUEUE_SIZE; i++) {
        const tn_tx_slot_t *s = &t->tx[i];
        if (!s->valid) continue;
        d = deadline_in(s->next_tx_ms, now);
        if (d < best) best = d;
    }

    if (t->role != TREENET_ROLE_MASTER && t->parent != TREENET_ADDR_INVALID) {
        d = deadline_in(t->parent_beacon_ms + TREENET_PARENT_TIMEOUT_MS, now);
        if (d < best) best = d;
    }
    return best;
}

void treenet_poll(treenet_t *t)
{
    if (t == NULL || !t->initialized) return;

    t->now_ms = t->port.now_ms();

    /* 1. Move frames received since the last call into the protocol. */
    tn_rx_drain(t);

    /* 2. Timed control traffic. */
    tn_beacon_tick(t, t->now_ms);
    tn_routing_tick(t, t->now_ms);

    /* 3. Maintenance, only when something is actually due. Each task also
     *    self-checks its own timeout, so running the block early is harmless. */
    if (next_maint_deadline(t, t->now_ms) == 0u) {
        tn_route_expire(&t->routes, t->now_ms, TREENET_ROUTE_TIMEOUT_MS);
        tn_age_neighbors(t);
        tn_expire_reasm(t);

        if (t->role != TREENET_ROLE_MASTER) {
            if (!t->connected) {
                if (deadline_in(t->probe_last_ms + TREENET_PROBE_INTERVAL_MS,
                                t->now_ms) == 0u) {
                    tn_probe_send(t); /* keep soliciting until a parent is found */
                }
            } else if (deadline_in(t->dao_last_ms + TREENET_ROUTE_REFRESH_MS,
                                   t->now_ms) == 0u) {
                tn_dao_send(t, t->now_ms);
            }
        }
    }

    /* 4. Transmit what is due. */
    tn_tx_poll(t);

    /* 5. Deliver events last, so the application sees a settled state. */
    tn_events_flush(t);

    /* 6. Arm the port's wake timer for the next deadline (tickless). */
    if (t->port.timer_arm != NULL) {
        t->port.timer_arm(next_deadline_ms(t, t->now_ms));
    }
}

int treenet_rx(treenet_t *t, const uint8_t *buf, size_t len,
               int16_t rssi, int8_t snr)
{
    if (t == NULL || !t->initialized || buf == NULL) return -1;
    if (len == 0 || len > TREENET_MTU) return -1;

    tn_rx_meta_t meta;
    meta.len = (uint16_t)len;
    meta.rssi_dbm = rssi;
    meta.snr_db = snr;

    /* The receive ring is lock-free single-producer/single-consumer: this
     * function is the sole producer, so no critical section is needed. */
    return tn_ringbuf_push(&t->rx, &meta, buf) ? 0 : -1;
}

/* ========================================================================= */
/* Data plane                                                                 */
/* ========================================================================= */

static int tn_send_datagram(treenet_t *t, treenet_addr_t dst,
                            const void *data, size_t len, bool flood)
{
    if (data == NULL || len == 0 || len > TREENET_MAX_DATAGRAM) return -1;

    const uint8_t *p = (const uint8_t *)data;
    const size_t max_payload = TREENET_MTU - TN_FRAME_OVERHEAD;
    const bool reliable = (!flood && t->cfg.reliable);

    treenet_addr_t nh = TREENET_ADDR_BROADCAST;
    if (!flood) {
        nh = tn_route_next_hop(t, dst);
        if (nh == TREENET_ADDR_INVALID) {
            return -3; /* no path to the destination */
        }
    }

    /* --- Single frame ---------------------------------------------------- */
    if (len <= max_payload) {
        tn_frame_t f;
        memset(&f, 0, sizeof(f));
        f.version = TREENET_PROTOCOL_VERSION;
        f.type = flood ? TREENET_FRAME_FLOOD : TREENET_FRAME_DATA;
        f.flags = reliable ? TN_FLAG_WANT_ACK : 0u;
        f.src = t->addr;
        f.dst = flood ? TREENET_ADDR_BROADCAST : dst;
        f.seq = tn_next_seq(t);
        f.net_id = t->net_id;
        f.hop_limit = TREENET_MAX_HOPS;
        f.payload = p;
        f.payload_len = len;

        int rc = tn_tx_submit(t, &f, reliable, nh, 0);
        if (rc == 0) t->stats.datagrams_tx++;
        return rc;
    }

#if TREENET_ENABLE_FRAGMENTATION
    /* --- Fragmented ------------------------------------------------------ */
    const size_t chunk = TN_FRAG_CHUNK;
    size_t count = (len + chunk - 1u) / chunk;
    if (count > TREENET_MAX_FRAGMENTS) return -2;

    uint16_t dgram_id = tn_next_seq(t);
    for (size_t i = 0; i < count; i++) {
        uint8_t buf[TREENET_MTU];
        tn_frag_hdr_t fh;
        fh.dgram_id = dgram_id;
        fh.index = (uint8_t)i;
        fh.count = (uint8_t)count;
        tn_frag_encode(buf, &fh);

        size_t off = i * chunk;
        size_t cl = len - off;
        if (cl > chunk) cl = chunk;
        memcpy(buf + TN_FRAG_HDR_LEN, p + off, cl);

        tn_frame_t f;
        memset(&f, 0, sizeof(f));
        f.version = TREENET_PROTOCOL_VERSION;
        f.type = flood ? TREENET_FRAME_FLOOD : TREENET_FRAME_DATA;
        f.flags = TN_FLAG_FRAG | (reliable ? TN_FLAG_WANT_ACK : 0u);
        f.src = t->addr;
        f.dst = flood ? TREENET_ADDR_BROADCAST : dst;
        f.seq = tn_next_seq(t);
        f.net_id = t->net_id;
        f.hop_limit = TREENET_MAX_HOPS;
        f.payload = buf;
        f.payload_len = TN_FRAG_HDR_LEN + cl;

        (void)tn_tx_submit(t, &f, reliable, nh, 0);
    }
    t->stats.datagrams_tx++;
    return 0;
#else
    return -2; /* too large and fragmentation disabled */
#endif
}

int treenet_send(treenet_t *t, treenet_addr_t dst, const void *data, size_t len)
{
    if (t == NULL || !t->initialized) return -1;
    if (dst == TREENET_ADDR_BROADCAST) return treenet_broadcast(t, data, len);
    if (dst == t->addr || dst == TREENET_ADDR_INVALID) return -1;
    return tn_send_datagram(t, dst, data, len, false);
}

int treenet_broadcast(treenet_t *t, const void *data, size_t len)
{
    if (t == NULL || !t->initialized) return -1;
    return tn_send_datagram(t, TREENET_ADDR_BROADCAST, data, len, true);
}

/* ========================================================================= */
/* Introspection                                                              */
/* ========================================================================= */

treenet_addr_t treenet_addr(const treenet_t *t)
{
    return t ? t->addr : TREENET_ADDR_INVALID;
}

treenet_role_t treenet_role(const treenet_t *t)
{
    return t ? t->role : TREENET_ROLE_NODE;
}

treenet_addr_t treenet_parent(const treenet_t *t)
{
    return t ? t->parent : TREENET_ADDR_INVALID;
}

uint16_t treenet_rank(const treenet_t *t)
{
    return t ? t->rank : TREENET_RANK_INFINITE;
}

bool treenet_is_connected(const treenet_t *t)
{
    return t ? t->connected : false;
}

const treenet_stats_t *treenet_stats(const treenet_t *t)
{
    return t ? &t->stats : NULL;
}

size_t treenet_neighbors(const treenet_t *t, treenet_neighbor_info_t *out,
                         size_t max)
{
    if (t == NULL || out == NULL) return 0;
    size_t n = 0;
    for (size_t i = 0; i < TREENET_MAX_NEIGHBORS && n < max; i++) {
        const tn_neighbor_t *nb = &t->neighbors.entries[i];
        if (!nb->valid) continue;
        out[n].addr = nb->addr;
        out[n].rank = nb->rank;
        out[n].age_ms = tn_elapsed(nb->last_seen_ms, t->now_ms);
        out[n].is_parent = (nb->addr == t->parent);
        out[n].lq.rssi_dbm = (int16_t)tn_ewma16_get(&nb->rssi);
        out[n].lq.snr_db = (int8_t)tn_ewma16_get(&nb->snr);
        out[n].lq.pdr_q8 = nb->pdr.init ? (uint16_t)tn_ewma16_get(&nb->pdr) : 256u;
        out[n].lq.etx_q8 = nb->etx_q8;
        out[n].lq.link_cost = nb->link_cost;
        n++;
    }
    return n;
}
