/**
 * @file test_mesh.c
 * @brief End-to-end scenario tests running the full stack on the simulator:
 *        network formation, rank ordering, unicast both ways, broadcast and
 *        seamless re-parenting when a relay fails.
 */
#include "test.h"

#include "sim.h"
#include "treenet/treenet.h"
#include "core/internal.h"

/* A mild path-loss model plus a hard 250 m range gives crisp, deterministic
 * topologies: a node is either clearly in range or clearly out of it, while
 * RSSI/SNR values remain realistic. */
#define SIM_TX_POWER   20.0
#define SIM_SENSITIVITY (-137.0)
#define SIM_NOISE      (-120.0)
#define SIM_REF_LOSS   40.0
#define SIM_EXPONENT   3.0
#define SIM_RANGE      250.0

/** Build a straight chain master - n1 - n2 - ... with 200 m spacing. */
static sim_t *make_chain(unsigned nodes)
{
    sim_t *s = sim_create(0xC0FFEEu, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);
    for (unsigned i = 1; i < nodes; i++) {
        sim_add_node(s, (treenet_addr_t)(i + 1), TREENET_ROLE_NODE,
                     (double)(i * 200), 0.0, false);
    }
    return s;
}

/* --- Event counters for the ACK / fast-repair tests ---------------------- */
typedef struct {
    uint32_t tx_done;
    uint32_t tx_failed;
    uint32_t parent_changed;
} evc_t;

static evc_t EVC[8];

static void ev_count(sim_node_t *n, treenet_event_t ev)
{
    if (n->addr >= 8) return;
    switch (ev) {
    case TREENET_EV_TX_DONE:        EVC[n->addr].tx_done++; break;
    case TREENET_EV_TX_FAILED:      EVC[n->addr].tx_failed++; break;
    case TREENET_EV_PARENT_CHANGED: EVC[n->addr].parent_changed++; break;
    default: break;
    }
}

/**
 * Build M(0,0) - X(150,0) - A(300,0) with B(150,150).
 *
 * A hears X and B but not M, so its parent is X. B hears M and X, so its parent
 * is M. When X dies, A's frames are still heard by B, which forwards them to M:
 * an opportunistic forwarder.
 */
static sim_t *make_ack_net(void)
{
    sim_t *s = sim_create(0xAAu, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, 70.0, 3.0);
    sim_set_range(s, SIM_RANGE);
    sim_set_callbacks(s, NULL, ev_count);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);   /* M */
    sim_add_node(s, 2, TREENET_ROLE_NODE, 150.0, 0.0, true);   /* X */
    sim_add_node(s, 3, TREENET_ROLE_NODE, 300.0, 0.0, true);   /* A */
    sim_add_node(s, 4, TREENET_ROLE_NODE, 150.0, 150.0, true); /* B */
    return s;
}

static void test_network_forms(void)
{
    sim_t *s = make_chain(4);
    sim_run(s, 120000);

    for (size_t i = 0; i < sim_node_count(s); i++) {
        sim_node_t *n = sim_node_at(s, i);
        CHECK(treenet_is_connected(n->net));
    }
    sim_destroy(s);
}

static void test_rank_ordering(void)
{
    sim_t *s = make_chain(4);
    sim_run(s, 120000);

    sim_node_t *master = sim_find(s, 1);
    sim_node_t *n1 = sim_find(s, 2);
    sim_node_t *n2 = sim_find(s, 3);
    sim_node_t *n3 = sim_find(s, 4);

    CHECK_EQ(treenet_rank(master->net), 0);
    /* Each hop must strictly increase the rank. */
    CHECK(treenet_rank(n1->net) > treenet_rank(master->net));
    CHECK(treenet_rank(n2->net) > treenet_rank(n1->net));
    CHECK(treenet_rank(n3->net) > treenet_rank(n2->net));

    /* The chain topology implies the parent relationships. */
    CHECK_EQ(treenet_parent(n1->net), 1u);
    CHECK_EQ(treenet_parent(n2->net), 2u);
    CHECK_EQ(treenet_parent(n3->net), 3u);

    sim_destroy(s);
}

