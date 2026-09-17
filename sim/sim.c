/**
 * @file sim.c
 * @brief Deterministic shared-channel network simulator (see sim.h).
 */
#include "sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

/** Virtual time step of the simulator in milliseconds. */
#define SIM_STEP_MS 10u

/** SNR below which a frame cannot be demodulated. */
#define SIM_SNR_THRESHOLD_DB (-20.0)

/** One frame queued in the virtual channel for the current millisecond. */
typedef struct {
    treenet_addr_t sender;
    uint16_t       len;
    uint8_t        buf[TREENET_MTU];
} sim_txrec_t;

struct sim {
    uint32_t now_ms;
    uint32_t rnd;

    double tx_power_dbm;
    double sensitivity_dbm;
    double noise_floor_dbm;
    double ref_loss_db;
    double path_loss_exp;
    double range_m;
    double bit_error_rate;

    uint8_t  radio_sf;
    uint32_t radio_bw_hz;
    uint16_t net_id;

    size_t     count;
    sim_node_t nodes[SIM_MAX_NODES];

    sim_txrec_t channel[SIM_CHANNEL_CAPACITY];
    int         channel_count;

    /* Per-node reception scratch for collision resolution. */
    int     pending_rx[SIM_MAX_NODES];
    int     first_rec[SIM_MAX_NODES];
    int16_t first_rssi[SIM_MAX_NODES];
    int8_t  first_snr[SIM_MAX_NODES];

    sim_recv_cb  user_recv;
    sim_event_cb user_event;
};

/* The port callbacks carry no user context, so a single simulator is active at
 * a time and the node currently being polled is tracked globally. */
static sim_t *g_sim = NULL;
static int    g_current = -1;

/* ------------------------------------------------------------------------- */
/* PRNG                                                                       */
/* ------------------------------------------------------------------------- */

static uint32_t sim_rnd_raw(sim_t *s)
{
    uint32_t x = s->rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rnd = x ? x : 0x12345678u;
    return s->rnd;
}

static double sim_rnd_unit(sim_t *s)
{
    return (double)(sim_rnd_raw(s) >> 8) / (double)(1u << 24);
}

/* ------------------------------------------------------------------------- */
/* Port callbacks                                                             */
/* ------------------------------------------------------------------------- */

static int sim_tx(const uint8_t *buf, size_t len)
{
    sim_t *s = g_sim;
    if (s == NULL || g_current < 0) return -1;
    if (s->channel_count >= SIM_CHANNEL_CAPACITY) return -1;
    if (len > TREENET_MTU) return -1;

    sim_txrec_t *r = &s->channel[s->channel_count++];
    r->sender = s->nodes[g_current].addr;
    r->len = (uint16_t)len;
    memcpy(r->buf, buf, len);
    return 0;
}

static uint32_t sim_now_cb(void)
{
    return g_sim ? g_sim->now_ms : 0u;
}

static uint32_t sim_rnd_cb(void)
{
    return g_sim ? sim_rnd_raw(g_sim) : 0u;
}

static bool sim_channel_free_cb(void)
{
    return true; /* collisions are resolved by the channel, not by CAD */
}

static void sim_timer_arm_cb(uint32_t delay_ms)
{
    sim_t *s = g_sim;
    if (s == NULL || g_current < 0) return;
    sim_node_t *n = &s->nodes[g_current];
    n->last_timer_arm = delay_ms;
    if (delay_ms != UINT32_MAX && delay_ms > n->max_timer_arm) {
        n->max_timer_arm = delay_ms;
    }
    n->timer_arm_count++;
}

static void sim_on_recv(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                        size_t len, int16_t rssi, int8_t snr, uint8_t hops)
{
    (void)t;
    (void)data;
    (void)hops;
    if (g_sim == NULL) return;
    /* Identify the node that owns this instance. */
    for (size_t i = 0; i < g_sim->count; i++) {
        if (g_sim->nodes[i].net == t) {
            sim_node_t *n = &g_sim->nodes[i];
            n->datagrams_rx++;
            n->last_src = src;
            n->last_len = (uint32_t)len;
            n->last_rssi = rssi;
            n->last_snr = snr;
            if (g_sim->user_recv != NULL) {
                g_sim->user_recv(n, src, data, len, rssi, snr, hops);
            }
            return;
        }
    }
}

