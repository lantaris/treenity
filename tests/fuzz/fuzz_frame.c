/**
 * @file fuzz_frame.c
 * @brief Robustness harness for the frame parser and the receive path.
 *
 * Two input strategies are combined:
 *   - a mutation fuzzer that starts from a corpus of valid frames (one per
 *     type, plus a fragment) and applies random mutations. This reaches deep
 *     code paths far more often than pure random bytes, which the CRC almost
 *     always rejects;
 *   - a pure random-byte generator for good measure.
 *
 * After every input the harness checks structural invariants of the live
 * instance (table sizes, rank/connectivity consistency, parent sanity) and
 * aborts on violation. The decoders must never report success on a malformed
 * buffer and the library must never crash.
 *
 * It runs as a normal executable (any toolchain) and can also be linked with
 * libFuzzer by defining TREENET_LIBFUZZER, which provides only
 * LLVMFuzzerTestOneInput.
 *
 * Build & run (from the treenity directory):
 *   make fuzz        (or link the core sources plus sim/sim.c manually)
 *   ./build/fuzz 200000
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "treenet/treenet.h"
#include "core/internal.h"

/* ------------------------------------------------------------------------- */
/* PRNG                                                                       */
/* ------------------------------------------------------------------------- */

static uint32_t fuzz_state = 0x12345678u;

static uint32_t fuzz_rand(void)
{
    uint32_t x = fuzz_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fuzz_state = x ? x : 0x9E3779B9u;
    return fuzz_state;
}

static uint32_t fuzz_rand_range(uint32_t n)
{
    return n ? (fuzz_rand() % n) : 0u;
}

/* ------------------------------------------------------------------------- */
/* Seed corpus of valid frames                                                */
/* ------------------------------------------------------------------------- */

#define CORPUS_MAX 8

static uint8_t g_corpus[CORPUS_MAX][TREENET_MTU];
static size_t  g_corpus_len[CORPUS_MAX];
static int     g_corpus_count = 0;

static void corpus_add(const tn_frame_t *f)
{
    if (g_corpus_count >= CORPUS_MAX) return;
    size_t n = tn_frame_encode(g_corpus[g_corpus_count], TREENET_MTU, f);
    if (n > 0) {
        g_corpus_len[g_corpus_count] = n;
        g_corpus_count++;
    }
}

static void build_corpus(void)
{
    if (g_corpus_count > 0) return;

    tn_frame_t f;
    uint8_t payload[TN_BEACON_PAYLOAD_LEN];

    /* BEACON */
    tn_beacon_payload_t b;
    b.rank = 300;
    b.parent = 1;
    b.flags = TN_BEACON_HAS_PARENT;
    b.interval_100ms = 100;
    tn_beacon_encode(payload, &b);
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_BEACON;
    f.src = 2; f.dst = TREENET_ADDR_BROADCAST; f.seq = 1; f.net_id = 1;
    f.hop_limit = 1; f.prev = 2;
    f.payload = payload; f.payload_len = sizeof(payload);
    corpus_add(&f);

    /* DATA */
    uint8_t data[6] = { 1, 2, 3, 4, 5, 6 };
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DATA;
    f.flags = TN_FLAG_WANT_ACK;
    f.src = 2; f.dst = 1; f.seq = 2; f.net_id = 1; f.hop_limit = 8; f.prev = 2;
    f.payload = data; f.payload_len = sizeof(data);
    corpus_add(&f);

    /* ACK */
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_ACK;
    f.src = 1; f.dst = 2; f.seq = 2; f.net_id = 1; f.hop_limit = 1; f.prev = 1;
    corpus_add(&f);

    /* FLOOD */
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_FLOOD;
    f.src = 2; f.dst = TREENET_ADDR_BROADCAST; f.seq = 3; f.net_id = 1;
    f.hop_limit = 8; f.prev = 2;
    f.payload = data; f.payload_len = sizeof(data);
    corpus_add(&f);

    /* PROBE */
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_PROBE;
    f.src = 2; f.dst = TREENET_ADDR_BROADCAST; f.seq = 4; f.net_id = 1;
    f.hop_limit = 1; f.prev = 2;
    corpus_add(&f);

    /* DAO */
    uint8_t dao[TN_DAO_PAYLOAD_LEN];
    tn_dao_payload_t d;
    d.origin = 2; d.hops = 0;
    tn_dao_encode(dao, &d);
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DAO;
    f.flags = TN_FLAG_WANT_ACK;
    f.src = 2; f.dst = 1; f.seq = 5; f.net_id = 1; f.hop_limit = 8; f.prev = 2;
    f.payload = dao; f.payload_len = sizeof(dao);
    corpus_add(&f);

    /* Fragmented DATA */
    uint8_t frag[TN_FRAG_HDR_LEN + 8];
    tn_frag_hdr_t fh;
    fh.dgram_id = 1; fh.index = 0; fh.count = 2;
    tn_frag_encode(frag, &fh);
    memcpy(frag + TN_FRAG_HDR_LEN, "abcdefgh", 8);
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DATA;
    f.flags = TN_FLAG_FRAG;
    f.src = 2; f.dst = 1; f.seq = 6; f.net_id = 1; f.hop_limit = 8; f.prev = 2;
    f.payload = frag; f.payload_len = sizeof(frag);
    corpus_add(&f);
}