static void test_unicast_down_and_up(void)
{
    sim_t *s = make_chain(4);
    sim_run(s, 120000);

    sim_node_t *master = sim_find(s, 1);
    sim_node_t *leaf = sim_find(s, 4);

    /* Master -> leaf (downward, uses the DAO-learned route). */
    uint32_t before = leaf->datagrams_rx;
    uint8_t msg[] = { 'h', 'e', 'l', 'l', 'o' };
    CHECK_EQ(treenet_send(master->net, 4, msg, sizeof(msg)), 0);
    sim_run(s, 5000);
    CHECK(leaf->datagrams_rx > before);
    CHECK_EQ(leaf->last_src, 1u);
    CHECK_EQ(leaf->last_len, sizeof(msg));

    /* Leaf -> master (upward, follows the parent chain). */
    uint32_t mbefore = master->datagrams_rx;
    uint8_t up[] = { 1, 2, 3 };
    CHECK_EQ(treenet_send(leaf->net, 1, up, sizeof(up)), 0);
    sim_run(s, 5000);
    CHECK(master->datagrams_rx > mbefore);
    CHECK_EQ(master->last_src, 4u);

    sim_destroy(s);
}

static void test_broadcast_reaches_all(void)
{
    sim_t *s = make_chain(4);
    sim_run(s, 120000);

    uint32_t before[SIM_MAX_NODES];
    for (size_t i = 0; i < sim_node_count(s); i++) {
        before[i] = sim_node_at(s, i)->datagrams_rx;
    }

    uint8_t msg[] = { 'B' };
    CHECK_EQ(treenet_broadcast(sim_find(s, 4)->net, msg, sizeof(msg)), 0);
    sim_run(s, 10000);

    for (size_t i = 0; i < sim_node_count(s); i++) {
        sim_node_t *n = sim_node_at(s, i);
        if (n->addr == 4) continue; /* sender does not deliver to itself */
        CHECK(n->datagrams_rx > before[i]);
    }
    sim_destroy(s);
}

static void test_seamless_reparenting(void)
{
    /* Grid where the mobile-ish node N hears two relays. */
    sim_t *s = sim_create(0xBEEFu, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 200.0, 0.0, false);   /* relay A */
    sim_add_node(s, 3, TREENET_ROLE_NODE, 0.0, 200.0, false);   /* relay B */
    sim_add_node(s, 4, TREENET_ROLE_NODE, 200.0, 200.0, false); /* N */

    sim_run(s, 120000);

    sim_node_t *n = sim_find(s, 4);
    CHECK(treenet_is_connected(n->net));
    treenet_addr_t old_parent = treenet_parent(n->net);
    CHECK(old_parent == 2u || old_parent == 3u);

    /* Fail the relay N currently depends on. */
    sim_set_active(s, old_parent, false);
    sim_run(s, 120000);

    CHECK(treenet_is_connected(n->net));
    treenet_addr_t new_parent = treenet_parent(n->net);
    CHECK(new_parent != old_parent);
    CHECK(new_parent != TREENET_ADDR_INVALID);

    sim_destroy(s);
}

static void test_beacon_reset_on_reparent(void)
{
    /* Grid where N hears two relays but not the Master directly. */
    sim_t *s = sim_create(0xBEEFu, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 200.0, 0.0, false);   /* relay A */
    sim_add_node(s, 3, TREENET_ROLE_NODE, 0.0, 200.0, false);   /* relay B */
    sim_add_node(s, 4, TREENET_ROLE_NODE, 200.0, 200.0, false); /* N */

    sim_run(s, 120000);

    treenet_t *n = sim_find(s, 4)->net;
    CHECK(treenet_is_connected(n));
    treenet_addr_t old_parent = treenet_parent(n);
    CHECK(old_parent == 2u || old_parent == 3u);

    /* In a stable network the Trickle interval has grown past the minimum. */
    CHECK(n->trickle_I > TREENET_BEACON_MIN_MS);

    /* Fail the current parent and wait (in 1 s slices) until N re-parents. */
    sim_set_active(s, old_parent, false);
    uint32_t start = sim_now(s);
    while (treenet_parent(n) == old_parent &&
           (uint32_t)(sim_now(s) - start) < 150000u) {
        sim_run(s, 1000);
    }

    CHECK(treenet_parent(n) != old_parent);
    CHECK(treenet_is_connected(n));

    /* The Trickle interval must have been reset to the minimum so the new
     * parent is advertised quickly. The slice overshoot is at most 1 s, which
     * is below the minimum beacon delay (2.5 s), so the interval cannot have
     * doubled again yet. */
    CHECK_EQ(n->trickle_I, TREENET_BEACON_MIN_MS);

    /* Behavioural confirmation: a beacon follows within ~6 s, which only holds
     * because the interval was reset (otherwise it could be up to 20 s away). */
    uint32_t beacons_before = treenet_stats(n)->beacons_tx;
    sim_run(s, 6000);
    CHECK(treenet_stats(n)->beacons_tx > beacons_before);

    sim_destroy(s);
}

