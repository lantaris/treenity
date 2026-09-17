/**
 * @file test_corrupt.c
 * @brief Corruption and malformed-input tests for the receive path.
 *
 * These tests verify that a damaged, truncated or semantically invalid frame
 * can never crash the library, never install bogus state (neighbours, parent,
 * routes) and is always dropped when the frame integrity check is enabled.
 */
#include "test.h"

#include <string.h>

#include "sim.h"
#include "core/internal.h"

/** Payload bytes carried by one full fragment (mirrors the sender). */
#define TEST_FRAG_CHUNK (TREENET_MTU - TN_FRAME_OVERHEAD - TN_FRAG_HDR_LEN)

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/** Encode a frame with the given fields and return its length. */
static size_t enc(uint8_t *buf, size_t cap, uint8_t type, treenet_addr_t src,
                  treenet_addr_t dst, uint16_t net_id, const uint8_t *pl,
                  size_t plen)
{
    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = type;
    f.src = src;
    f.dst = dst;
    f.seq = 1;
    f.net_id = net_id;
    f.hop_limit = 8;
    f.prev = src;
    f.payload = pl;
    f.payload_len = plen;
    return tn_frame_encode(buf, cap, &f);
}

/** Build a small, fully converged two-node network (master + one node). */
static sim_t *make_net(void)
{
    sim_t *s = sim_create(7u, 20.0, -137.0, -120.0);
    sim_set_path_loss(s, 40.0, 3.0);
    sim_set_range(s, 250.0);
    sim_add_node(s, 1, TREENET_ROLE_MASTER, 0.0, 0.0, false);
    sim_add_node(s, 2, TREENET_ROLE_NODE, 100.0, 0.0, false);
    sim_run(s, 60000);
    return s;
}

/** Snapshot of the routing/link state that corruption must never change. */
typedef struct {
    uint32_t       neighbors;
    uint32_t       routes;
    treenet_addr_t parent;
    uint16_t       rank;
    bool           connected;
} state_t;

static state_t snapshot(const treenet_t *t)
{
    state_t s;
    s.neighbors = t->neighbors.count;
    s.routes = t->routes.count;
    s.parent = t->parent;
    s.rank = t->rank;
    s.connected = t->connected;
    return s;
}

static void check_state_equal(const state_t *a, const state_t *b)
{
    CHECK_EQ(a->neighbors, b->neighbors);
    CHECK_EQ(a->routes, b->routes);
    CHECK_EQ(a->parent, b->parent);
    CHECK_EQ(a->rank, b->rank);
    CHECK_EQ(a->connected, b->connected);
}

/** Inject a raw buffer into a node and run the poll loop. */
static void inject(sim_t *s, treenet_addr_t dst, const uint8_t *buf, size_t len)
{
    sim_node_t *n = sim_find(s, dst);
    if (n == NULL) return;
    (void)treenet_rx(n->net, buf, len, -80, 10);
    treenet_poll(n->net);
}

/* ------------------------------------------------------------------------- */
/* Integrity check (CRC)                                                      */
/* ------------------------------------------------------------------------- */