/** Apply a few random mutations to a buffer. */
static void mutate(uint8_t *buf, size_t *len)
{
    int rounds = 1 + (int)fuzz_rand_range(4);
    for (int r = 0; r < rounds; r++) {
        if (*len == 0) break;
        switch (fuzz_rand_range(4)) {
        case 0: /* bit flip */
            buf[fuzz_rand_range((uint32_t)*len)] ^=
                (uint8_t)(1u << fuzz_rand_range(8));
            break;
        case 1: /* random byte */
            buf[fuzz_rand_range((uint32_t)*len)] = (uint8_t)fuzz_rand();
            break;
        case 2: /* truncate */
            *len = fuzz_rand_range((uint32_t)(*len + 1));
            break;
        case 3: /* swap two bytes */
            if (*len > 1) {
                size_t i = fuzz_rand_range((uint32_t)*len);
                size_t j = fuzz_rand_range((uint32_t)*len);
                uint8_t t = buf[i];
                buf[i] = buf[j];
                buf[j] = t;
            }
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Live instance and invariant checks                                         */
/* ------------------------------------------------------------------------- */

static sim_t *g_sim = NULL;

static sim_t *get_sim(void)
{
    if (g_sim == NULL) {
        g_sim = sim_create(1u, 20.0, -137.0, -120.0);
        sim_set_path_loss(g_sim, 40.0, 3.0);
        sim_set_range(g_sim, 250.0);
        sim_add_node(g_sim, 1, TREENET_ROLE_MASTER, 0.0, 0.0, true);
        sim_add_node(g_sim, 2, TREENET_ROLE_NODE, 100.0, 0.0, true);
    }
    return g_sim;
}

static void invariant_fail(const char *what, const treenet_t *t)
{
    fprintf(stderr, "BUG: invariant violated: %s (addr=%u rank=%u parent=%u "
                    "connected=%d nbrs=%u routes=%u)\n",
            what, treenet_addr(t), treenet_rank(t), treenet_parent(t),
            treenet_is_connected(t), t->neighbors.count, t->routes.count);
    abort();
}

static void check_invariants(const treenet_t *t)
{
    if (t->neighbors.count > TREENET_MAX_NEIGHBORS) {
        invariant_fail("neighbour table overflow", t);
    }
    if (t->routes.count > TREENET_MAX_ROUTES) {
        invariant_fail("route table overflow", t);
    }
    if (t->role == TREENET_ROLE_MASTER) {
        if (t->rank != 0 || t->parent != TREENET_ADDR_INVALID) {
            invariant_fail("master state corrupted", t);
        }
        return;
    }
    if (t->parent == t->addr) {
        invariant_fail("node is its own parent", t);
    }
    if ((t->rank != TREENET_RANK_INFINITE) != t->connected) {
        invariant_fail("rank/connected mismatch", t);
    }
    if (t->connected && t->parent == TREENET_ADDR_INVALID) {
        invariant_fail("connected without parent", t);
    }
}

/* ------------------------------------------------------------------------- */
/* Decoders                                                                   */
/* ------------------------------------------------------------------------- */

static void fuzz_decoders(const uint8_t *buf, size_t len)
{
    tn_frame_t f;
    if (!tn_frame_decode(buf, len, &f)) {
        return;
    }
    /* A successfully decoded frame must have a sane header. */
    if (f.payload_len > TREENET_MTU) {
        fprintf(stderr, "BUG: payload_len %zu exceeds MTU\n", f.payload_len);
        abort();
    }
    tn_beacon_payload_t b;
    (void)tn_beacon_decode(f.payload, f.payload_len, &b);
    tn_dao_payload_t d;
    (void)tn_dao_decode(f.payload, f.payload_len, &d);
    tn_frag_hdr_t fr;
    (void)tn_frag_decode(f.payload, f.payload_len, &fr);
}

static void fuzz_rx_path(const uint8_t *buf, size_t len)
{
    sim_t *s = get_sim();
    for (size_t i = 0; i < sim_node_count(s); i++) {
        sim_node_t *n = sim_node_at(s, i);
        (void)treenet_rx(n->net, buf, len,
                         (int16_t)fuzz_rand_range(120) - 120,
                         (int8_t)(fuzz_rand_range(40) - 20));
        treenet_poll(n->net);
        check_invariants(n->net);
    }
}

static void run_one(const uint8_t *buf, size_t len)
{
    fuzz_decoders(buf, len);
    fuzz_rx_path(buf, len);
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                               */
/* ------------------------------------------------------------------------- */

#ifdef TREENET_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_state = (uint32_t)(size * 2654435761u) | 1u;
    run_one(data, size);
    return 0;
}
#else
int main(int argc, char **argv)
{
    unsigned iterations = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 10)
                                     : 100000u;

    build_corpus();
    printf("fuzzing frame parser and rx path (%u iterations, %d seeds)...\n",
           iterations, g_corpus_count);

    uint8_t buf[TREENET_MTU + 16];

    for (unsigned i = 0; i < iterations; i++) {
        size_t len;
        if ((i & 1u) == 0) {
            /* Mutation of a valid seed frame. */
            size_t idx = (size_t)fuzz_rand_range((uint32_t)g_corpus_count);
            len = g_corpus_len[idx];
            memcpy(buf, g_corpus[idx], len);
            mutate(buf, &len);
        } else {
            /* Pure random bytes. */
            len = fuzz_rand_range(sizeof(buf));
            for (size_t k = 0; k < len; k++) {
                buf[k] = (uint8_t)fuzz_rand();
            }
        }
        run_one(buf, len);
    }

    printf("ok: no crashes, no invariant violations\n");
    return 0;
}
#endif