static void test_timer_arm_idle(void)
{
    /* Tickless scheduling: in a stable network the wake timer is bounded by
     * the beacon interval, not by a fixed 5 s maintenance tick. */
    sim_t *s = make_chain(2);
    sim_run(s, 120000);

    sim_node_t *n = sim_find(s, 2);
    CHECK(n->timer_arm_count > 0);
    CHECK(n->last_timer_arm <= TREENET_BEACON_MAX_MS);
    CHECK(n->max_timer_arm > 5000); /* no artificial 5 s cap */

    sim_destroy(s);
}

static void test_timer_arm_disconnected(void)
{
    /* A lone node with no Master keeps searching: the PROBE interval bounds
     * the wake timer. */
    sim_t *s = sim_create(3u, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_NODE, 0.0, 0.0, false);
    sim_run(s, 30000);

    sim_node_t *n = sim_find(s, 1);
    CHECK(!treenet_is_connected(n->net));
    CHECK(n->timer_arm_count > 0);
    CHECK(n->last_timer_arm <= TREENET_PROBE_INTERVAL_MS);

    sim_destroy(s);
}

static void test_timer_arm_tx(void)
{
    /* A queued / ACK-awaiting frame must be reflected in the wake deadline. */
    sim_t *s = sim_create(5u, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 100.0, 0.0, true); /* reliable */
    sim_run(s, 60000);

    sim_node_t *n = sim_find(s, 2);
    uint8_t msg = 0x42;
    CHECK_EQ(treenet_send(n->net, 1, &msg, 1), 0);
    sim_run(s, 10); /* one poll recomputes the deadline */

    uint32_t now = n->net->now_ms;
    uint32_t tx_rem = UINT32_MAX;
    for (size_t i = 0; i < TREENET_TX_QUEUE_SIZE; i++) {
        if (!n->net->tx[i].valid) continue;
        uint32_t due = n->net->tx[i].next_tx_ms;
        uint32_t rem = tn_time_after(now, due) ? 0u : (due - now);
        if (rem < tx_rem) tx_rem = rem;
    }
    CHECK(tx_rem != UINT32_MAX);
    if (tx_rem != UINT32_MAX) {
        CHECK(n->last_timer_arm <= tx_rem);
    }

    sim_destroy(s);
}

static void test_leaf_joins_and_traffic(void)
{
    sim_t *s = sim_create(11u, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);
    sim_add_node(s, 2, TREENET_ROLE_LEAF, 100.0, 0.0, true);
    sim_run(s, 60000);

    sim_node_t *master = sim_find(s, 1);
    sim_node_t *leaf = sim_find(s, 2);
    CHECK_EQ(treenet_role(leaf->net), TREENET_ROLE_LEAF);
    CHECK(treenet_is_connected(leaf->net));
    CHECK_EQ(treenet_parent(leaf->net), 1u);

    /* Leaf -> Master (uplink). */
    uint8_t up[3] = { 1, 2, 3 };
    CHECK_EQ(treenet_send(leaf->net, 1, up, sizeof(up)), 0);
    sim_run(s, 5000);
    CHECK(master->datagrams_rx > 0);

    /* Master -> leaf (downward route built by the leaf's DAO). */
    uint32_t before = leaf->datagrams_rx;
    uint8_t down[2] = { 9, 9 };
    CHECK_EQ(treenet_send(master->net, 2, down, sizeof(down)), 0);
    sim_run(s, 5000);
    CHECK(leaf->datagrams_rx > before);

    sim_destroy(s);
}

static void test_leaf_not_a_parent(void)
{
    /* The node is closer to the leaf than to the Master, but a leaf must never
     * be chosen as a parent. */
    sim_t *s = sim_create(13u, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 100.0, 0.0, false);
    sim_add_node(s, 3, TREENET_ROLE_LEAF, 50.0, 0.0, false);
    sim_run(s, 90000);

    sim_node_t *node = sim_find(s, 2);
    sim_node_t *leaf = sim_find(s, 3);

    CHECK(treenet_is_connected(node->net));
    CHECK_EQ(treenet_parent(node->net), 1u); /* Master, not the leaf */
    CHECK(treenet_is_connected(leaf->net));
    treenet_addr_t lp = treenet_parent(leaf->net);
    CHECK(lp == 1u || lp == 2u); /* the leaf uses a router */

    sim_destroy(s);
}

