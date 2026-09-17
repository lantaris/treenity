/**
 * @file main.c
 * @brief Desktop example: build a small treenity mesh in the simulator and
 *        watch it converge, then exchange unicast and broadcast traffic.
 *
 * It also shows two features:
 *   - the LEAF role (an end device that never routes);
 *   - tickless scheduling: the port's timer_arm() callback, which the
 *     simulator records in sim_node_t::last_timer_arm. On real hardware you
 *     implement timer_arm yourself and sleep the MCU until it fires (see the
 *     porting guide under Doc/).
 *
 * Build (from the treenity directory):
 *   cmake -S . -B build && cmake --build build
 *   ./build/treenet_example
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "treenet/treenet.h"

static const char *role_name(treenet_role_t r)
{
    switch (r) {
    case TREENET_ROLE_MASTER:   return "master";
    case TREENET_ROLE_REPEATER: return "repeater";
    case TREENET_ROLE_LEAF:     return "leaf";
    default:                    return "node";
    }
}

/* Observation hooks: the simulator calls these whenever a node receives a
 * datagram or reports a network event. */
static void on_recv(sim_node_t *node, treenet_addr_t src, const uint8_t *data,
                    size_t len, int16_t rssi, int8_t snr, uint8_t hops)
{
    printf("  node %u <= from %u (%u bytes, rssi %d, snr %d, %u hops): %.*s\n",
           node->addr, src, (unsigned)len, rssi, snr, hops,
           (int)len, (const char *)data);
}

static void on_event(sim_node_t *node, treenet_event_t ev)
{
    static const char *names[] = {
        "NETWORK_READY", "JOINED", "PARENT_CHANGED", "NEIGHBOR_ADDED",
        "NEIGHBOR_REMOVED", "ROUTE_LOST", "TX_DONE", "TX_FAILED",
        "DISCONNECTED"
    };
    if (ev <= TREENET_EV_DISCONNECTED) {
        printf("  node %u event: %s\n", node->addr, names[ev]);
    }
}

int main(void)
{
    printf("treenity %s desktop example\n\n", treenet_version());

    /* Radio model: mild path loss with a hard 250 m range. */
    sim_t *s = sim_create(1234u, 20.0, -137.0, -120.0);
    sim_set_path_loss(s, 40.0, 3.0);
    sim_set_range(s, 250.0);
    sim_set_callbacks(s, on_recv, on_event);

    /* A Master, five routers over ~1 km, and a leaf sensor at the edge. */
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 200.0, 0.0, true);
    sim_add_node(s, 3, TREENET_ROLE_NODE, 400.0, 0.0, true);
    sim_add_node(s, 4, TREENET_ROLE_NODE, 600.0, 0.0, true);
    sim_add_node(s, 5, TREENET_ROLE_NODE, 800.0, 0.0, true);
    sim_add_node(s, 6, TREENET_ROLE_NODE, 1000.0, 0.0, true);
    sim_add_node(s, 7, TREENET_ROLE_LEAF, 1200.0, 0.0, true);

    /* Let the mesh form. */
    printf("forming the mesh...\n");
    sim_run(s, 120000);

    printf("\ntopology after convergence:\n");
    for (size_t i = 0; i < sim_node_count(s); i++) {
        sim_node_t *n = sim_node_at(s, i);
        printf("  node %u: role=%-8s rank=%-5u parent=%-3u connected=%s "
               "wake=%ums\n",
               n->addr, role_name(treenet_role(n->net)), treenet_rank(n->net),
               treenet_parent(n->net),
               treenet_is_connected(n->net) ? "yes" : "no",
               n->last_timer_arm);
    }

    /* Master -> far router (downward route). */
    printf("\nmaster sends a unicast to node 6:\n");
    const char *msg = "hello from the master";
    if (treenet_send(sim_find(s, 1)->net, 6, msg, strlen(msg)) == 0) {
        sim_run(s, 5000);
    } else {
        printf("  no route\n");
    }

    /* The leaf is a source too: it sends its own data upward. */
    printf("\nleaf (node 7) sends a reading to the master:\n");
    const char *reading = "temp=21.5";
    if (treenet_send(sim_find(s, 7)->net, 1, reading, strlen(reading)) == 0) {
        sim_run(s, 5000);
    } else {
        printf("  no route\n");
    }

    /* And a destination: the Master can address the leaf directly. */
    printf("\nmaster sends a unicast to the leaf (node 7):\n");
    const char *cmd = "set-interval=60";
    if (treenet_send(sim_find(s, 1)->net, 7, cmd, strlen(cmd)) == 0) {
        sim_run(s, 5000);
    } else {
        printf("  no route\n");
    }

    /* Mesh-wide broadcast via managed flooding; the leaf receives it but does
     * not rebroadcast it. */
    printf("\nnode 3 broadcasts to everyone:\n");
    const char *bc = "broadcast!";
    (void)treenet_broadcast(sim_find(s, 3)->net, bc, strlen(bc));
    sim_run(s, 10000);

    const treenet_stats_t *st = treenet_stats(sim_find(s, 3)->net);
    printf("\nnode 3 statistics: tx=%u rx=%u retx=%u beacons_tx=%u "
           "beacons_rx=%u parent_changes=%u airtime=%ums\n",
           st->frames_tx, st->frames_rx, st->retransmissions, st->beacons_tx,
           st->beacons_rx, st->parent_changes, st->airtime_ms);

    sim_destroy(s);
    printf("\ndone.\n");
    return 0;
}
