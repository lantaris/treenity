/**
 * @file test_treenet_threads.c
 * @brief Concurrency stress test at the library level.
 *
 * A producer thread feeds valid BEACON frames into treenet_rx() (the sole
 * producer of the receive ring, as a modem ISR would); a consumer thread drives
 * treenet_poll(). Every frame that treenet_rx accepted must be processed
 * exactly once, with no corruption of the internal tables.
 */
#include <stdio.h>
#include <string.h>

#include "treenet/treenet.h"
#include "core/internal.h"
#include "mac/frame.h"
#include "thread.h"

#define LIB_FRAMES 500000u

static treenet_t *g_node;
static uint8_t    g_ctx[32768];

static volatile uint32_t g_attempts;
static volatile uint32_t g_pushed;
static volatile int      g_stop;

/* --- minimal single-threaded port ---------------------------------------- */
static uint32_t g_now;
static uint32_t g_rnd = 0x12345678u;

static int port_tx(const uint8_t *buf, size_t len)
{
    (void)buf; (void)len;
    return 0;
}
static uint32_t port_now(void) { return ++g_now; }
static uint32_t port_rnd(void)
{
    uint32_t x = g_rnd;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rnd = x ? x : 1u;
    return g_rnd;
}

static void *rx_producer(void *arg)
{
    (void)arg;
    uint8_t buf[TREENET_MTU];
    uint8_t payload[TN_BEACON_PAYLOAD_LEN];
    tn_beacon_payload_t b;
    b.rank = 300;
    b.parent = 1;
    b.flags = TN_BEACON_HAS_PARENT;
    b.interval_100ms = 100;
    tn_beacon_encode(payload, &b);

    for (uint32_t i = 0; i < LIB_FRAMES; i++) {
        tn_frame_t f;
        memset(&f, 0, sizeof(f));
        f.version = TREENET_PROTOCOL_VERSION;
        f.type = TREENET_FRAME_BEACON;
        f.src = 100u + (i & 3u); /* four distinct sources */
        f.dst = TREENET_ADDR_BROADCAST;
        f.seq = (uint16_t)i;
        f.net_id = 1;
        f.hop_limit = 1;
        f.prev = f.src;
        f.payload = payload;
        f.payload_len = sizeof(payload);

        size_t len = tn_frame_encode(buf, sizeof(buf), &f);
        if (len == 0) continue;

        g_attempts++;
        if (treenet_rx(g_node, buf, len, -80, 10) == 0) {
            g_pushed++;
        }
    }
    g_stop = 1;
    return NULL;
}

static void *poll_consumer(void *arg)
{
    (void)arg;
    while (!g_stop) {
        treenet_poll(g_node);
    }
    return NULL;
}

int run_treenet_stress(void)
{
    treenet_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.addr = 2;
    cfg.role = TREENET_ROLE_NODE;
    cfg.net_id = 1;

    treenet_port_t port;
    memset(&port, 0, sizeof(port));
    port.tx = port_tx;
    port.now_ms = port_now;
    port.rnd = port_rnd;

    g_node = treenet_init(g_ctx, sizeof(g_ctx), &cfg, &port);
    if (g_node == NULL) {
        printf("  init failed (context too small?)\n");
        return -1;
    }

    g_attempts = 0;
    g_pushed = 0;
    g_stop = 0;
    g_now = 0;

    tn_stress_thread_t pt, ct;
    if (tn_stress_thread_start(&pt, rx_producer, NULL) != 0) return -1;
    if (tn_stress_thread_start(&ct, poll_consumer, NULL) != 0) return -1;
    tn_stress_thread_join(pt);
    tn_stress_thread_join(ct);

    /* Drain anything still queued (single-threaded now). */
    const treenet_stats_t *st = treenet_stats(g_node);
    for (uint32_t spins = 0; st->beacons_rx < g_pushed && spins < 1000000u;
         spins++) {
        treenet_poll(g_node);
    }

    printf("  attempts=%u accepted=%u beacons_rx=%u full-drops=%u "
           "neighbours=%u\n",
           g_attempts, g_pushed, st->beacons_rx, (unsigned)g_node->rx.dropped,
           (unsigned)g_node->neighbors.count);

    int ok = 1;
    if (st->beacons_rx != g_pushed) {
        printf("  MISMATCH: accepted %u but processed %u\n",
               g_pushed, st->beacons_rx);
        ok = 0;
    }
    if (g_node->neighbors.count > TREENET_MAX_NEIGHBORS) ok = 0;
    if (g_node->routes.count > TREENET_MAX_ROUTES) ok = 0;
    return ok ? 0 : -1;
}