static void test_leaf_does_not_rebroadcast(void)
{
    sim_t *s = sim_create(17u, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);
    sim_add_node(s, 2, TREENET_ROLE_LEAF, 200.0, 0.0, false);
    sim_add_node(s, 3, TREENET_ROLE_NODE, 400.0, 0.0, false); /* beyond the leaf */
    sim_run(s, 90000);

    sim_node_t *master = sim_find(s, 1);
    sim_node_t *leaf = sim_find(s, 2);
    sim_node_t *far = sim_find(s, 3);

    CHECK(treenet_is_connected(leaf->net));
    /* The far node can only hear the leaf, which is not a router. */
    CHECK(!treenet_is_connected(far->net));

    /* A Master broadcast reaches the leaf but is not rebroadcast by it, so the
     * far node never sees it. */
    uint32_t leaf_before = leaf->datagrams_rx;
    uint8_t msg = 0x5A;
    (void)treenet_broadcast(master->net, &msg, 1);
    sim_run(s, 10000);

    CHECK(leaf->datagrams_rx > leaf_before);
    CHECK_EQ(far->datagrams_rx, 0u);

    sim_destroy(s);
}

static void test_bit_errors_tolerated(void)
{
    /* A noisy channel: the CRC rejects corrupted frames, so the mesh sees
     * extra loss but must stay connected and never adopt a bogus parent. */
    sim_t *s = make_chain(4);
    sim_set_bit_error_rate(s, 0.0004);
    sim_run(s, 180000);

    for (size_t i = 0; i < sim_node_count(s); i++) {
        sim_node_t *n = sim_node_at(s, i);
        CHECK(treenet_is_connected(n->net));

        treenet_addr_t p = treenet_parent(n->net);
        if (n->role == TREENET_ROLE_MASTER) {
            CHECK_EQ(p, TREENET_ADDR_INVALID);
            continue;
        }
        CHECK(p != TREENET_ADDR_INVALID);

        /* The parent must always be a node we actually hear. */
        treenet_neighbor_info_t nb[32];
        size_t c = treenet_neighbors(n->net, nb, 32);
        bool found = false;
        for (size_t k = 0; k < c; k++) {
            if (nb[k].addr == p) found = true;
        }
        CHECK(found);
    }
    sim_destroy(s);
}

static void test_ack_from_any_neighbor(void)
{
    memset(EVC, 0, sizeof(EVC));
    sim_t *s = make_ack_net();
    sim_run(s, 90000);

    sim_node_t *master = sim_find(s, 1);
    sim_node_t *A = sim_find(s, 3);
    CHECK_EQ(treenet_parent(A->net), 2u); /* A's parent is X */

    /* X loses power. A's frame is forwarded by B, whose ACK A now accepts. */
    sim_set_active(s, 2, false);
    uint32_t mbefore = master->datagrams_rx;
    uint32_t done_before = EVC[3].tx_done;

    uint8_t msg[3] = { 7, 7, 7 };
    CHECK_EQ(treenet_send(A->net, 1, msg, sizeof(msg)), 0);
    sim_run(s, 5000);

    CHECK(master->datagrams_rx > mbefore); /* delivered via B */
    CHECK(EVC[3].tx_done > done_before);   /* A recognised B's ACK */
    CHECK_EQ(EVC[3].tx_failed, 0u);

    sim_destroy(s);
}

static void test_fast_repair_on_ack_failure(void)
{
    memset(EVC, 0, sizeof(EVC));
    sim_t *s = make_ack_net();
    sim_run(s, 90000);

    sim_node_t *A = sim_find(s, 3);
    CHECK_EQ(treenet_parent(A->net), 2u);

    sim_set_active(s, 2, false);
    uint32_t T = sim_now(s);
    uint32_t changes_before = EVC[3].parent_changed;

    /* Keep sending. Each frame is forwarded by B, but X never acknowledges, so
     * after TREENET_ACK_FAIL_THRESHOLD frames X is marked suspect and A
     * re-parents to B, well before the beacon timeout. */
    for (int i = 0; i < 10; i++) {
        uint8_t msg[2] = { 1, 2 };
        (void)treenet_send(A->net, 1, msg, sizeof(msg));
        sim_run(s, 500);
        if (EVC[3].parent_changed > changes_before) break;
    }
    uint32_t reconn = sim_now(s) - T;

    CHECK(EVC[3].parent_changed > changes_before);
    CHECK_EQ(treenet_parent(A->net), 4u);      /* re-parented to B */
    CHECK(reconn < TREENET_PARENT_TIMEOUT_MS); /* faster than the beacon timeout */
    CHECK(reconn <= 10000u);

    sim_destroy(s);
}

