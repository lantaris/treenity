/**
 * @file beacon.c
 * @brief Link-local beaconing driven by a Trickle-style timer.
 *
 * Beacons are the only control traffic that is sent unconditionally. They are
 * broadcast one hop and never forwarded. Their purpose is twofold:
 *   - let neighbours measure the link (RSSI/SNR/PDR);
 *   - advertise this node's rank and parent so the DODAG can form.
 *
 * To keep the channel quiet in large, stable networks the interval adapts like
 * RFC 6206 (Trickle): it starts at TREENET_BEACON_MIN_MS and doubles after
 * every transmission up to TREENET_BEACON_MAX_MS. A topology change resets it
 * to the minimum so the network can reconverge quickly.
 */
#include "core/internal.h"

#include <string.h>

/* Schedule the next Trickle transmission in the second half of the interval,
 * as prescribed by Trickle, to desynchronise neighbouring nodes. */
static void beacon_schedule(treenet_t *t, uint32_t now)
{
    uint32_t half = t->trickle_I / 2u;
    if (half == 0u) half = 1u;
    uint32_t jitter = tn_rand_below(t->port.rnd(), half);
    tn_timer_start(&t->beacon_timer, now, half + jitter);
}

void tn_beacon_reset(treenet_t *t, uint32_t now)
{
    t->trickle_I = TREENET_BEACON_MIN_MS;
    beacon_schedule(t, now);
}

void tn_beacon_send(treenet_t *t, uint32_t now)
{
    uint8_t payload[TN_BEACON_PAYLOAD_LEN];
    tn_beacon_payload_t b;
    b.rank = t->rank;
    b.parent = t->parent;
    b.flags = t->connected ? TN_BEACON_HAS_PARENT : 0u;
    /* Advertise the current Trickle interval (in 100 ms units) so receivers
     * can compute an accurate delivery ratio. */
    uint32_t interval_100 = t->trickle_I / 100u;
    if (interval_100 > 0xFFFFu) interval_100 = 0xFFFFu;
    b.interval_100ms = (uint16_t)interval_100;
    tn_beacon_encode(payload, &b);

    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_BEACON;
    f.flags = 0;
    f.src = t->addr;
    f.dst = TREENET_ADDR_BROADCAST;
    f.seq = tn_next_seq(t);
    f.net_id = t->net_id;
    f.hop_limit = 1; /* link-local only */
    f.payload = payload;
    f.payload_len = TN_BEACON_PAYLOAD_LEN;

    if (tn_tx_submit(t, &f, false, TREENET_ADDR_BROADCAST, 0) == 0) {
        t->stats.beacons_tx++;
    }

    /* Grow the interval, capped at the maximum. */
    uint32_t next = t->trickle_I * 2u;
    if (next > TREENET_BEACON_MAX_MS) next = TREENET_BEACON_MAX_MS;
    t->trickle_I = next;
    beacon_schedule(t, now);
}

void tn_beacon_tick(treenet_t *t, uint32_t now)
{
    if (tn_timer_fire(&t->beacon_timer, now)) {
        tn_beacon_send(t, now);
    }
}