static void sim_on_event(treenet_t *t, treenet_event_t ev, void *arg)
{
    (void)ev;
    (void)arg;
    if (g_sim == NULL) return;
    for (size_t i = 0; i < g_sim->count; i++) {
        if (g_sim->nodes[i].net == t) {
            g_sim->nodes[i].events++;
            if (g_sim->user_event != NULL) {
                g_sim->user_event(&g_sim->nodes[i], ev);
            }
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Channel model                                                              */
/* ------------------------------------------------------------------------- */

/** Compute RSSI/SNR between two nodes. @return false if the link is unusable. */
static bool sim_link(sim_t *s, const sim_node_t *a, const sim_node_t *b,
                     int16_t *out_rssi, int8_t *out_snr)
{
    double dx = a->x - b->x;
    double dy = a->y - b->y;
    double d = sqrt(dx * dx + dy * dy);
    if (d < 1.0) d = 1.0;
    if (d > s->range_m) return false;

    /* Log-distance path loss. The model is deterministic apart from the
     * probabilistic loss below, which keeps scenarios reproducible. */
    double loss = s->ref_loss_db + 10.0 * s->path_loss_exp * log10(d);
    double rssi = s->tx_power_dbm - loss;
    double snr = rssi - s->noise_floor_dbm;

    if (rssi < s->sensitivity_dbm) return false;
    if (snr < SIM_SNR_THRESHOLD_DB) return false;

    /* Probabilistic loss: the closer to the sensitivity floor, the more
     * likely the frame is dropped. */
    double margin = rssi - s->sensitivity_dbm;
    double p_loss = pow(0.5, margin / 3.0);
    if (sim_rnd_unit(s) < p_loss) return false;

    *out_rssi = (int16_t)rssi;
    *out_snr = (int8_t)snr;
    return true;
}

/** Resolve the frames queued during the current step and deliver survivors. */
static void sim_deliver(sim_t *s)
{
    for (size_t i = 0; i < s->count; i++) {
        s->pending_rx[i] = 0;
        s->first_rec[i] = -1;
    }

    /* First pass: for every record, find which nodes can hear it. */
    for (int r = 0; r < s->channel_count; r++) {
        const sim_txrec_t *rec = &s->channel[r];
        for (size_t j = 0; j < s->count; j++) {
            sim_node_t *n = &s->nodes[j];
            if (!n->active || n->addr == rec->sender) continue;

            sim_node_t *txn = NULL;
            for (size_t k = 0; k < s->count; k++) {
                if (s->nodes[k].addr == rec->sender) {
                    txn = &s->nodes[k];
                    break;
                }
            }
            if (txn == NULL || !txn->active) continue;

            int16_t rssi = 0;
            int8_t snr = 0;
            if (!sim_link(s, txn, n, &rssi, &snr)) continue;

            if (s->pending_rx[j] == 0) {
                s->first_rec[j] = r;
                s->first_rssi[j] = rssi;
                s->first_snr[j] = snr;
            }
            s->pending_rx[j]++;
        }
    }

    /* Second pass: deliver only the frames that were heard exactly once.
     * Multiple simultaneous receptions are treated as a collision. */
    for (size_t j = 0; j < s->count; j++) {
        if (s->pending_rx[j] != 1) continue;
        const sim_txrec_t *rec = &s->channel[s->first_rec[j]];

        const uint8_t *data = rec->buf;
        size_t len = rec->len;

        /* Optional bit-error injection: model a noisy link by flipping bits
         * in a private copy before the frame reaches the library. */
        uint8_t corrupted[TREENET_MTU];
        if (s->bit_error_rate > 0.0) {
            memcpy(corrupted, rec->buf, len);
            for (size_t b = 0; b < len; b++) {
                for (int bit = 0; bit < 8; bit++) {
                    if (sim_rnd_unit(s) < s->bit_error_rate) {
                        corrupted[b] ^= (uint8_t)(1u << bit);
                    }
                }
            }
            data = corrupted;
        }

        (void)treenet_rx(s->nodes[j].net, data, len,
                         s->first_rssi[j], s->first_snr[j]);
    }

    s->channel_count = 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

sim_t *sim_create(uint32_t seed, double tx_power_dbm, double sensitivity_dbm,
                  double noise_floor_dbm)
{
    sim_t *s = (sim_t *)calloc(1, sizeof(sim_t));
    if (s == NULL) return NULL;

    s->now_ms = 0;
    s->rnd = seed ? seed : 1u;
    s->tx_power_dbm = tx_power_dbm;
    s->sensitivity_dbm = sensitivity_dbm;
    s->noise_floor_dbm = noise_floor_dbm;
    s->ref_loss_db = 40.0;   /* path loss at 1 m */
    s->path_loss_exp = 2.7;  /* suburban-ish exponent */
    s->range_m = 1.0e9;      /* unlimited unless overridden */
    s->bit_error_rate = 0.0; /* clean channel by default */
    s->radio_sf = 9;
    s->radio_bw_hz = 125000u;
    s->net_id = 1u;

    g_sim = s;
    return s;
}

void sim_destroy(sim_t *s)
{
    if (s == NULL) return;
    for (size_t i = 0; i < s->count; i++) {
        free(s->nodes[i].net); /* context was malloc'ed by sim_add_node */
    }
    if (g_sim == s) {
        g_sim = NULL;
        g_current = -1;
    }
    free(s);
}

sim_node_t *sim_add_node(sim_t *s, treenet_addr_t addr, treenet_role_t role,
                         double x, double y, bool reliable)
{
    if (s == NULL || s->count >= SIM_MAX_NODES) return NULL;
    if (addr == TREENET_ADDR_INVALID || addr == TREENET_ADDR_BROADCAST) {
        return NULL;
    }

    sim_node_t *n = &s->nodes[s->count];
    memset(n, 0, sizeof(*n));
    n->addr = addr;
    n->role = role;
    n->x = x;
    n->y = y;
    n->active = true;

    void *ctx = malloc(treenet_context_size());
    if (ctx == NULL) return NULL;

    treenet_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.addr = addr;
    cfg.role = role;
    cfg.net_id = s->net_id;
    cfg.reliable = reliable;
    cfg.on_recv = sim_on_recv;
    cfg.on_event = sim_on_event;
    cfg.user = n;
    cfg.radio.spreading_factor = s->radio_sf;
    cfg.radio.bandwidth_hz = s->radio_bw_hz;
    cfg.radio.coding_rate = 1;
    cfg.radio.tx_power_dbm = (int8_t)s->tx_power_dbm;

    treenet_port_t port;
    memset(&port, 0, sizeof(port));
    port.tx = sim_tx;
    port.now_ms = sim_now_cb;
    port.rnd = sim_rnd_cb;
    port.channel_free = sim_channel_free_cb;
    port.timer_arm = sim_timer_arm_cb;

    n->net = treenet_init(ctx, treenet_context_size(), &cfg, &port);
    if (n->net == NULL) {
        free(ctx);
        return NULL;
    }

    s->count++;
    return n;
}

sim_node_t *sim_find(sim_t *s, treenet_addr_t addr)
{
    if (s == NULL) return NULL;
    for (size_t i = 0; i < s->count; i++) {
        if (s->nodes[i].addr == addr) return &s->nodes[i];
    }
    return NULL;
}

void sim_move_node(sim_t *s, treenet_addr_t addr, double x, double y)
{
    sim_node_t *n = sim_find(s, addr);
    if (n == NULL) return;
    n->x = x;
    n->y = y;
}

void sim_set_active(sim_t *s, treenet_addr_t addr, bool active)
{
    sim_node_t *n = sim_find(s, addr);
    if (n != NULL) n->active = active;
}

size_t sim_node_count(const sim_t *s)
{
    return s ? s->count : 0;
}

sim_node_t *sim_node_at(const sim_t *s, size_t i)
{
    if (s == NULL || i >= s->count) return NULL;
    return (sim_node_t *)&s->nodes[i];
}

uint32_t sim_now(const sim_t *s)
{
    return s ? s->now_ms : 0u;
}

void sim_set_path_loss(sim_t *s, double ref_loss_db, double exponent)
{
    if (s == NULL) return;
    s->ref_loss_db = ref_loss_db;
    s->path_loss_exp = exponent;
}

void sim_set_range(sim_t *s, double range_m)
{
    if (s == NULL) return;
    s->range_m = (range_m > 0.0) ? range_m : 1.0e9;
}

void sim_set_bit_error_rate(sim_t *s, double ber)
{
    if (s == NULL) return;
    s->bit_error_rate = (ber > 0.0) ? ber : 0.0;
}

void sim_set_radio(sim_t *s, uint8_t sf, uint32_t bw_hz)
{
    if (s == NULL) return;
    s->radio_sf = sf ? sf : 9;
    s->radio_bw_hz = bw_hz ? bw_hz : 125000u;
}

void sim_set_net_id(sim_t *s, uint16_t net_id)
{
    if (s == NULL) return;
    s->net_id = net_id;
}

void sim_set_callbacks(sim_t *s, sim_recv_cb recv, sim_event_cb event)
{
    if (s == NULL) return;
    s->user_recv = recv;
    s->user_event = event;
}

void sim_run(sim_t *s, uint32_t ms)
{
    if (s == NULL) return;
    uint32_t steps = (ms + SIM_STEP_MS - 1u) / SIM_STEP_MS;
    for (uint32_t k = 0; k < steps; k++) {
        for (size_t i = 0; i < s->count; i++) {
            if (!s->nodes[i].active) continue;
            g_current = (int)i;
            treenet_poll(s->nodes[i].net);
        }
        g_current = -1;
        sim_deliver(s);
        s->now_ms += SIM_STEP_MS;
    }
}