static void test_coexisting_networks(void)
{
    /* Two co-located networks (different net_id) share the channel physically
     * but must not exchange anything. */
    sim_t *s = sim_create(0xC0u, SIM_TX_POWER, SIM_SENSITIVITY, SIM_NOISE);
    sim_set_path_loss(s, SIM_REF_LOSS, SIM_EXPONENT);
    sim_set_range(s, SIM_RANGE);

    sim_set_net_id(s, 1);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);   /* net 1 */
    sim_add_node(s, 2, TREENET_ROLE_NODE, 100.0, 0.0, false);

    sim_set_net_id(s, 2);
    sim_add_node(s, 3, TREENET_ROLE_MASTER, 0.0, 150.0, false); /* net 2 */
    sim_add_node(s, 4, TREENET_ROLE_NODE, 100.0, 150.0, false);

    sim_run(s, 60000);

    sim_node_t *a = sim_find(s, 2);
    sim_node_t *b = sim_find(s, 4);

    CHECK(treenet_is_connected(a->net));
    CHECK_EQ(treenet_parent(a->net), 1u); /* joined its own Master, not net 2 */
    CHECK(treenet_is_connected(b->net));
    CHECK_EQ(treenet_parent(b->net), 3u);

    /* Foreign frames are filtered before link metrics, so nodes of the other
     * network never appear as neighbours. */
    treenet_neighbor_info_t nb[8];
    CHECK_EQ(treenet_neighbors(a->net, nb, 8), 1u);
    CHECK_EQ(nb[0].addr, 1u);

    /* A broadcast in network 1 must not reach network 2. */
    uint32_t b_before = b->datagrams_rx;
    uint8_t msg = 0x5A;
    CHECK_EQ(treenet_broadcast(sim_find(s, 1)->net, &msg, 1), 0);
    sim_run(s, 5000);
    CHECK_EQ(b->datagrams_rx, b_before);

    sim_destroy(s);
}

static void test_neighbor_metrics(void)
{
    sim_t *s = make_chain(2); /* master + one node */
    sim_run(s, 60000);

    sim_node_t *n = sim_find(s, 2);
    treenet_neighbor_info_t info[8];
    size_t count = treenet_neighbors(n->net, info, 8);
    CHECK(count >= 1);
    if (count >= 1) {
        /* The master should be visible as a neighbour with a sane link. */
        CHECK_EQ(info[0].addr, 1u);
        CHECK(info[0].is_parent);
        CHECK(info[0].lq.rssi_dbm < 0);
        CHECK(info[0].lq.link_cost <= 512);
    }
    sim_destroy(s);
}

static int tn_test_tx(const uint8_t *b, size_t n) { (void)b; (void)n; return 0; }
static uint32_t tn_test_now(void) { return 0; }
static uint32_t tn_test_rnd(void) { return 1; }

static void test_init_validation(void)
{
    static union {
        uint64_t align;
        uint8_t  bytes[16384];
    } ctx;

    treenet_port_t port;
    memset(&port, 0, sizeof(port));
    port.tx = tn_test_tx;
    port.now_ms = tn_test_now;
    port.rnd = tn_test_rnd;

    treenet_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.addr = 0x1234;
    cfg.role = TREENET_ROLE_NODE;
    cfg.net_id = 1;

    CHECK(treenet_init(ctx.bytes, sizeof ctx.bytes, &cfg, &port) != NULL);

    cfg.role = (treenet_role_t)99; /* unknown role */
    CHECK(treenet_init(ctx.bytes, sizeof ctx.bytes, &cfg, &port) == NULL);
    cfg.role = TREENET_ROLE_NODE;

    CHECK(treenet_init(ctx.bytes, 16u, &cfg, &port) == NULL); /* too small */
    CHECK(treenet_init(ctx.bytes + 1u, sizeof ctx.bytes - 1u,
                       &cfg, &port) == NULL); /* unaligned */
}

void run_mesh_tests(void)
{
    RUN_TEST(test_init_validation);
    RUN_TEST(test_network_forms);
    RUN_TEST(test_rank_ordering);
    RUN_TEST(test_unicast_down_and_up);
    RUN_TEST(test_broadcast_reaches_all);
    RUN_TEST(test_seamless_reparenting);
    RUN_TEST(test_beacon_reset_on_reparent);
    RUN_TEST(test_timer_arm_idle);
    RUN_TEST(test_timer_arm_disconnected);
    RUN_TEST(test_timer_arm_tx);
    RUN_TEST(test_leaf_joins_and_traffic);
    RUN_TEST(test_leaf_not_a_parent);
    RUN_TEST(test_leaf_does_not_rebroadcast);
    RUN_TEST(test_ack_from_any_neighbor);
    RUN_TEST(test_fast_repair_on_ack_failure);
    RUN_TEST(test_coexisting_networks);
    RUN_TEST(test_bit_errors_tolerated);
    RUN_TEST(test_neighbor_metrics);
}
