/**
 * @file test_repeaters.c
 * @brief Stress / report test: one Master and 30 REPEATER nodes in a redundant
 *        grid, exercising two failure scenarios and reporting the measured
 *        reconfiguration timings.
 *
 *   Scenario 1 (180 s): corrupted / lost frames (bit-error injection).
 *                       Per-node parent-change churn.
 *   Scenario 2 (180 s): transit repeaters lose power (radio off).
 *                       Per-event reconnection timings of their children.
 *
 * The report is intentionally concise: one line per node for scenario 1, one
 * block per power-off event for scenario 2.
 *
 * Build & run (from the treenity directory):
 *   cmake -S . -B build && cmake --build build && ./build/treenet_repeaters
 *   # or
 *   make repeaters
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "treenet/treenet.h"

/* Master + 30 repeaters = 31 nodes (SIM_MAX_NODES is 32). */
#define N_NODES 31

/* ------------------------------------------------------------------------- */
/* Instrumentation                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t changes;        /**< parent changes */
    uint32_t last_change_ms; /**< timestamp of the previous parent change */
    uint32_t sum_interval_ms;
    uint32_t n_intervals;
    uint32_t min_interval_ms;
    uint32_t max_interval_ms;

    uint32_t disconnected_ms;    /**< time of the last DISCONNECTED (0 = up) */
    uint32_t disconnected_count; /**< number of disconnection episodes */
    uint32_t route_lost_count;   /**< no parent at all */
} node_stat_t;

static sim_t *G;
static node_stat_t S[N_NODES];

static size_t idx(treenet_addr_t addr) { return (size_t)(addr - 1u); }

static void stats_reset(void)
{
    memset(S, 0, sizeof(S));
}

static void on_event(sim_node_t *node, treenet_event_t ev)
{
    if (node->addr == 0 || node->addr > N_NODES) return;
    uint32_t t = sim_now(G);
    size_t i = idx(node->addr);

    switch (ev) {
    case TREENET_EV_PARENT_CHANGED:
        S[i].changes++;
        if (S[i].last_change_ms != 0) {
            uint32_t d = t - S[i].last_change_ms;
            S[i].sum_interval_ms += d;
            S[i].n_intervals++;
            if (S[i].min_interval_ms == 0 || d < S[i].min_interval_ms) {
                S[i].min_interval_ms = d;
            }
            if (d > S[i].max_interval_ms) S[i].max_interval_ms = d;
        }
        S[i].last_change_ms = t;
        if (S[i].disconnected_ms != 0) S[i].disconnected_ms = 0;
        break;
    case TREENET_EV_NETWORK_READY:
        if (S[i].disconnected_ms != 0) S[i].disconnected_ms = 0;
        break;
    case TREENET_EV_DISCONNECTED:
        S[i].disconnected_ms = t;
        S[i].disconnected_count++;
        break;
    case TREENET_EV_ROUTE_LOST:
        S[i].route_lost_count++;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Topology                                                                   */
/* ------------------------------------------------------------------------- */

static const char *role_name(treenet_role_t r)
{
    switch (r) {
    case TREENET_ROLE_MASTER:   return "master";
    case TREENET_ROLE_REPEATER: return "repeater";
    case TREENET_ROLE_LEAF:     return "leaf";
    default:                    return "node";
    }
}

/* Master at (0,0); 30 repeaters in a 6x5 grid, 150 m spacing, 250 m range. */
static sim_t *build_net(void)
{
    sim_t *s = sim_create(0x1234u, 20.0, -137.0, -120.0);
    sim_set_path_loss(s, 40.0, 3.0);
    sim_set_range(s, 250.0);
    sim_set_radio(s, 7, 125000); /* SF7 keeps 31 nodes within the airtime budget */
    sim_set_callbacks(s, NULL, on_event);
    G = s;

    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);
    treenet_addr_t addr = 2;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 6; col++) {
            sim_add_node(s, addr++, TREENET_ROLE_REPEATER,
                         150.0 * (col + 1), 150.0 * row, true);
        }
    }
    return s;
}

