/**
 * @file main.c
 * @brief Desktop example: build a small treenity mesh in the simulator and
 *        watch it converge, then exchange unicast and broadcast traffic.
 *
 * Build (from the treenity directory):
 *   cmake -S . -B build && cmake --build build
 *   ./build/treenet_example
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "treenet/treenet.h"

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

    /* A Master plus five nodes spread over ~1 km, forcing several hops. */
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 200.0, 0.0, true);
    sim_add_node(s, 3, TREENET_ROLE_NODE, 400.0, 0.0, true);
    sim_add_node(s, 4, TREENET_ROLE_NODE, 600.0, 0.0, true);
    sim_add_node(s, 5, TREENET_ROLE_NODE, 800.0, 0.0, true);
    sim_add_node(s, 6, TREENET_ROLE_NODE, 1000.0, 0.0, true);

    /* Let the mesh form. */
    printf("forming the mesh...\n");
    sim_run(s, 120000);

    printf("\ntopology after convergence:\n");
    for (size_t i = 0; i < sim_node_count(s); i++) {
        sim_node_t *n = sim_node_at(s, i);
        printf("  node %u: rank=%-5u parent=%-3u connected=%s\n", n->addr,
               treenet_rank(n->net), treenet_parent(n->net),
               treenet_is_connected(n->net) ? "yes" : "no");
    }

    /* Master -> far leaf (downward route). */
    printf("\nmaster sends a unicast to node 6:\n");
    const char *msg = "hello from the master";
    if (treenet_send(sim_find(s, 1)->net, 6, msg, strlen(msg)) == 0) {
        sim_run(s, 5000);
    } else {
        printf("  no route\n");
    }

    /* Leaf -> master (upward along the parent chain). */
    printf("\nnode 6 sends a unicast to the master:\n");
    const char *reply = "ack from node 6";
    if (treenet_send(sim_find(s, 6)->net, 1, reply, strlen(reply)) == 0) {
        sim_run(s, 5000);
    } else {
        printf("  no route\n");
    }

    /* Mesh-wide broadcast via managed flooding. */
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