#if TREENET_ENABLE_FRAME_CRC
static void test_crc_detects_bitflip(void)
{
    const uint8_t types[] = {
        TREENET_FRAME_BEACON, TREENET_FRAME_DATA, TREENET_FRAME_ACK,
        TREENET_FRAME_FLOOD, TREENET_FRAME_PROBE, TREENET_FRAME_DAO
    };
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    for (size_t ti = 0; ti < sizeof(types); ti++) {
        uint8_t buf[TREENET_MTU];
        size_t len = enc(buf, sizeof(buf), types[ti], 0x11, 0x22, 1,
                         payload, sizeof(payload));
        CHECK(len > 0);

        tn_frame_t d;
        CHECK(tn_frame_decode(buf, len, &d)); /* baseline decodes */

        /* Every single-bit corruption must be rejected. */
        for (size_t bit = 0; bit < len * 8u; bit++) {
            uint8_t mut[TREENET_MTU];
            memcpy(mut, buf, len);
            mut[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
            tn_frame_t dd;
            CHECK(!tn_frame_decode(mut, len, &dd));
        }
    }
}

static void test_crc_detects_truncation(void)
{
    uint8_t payload[6] = { 9, 8, 7, 6, 5, 4 };
    uint8_t buf[TREENET_MTU];
    size_t len = enc(buf, sizeof(buf), TREENET_FRAME_DATA, 0x11, 0x22, 1,
                     payload, sizeof(payload));
    CHECK(len > 0);

    for (size_t l = 0; l < len; l++) {
        tn_frame_t d;
        CHECK(!tn_frame_decode(buf, l, &d));
    }
}
#endif /* TREENET_ENABLE_FRAME_CRC */

static void test_wrong_version_rejected(void)
{
    uint8_t buf[TREENET_MTU];
    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = (uint8_t)(TREENET_PROTOCOL_VERSION + 1u); /* wrong version */
    f.type = TREENET_FRAME_DATA;
    f.src = 0x11;
    f.dst = 0x22;
    f.net_id = 1;
    f.hop_limit = 8;
    f.prev = 0x11;
    size_t len = tn_frame_encode(buf, sizeof(buf), &f);
    CHECK(len > 0);

    tn_frame_t d;
    CHECK(!tn_frame_decode(buf, len, &d));
}

static void test_invalid_addresses_rejected(void)
{
    uint8_t buf[TREENET_MTU];
    tn_frame_t d;

    /* Source == 0 (null address). */
    size_t len = enc(buf, sizeof(buf), TREENET_FRAME_DATA, TREENET_ADDR_INVALID,
                     0x22, 1, NULL, 0);
    CHECK(len > 0);
    CHECK(!tn_frame_decode(buf, len, &d));

    /* Source == broadcast. */
    len = enc(buf, sizeof(buf), TREENET_FRAME_DATA, TREENET_ADDR_BROADCAST,
              0x22, 1, NULL, 0);
    CHECK(len > 0);
    CHECK(!tn_frame_decode(buf, len, &d));

    /* Destination == 0 (null address). */
    len = enc(buf, sizeof(buf), TREENET_FRAME_DATA, 0x11, TREENET_ADDR_INVALID,
              1, NULL, 0);
    CHECK(len > 0);
    CHECK(!tn_frame_decode(buf, len, &d));
}

/* ------------------------------------------------------------------------- */
/* State protection against corrupted / malformed frames                      */
/* ------------------------------------------------------------------------- */

static void test_corrupt_frame_leaves_state(void)
{
    sim_t *s = make_net();
    treenet_t *t = sim_find(s, 2)->net;

    state_t before = snapshot(t);

    /* Inject corrupted frames from an unknown source; a corrupted frame must
     * be dropped before it can create a neighbour or touch routing state. */
    uint8_t payload[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    for (unsigned i = 0; i < 200; i++) {
        uint8_t buf[TREENET_MTU];
        size_t len = enc(buf, sizeof(buf), (uint8_t)(i % 6u), 77, 2, 1,
                         payload, sizeof(payload));
        CHECK(len > 0);
        /* Corrupt one byte (the CRC will no longer match). */
        buf[i % len] ^= 0xFF;
        inject(s, 2, buf, len);
    }

    state_t after = snapshot(t);
    check_state_equal(&before, &after);

    sim_destroy(s);
}

static void test_wrong_netid_ignored(void)
{
    sim_t *s = make_net();
    treenet_t *t = sim_find(s, 2)->net;
    state_t before = snapshot(t);

    uint8_t payload[4] = { 1, 2, 3, 4 };
    for (unsigned i = 0; i < 20; i++) {
        uint8_t buf[TREENET_MTU];
        size_t len = enc(buf, sizeof(buf), TREENET_FRAME_DATA, 55, 2,
                         /*net_id*/ 2, payload, sizeof(payload));
        CHECK(len > 0);
        inject(s, 2, buf, len);
    }

    state_t after = snapshot(t);
    check_state_equal(&before, &after);

    sim_destroy(s);
}

static void test_malformed_beacon_payload(void)
{
    sim_t *s = make_net();
    treenet_t *t = sim_find(s, 2)->net;
    state_t before = snapshot(t);
    uint32_t dropped_before = t->stats.frames_dropped;

    /* A structurally valid BEACON frame whose payload is too short must be
     * dropped without touching the routing decision. */
    uint8_t short_pl[3] = { 0, 0, 0 };
    for (unsigned i = 0; i < 10; i++) {
        uint8_t buf[TREENET_MTU];
        size_t len = enc(buf, sizeof(buf), TREENET_FRAME_BEACON, 1, 2, 1,
                         short_pl, sizeof(short_pl));
        CHECK(len > 0);
        inject(s, 2, buf, len);
    }

    state_t after = snapshot(t);
    check_state_equal(&before, &after);
    CHECK(t->stats.frames_dropped > dropped_before);

    sim_destroy(s);
}

static void test_malformed_beacon_semantics(void)
{
    sim_t *s = make_net();
    treenet_t *t = sim_find(s, 2)->net;
    state_t before = snapshot(t);

    /* A full length beacon with an impossible advertised interval must be
     * rejected: it could otherwise poison the delivery-ratio estimate. */
    uint8_t pl[TN_BEACON_PAYLOAD_LEN];
    tn_beacon_payload_t b;
    b.rank = 100;
    b.parent = 1;
    b.flags = TN_BEACON_HAS_PARENT;
    b.interval_100ms = 0xFFFFu; /* ~1.8 hours: absurd */
    tn_beacon_encode(pl, &b);

    uint8_t buf[TREENET_MTU];
    size_t len = enc(buf, sizeof(buf), TREENET_FRAME_BEACON, 1, 2, 1, pl,
                     sizeof(pl));
    CHECK(len > 0);
    inject(s, 2, buf, len);

    state_t after = snapshot(t);
    check_state_equal(&before, &after);

    sim_destroy(s);
}

static void test_malformed_dao(void)
{
    sim_t *s = make_net();
    treenet_t *t = sim_find(s, 2)->net;
    state_t before = snapshot(t);

    /* DAO claiming more hops than the protocol allows must be ignored. It is
     * sent by the existing master neighbour so the only thing that could
     * change is the route table. */
    uint8_t pl[TN_DAO_PAYLOAD_LEN];
    tn_dao_payload_t d;
    d.origin = 99;
    d.hops = 200; /* > TREENET_MAX_HOPS */
    tn_dao_encode(pl, &d);

    uint8_t buf[TREENET_MTU];
    size_t len = enc(buf, sizeof(buf), TREENET_FRAME_DAO, 1, 2, 1, pl,
                     sizeof(pl));
    CHECK(len > 0);
    inject(s, 2, buf, len);

    state_t after = snapshot(t);
    check_state_equal(&before, &after);

    sim_destroy(s);
}

/** Encode a fragmented DATA frame. */
static size_t enc_frag(uint8_t *buf, size_t cap, treenet_addr_t src,
                       treenet_addr_t dst, uint16_t seq, uint16_t dgram_id,
                       uint8_t index, uint8_t count, const uint8_t *chunk,
                       size_t chunk_len)
{
    uint8_t body[TN_FRAG_HDR_LEN + TREENET_MTU];
    tn_frag_hdr_t fh;
    fh.dgram_id = dgram_id;
    fh.index = index;
    fh.count = count;
    tn_frag_encode(body, &fh);
    if (chunk_len > 0 && chunk != NULL) {
        memcpy(body + TN_FRAG_HDR_LEN, chunk, chunk_len);
    }

    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DATA;
    f.flags = TN_FLAG_FRAG;
    f.src = src;
    f.dst = dst;
    f.seq = seq;
    f.net_id = 1;
    f.hop_limit = 8;
    f.prev = src;
    f.payload = body;
    f.payload_len = TN_FRAG_HDR_LEN + chunk_len;
    return tn_frame_encode(buf, cap, &f);
}

static void test_malformed_fragments(void)
{
    sim_t *s = make_net();
    uint32_t delivered_before = sim_find(s, 2)->datagrams_rx;

    uint8_t buf[TREENET_MTU];
    uint8_t full[TEST_FRAG_CHUNK];
    memset(full, 0x5A, sizeof(full));

    /* count == 0 */
    size_t len = enc_frag(buf, sizeof(buf), 1, 2, 1, 1, 0, 0, full, 4);
    CHECK(len > 0);
    inject(s, 2, buf, len);

    /* index >= count */
    len = enc_frag(buf, sizeof(buf), 1, 2, 2, 2, 2, 2, full, 4);
    CHECK(len > 0);
    inject(s, 2, buf, len);

    /* Non-last fragment that is not full size (would leave a hole). */
    len = enc_frag(buf, sizeof(buf), 1, 2, 3, 3, 0, 3, full, 3);
    CHECK(len > 0);
    inject(s, 2, buf, len);

    /* Inconsistent total count for the same datagram: first a valid full
     * fragment, then one claiming a different count. */
    len = enc_frag(buf, sizeof(buf), 1, 2, 4, 4, 0, 2, full, sizeof(full));
    CHECK(len > 0);
    inject(s, 2, buf, len);
    len = enc_frag(buf, sizeof(buf), 1, 2, 5, 4, 1, 3, full, 4);
    CHECK(len > 0);
    inject(s, 2, buf, len);

    /* None of the malformed fragments may have produced a datagram. */
    CHECK_EQ(sim_find(s, 2)->datagrams_rx, delivered_before);

    sim_destroy(s);
}

void run_corrupt_tests(void)
{
#if TREENET_ENABLE_FRAME_CRC
    RUN_TEST(test_crc_detects_bitflip);
    RUN_TEST(test_crc_detects_truncation);
#endif
    RUN_TEST(test_wrong_version_rejected);
    RUN_TEST(test_invalid_addresses_rejected);
    RUN_TEST(test_corrupt_frame_leaves_state);
    RUN_TEST(test_wrong_netid_ignored);
    RUN_TEST(test_malformed_beacon_payload);
    RUN_TEST(test_malformed_beacon_semantics);
    RUN_TEST(test_malformed_dao);
    RUN_TEST(test_malformed_fragments);
}