static size_t children_of(sim_t *s, treenet_addr_t x, treenet_addr_t *out,
                          size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < N_NODES; i++) {
        sim_node_t *node = sim_node_at(s, i);
        if (node->addr != x && treenet_parent(node->net) == x) {
            if (out != NULL && n < max) out[n] = node->addr;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Scenario 1: corrupted / lost frames                                        */
/* ------------------------------------------------------------------------- */

static void scenario1(sim_t *s)
{
    printf("\n=== Scenario 1: corrupted/lost frames, 180 s (BER=0.002) ===\n");
    printf("node role      rank parent chg  change_interval avg/max ms  disc lost\n");

    stats_reset();
    sim_set_bit_error_rate(s, 0.002);
    sim_run(s, 180000);
    sim_set_bit_error_rate(s, 0.0);

    uint32_t tot_changes = 0, tot_disc = 0, tot_lost = 0;

    for (size_t i = 0; i < N_NODES; i++) {
        sim_node_t *n = sim_node_at(s, i);
        const node_stat_t *st = &S[i];
        uint32_t int_avg = st->n_intervals ? st->sum_interval_ms / st->n_intervals : 0;

        printf("%4u %-9s %4u %5u %4u  ", n->addr,
               role_name(treenet_role(n->net)), treenet_rank(n->net),
               treenet_parent(n->net), st->changes);
        if (st->n_intervals) {
            printf("%7u/%-8u", int_avg, st->max_interval_ms);
        } else {
            printf("%7s %-8s", "-", "-");
        }
        printf(" %4u %4u\n", st->disconnected_count, st->route_lost_count);

        tot_changes += st->changes;
        tot_disc += st->disconnected_count;
        tot_lost += st->route_lost_count;
    }

    printf("summary: parent_changes=%u  disconnects=%u  route_lost=%u "
           "(reparent after detection is immediate)\n",
           tot_changes, tot_disc, tot_lost);
}

/* ------------------------------------------------------------------------- */
/* Scenario 2: transit repeaters lose power                                   */
/* ------------------------------------------------------------------------- */

/* Active repeater with the most children, or 0 if none has any. */
static treenet_addr_t pick_target(sim_t *s)
{
    treenet_addr_t best = 0;
    size_t best_children = 0;
    for (size_t i = 0; i < N_NODES; i++) {
        sim_node_t *n = sim_node_at(s, i);
        if (n->addr == 1 || !n->active) continue;
        size_t c = children_of(s, n->addr, NULL, 0);
        if (c > best_children) {
            best_children = c;
            best = n->addr;
        }
    }
    return (best_children >= 1) ? best : 0;
}

static void scenario2(sim_t *s)
{
    printf("\n=== Scenario 2: transit repeater power-off, 180 s ===\n");
    printf("(reconnect = time until a child moves off the failed parent)\n");

    stats_reset();

    uint32_t all_sum = 0, all_n = 0, all_max = 0;
    uint32_t total_affected = 0, total_unrec = 0, events = 0;

    for (int k = 0; k < 4; k++) {
        treenet_addr_t x = pick_target(s);
        if (x == 0) {
            printf("\n(no transit repeater with children left)\n");
            break;
        }

        uint32_t T = sim_now(s);
        treenet_addr_t children[N_NODES];
        size_t nc = children_of(s, x, children, N_NODES);

        uint32_t reconn[N_NODES];
        treenet_addr_t newp[N_NODES];
        bool done[N_NODES];
        for (size_t c = 0; c < nc; c++) {
            reconn[c] = 0;
            newp[c] = TREENET_ADDR_INVALID;
            done[c] = false;
        }

        sim_set_active(s, x, false);
        events++;

        /* Poll every 100 ms until every child has moved to another parent. */
        uint32_t elapsed = 0;
        while (elapsed < 45000u) {
            sim_run(s, 100);
            elapsed += 100;
            bool all_done = true;
            for (size_t c = 0; c < nc; c++) {
                if (done[c]) continue;
                sim_node_t *cn = sim_find(s, children[c]);
                treenet_addr_t p = treenet_parent(cn->net);
                if (p != x) {
                    reconn[c] = elapsed;
                    newp[c] = p;
                    done[c] = true;
                } else {
                    all_done = false;
                }
            }
            if (all_done) break;
        }

        printf("\nt=+%us  off node %u  (direct children: %u)\n",
               T / 1000u, x, (unsigned)nc);

        uint32_t ev_sum = 0, ev_n = 0, ev_min = 0, ev_max = 0, unrec = 0;
        for (size_t c = 0; c < nc; c++) {
            if (done[c]) {
                uint32_t d = reconn[c];
                ev_sum += d;
                ev_n++;
                if (ev_min == 0 || d < ev_min) ev_min = d;
                if (d > ev_max) ev_max = d;
                printf("    node %-3u -> %5u ms  (new parent %u)\n",
                       children[c], d, newp[c]);
            } else {
                unrec++;
                printf("    node %-3u -> no reconnect\n", children[c]);
            }
        }
        printf("    reconnect: min/avg/max = %u / %u / %u ms, unreconnected=%u\n",
               ev_min, ev_n ? ev_sum / ev_n : 0, ev_max, unrec);

        total_affected += (uint32_t)nc;
        total_unrec += unrec;
        all_sum += ev_sum;
        all_n += ev_n;
        if (ev_max > all_max) all_max = ev_max;
    }

    printf("\nsummary: events=%u  affected=%u  reconnect avg=%u ms max=%u ms  "
           "unreconnected=%u\n",
           events, total_affected, all_n ? all_sum / all_n : 0, all_max,
           total_unrec);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    printf("treenity repeater stress report\n");
    printf("profile: BEACON %u..%u ms, PARENT_TIMEOUT %u ms, NEIGHBOR_TIMEOUT %u ms, "
           "DWELL %u ms\n",
           TREENET_BEACON_MIN_MS, TREENET_BEACON_MAX_MS,
           TREENET_PARENT_TIMEOUT_MS, TREENET_NEIGHBOR_TIMEOUT_MS,
           TREENET_PARENT_DWELL_MS);

    sim_t *s = build_net();
    printf("nodes: %u (1 master + 30 repeaters), grid 6x5 @150 m, range 250 m, SF7\n",
           (unsigned)sim_node_count(s));

    printf("\nconverging...\n");
    sim_run(s, 120000);

    size_t connected = 0;
    for (size_t i = 0; i < N_NODES; i++) {
        if (treenet_is_connected(sim_node_at(s, i)->net)) connected++;
    }
    printf("connected after convergence: %u/%u\n",
           (unsigned)connected, (unsigned)N_NODES);

    scenario1(s);

    /* Let the topology settle again before the power-off scenario. */
    printf("\nre-converging before scenario 2...\n");
    sim_run(s, 60000);

    scenario2(s);

    sim_destroy(s);
    return 0;
}
