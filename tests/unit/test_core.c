/**
 * @file test_core.c
 * @brief Unit tests for the freestanding building blocks: time arithmetic,
 *        ring buffer, EWMA, timers, duplicate cache, frame codec and airtime.
 */
#include "test.h"

#include "core/util.h"
#include "core/ringbuf.h"
#include "core/ewma.h"
#include "core/timer.h"
#include "mac/frame.h"
#include "mac/dupcache.h"
#include "mac/airtime.h"
#include "treenet/treenet.h"

/* ------------------------------------------------------------------------- */

static void test_time_wrap(void)
{
    /* Time comparisons must be correct across the 32 bit wrap. */
    uint32_t a = 0xFFFFFFF0u;
    uint32_t b = 0x00000010u; /* 32 ms after a */
    CHECK(tn_time_after(b, a));
    CHECK(!tn_time_after(a, b));
    CHECK_EQ(tn_elapsed(a, b), 32u);
}

static void test_clamp(void)
{
    CHECK_EQ(tn_clamp_i32(5, 0, 10), 5);
    CHECK_EQ(tn_clamp_i32(-3, 0, 10), 0);
    CHECK_EQ(tn_clamp_i32(42, 0, 10), 10);
}

static void test_snr_score(void)
{
    /* Better SNR must produce a lower (better) cost score. */
    CHECK(tn_snr_score_q8(10) < tn_snr_score_q8(0));
    CHECK(tn_snr_score_q8(0) < tn_snr_score_q8(-10));
    CHECK_EQ(tn_snr_score_q8(-100), tn_snr_score_q8(-20));
}

static void test_ewma(void)
{
    tn_ewma16_t e;
    tn_ewma16_reset(&e);
    CHECK_EQ(tn_ewma16_push(&e, 100), 100); /* first sample adopted */
    (void)tn_ewma16_push(&e, 0);
    int32_t v = tn_ewma16_get(&e);
    CHECK(v < 100);
    CHECK(v > 0);
}

static void test_ringbuf(void)
{
    uint8_t storage[64];
    tn_ringbuf_t rb;
    tn_ringbuf_init(&rb, storage, sizeof(storage));

    tn_rx_meta_t m = { 4, -80, 7 };
    uint8_t payload[4] = { 1, 2, 3, 4 };
    CHECK(tn_ringbuf_push(&rb, &m, payload));

    tn_rx_meta_t got;
    uint8_t out[16];
    CHECK(tn_ringbuf_pop(&rb, &got, out, sizeof(out)));
    CHECK_EQ(got.len, 4);
    CHECK_EQ(got.rssi_dbm, -80);
    CHECK_EQ(got.snr_db, 7);
    CHECK_EQ(memcmp(out, payload, 4), 0);
    CHECK(tn_ringbuf_empty(&rb));
}

static void test_ringbuf_overflow(void)
{
    uint8_t storage[16];
    tn_ringbuf_t rb;
    tn_ringbuf_init(&rb, storage, sizeof(storage));
    tn_rx_meta_t m = { 8, 0, 0 };
    uint8_t payload[8] = { 0 };
    /* Each record needs 5 + 8 = 13 bytes, so only one fits in 16 bytes. */
    CHECK(tn_ringbuf_push(&rb, &m, payload));
    CHECK(!tn_ringbuf_push(&rb, &m, payload));
    CHECK_EQ(rb.dropped, 1u);
}

static void test_timer(void)
{
    tn_timer_t tm;
    tn_timer_start(&tm, 1000, 500);
    CHECK(!tn_timer_expired(&tm, 1400));
    CHECK(tn_timer_expired(&tm, 1500));
    CHECK(tn_timer_fire(&tm, 1500));
    CHECK(!tm.active); /* one-shot */
}

static void test_timer_periodic(void)
{
    tn_timer_t tm;
    tn_timer_start_periodic(&tm, 0, 100);
    CHECK(tn_timer_fire(&tm, 100));
    CHECK(tm.active);
    CHECK(!tn_timer_fire(&tm, 150));
    CHECK(tn_timer_fire(&tm, 200));
}

static void test_dupcache(void)
{
    tn_dupcache_t c;
    tn_dupcache_init(&c, 1000);
    CHECK(!tn_dupcache_seen(&c, 42, 7, 0));
    CHECK(tn_dupcache_seen(&c, 42, 7, 10));
    CHECK(!tn_dupcache_seen(&c, 42, 8, 10));
    /* After the TTL the entry is forgotten. */
    CHECK(!tn_dupcache_seen(&c, 42, 7, 2000));
}

static void test_dupcache_check_mark(void)
{
    tn_dupcache_t c;
    tn_dupcache_init(&c, 1000);

    /* check() must not record the frame, so a dropped forward can retry. */
    CHECK(!tn_dupcache_check(&c, 5, 1, 0));
    CHECK(!tn_dupcache_check(&c, 5, 1, 0));

    tn_dupcache_mark(&c, 5, 1, 0);
    CHECK(tn_dupcache_check(&c, 5, 1, 10));
    CHECK(tn_dupcache_seen(&c, 5, 1, 10)); /* still fresh */

    /* After the TTL the entry is forgotten. */
    CHECK(!tn_dupcache_check(&c, 5, 1, 2000));
}

static void test_frame_roundtrip(void)
{
    uint8_t payload[3] = { 0xAA, 0xBB, 0xCC };
    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DATA;
    f.flags = TN_FLAG_WANT_ACK;
    f.src = 0x11223344u;
    f.dst = 0x55667788u;
    f.seq = 0x1234u;
    f.net_id = 7;
    f.hop_limit = 5;
    f.prev = 0xDEADBEEFu;
    f.payload = payload;
    f.payload_len = sizeof(payload);

    uint8_t buf[TREENET_MTU];
    size_t len = tn_frame_encode(buf, sizeof(buf), &f);
    CHECK_EQ(len, TN_FRAME_OVERHEAD + 3);

    tn_frame_t d;
    CHECK(tn_frame_decode(buf, len, &d));
    CHECK_EQ(d.type, TREENET_FRAME_DATA);
    CHECK_EQ(d.src, 0x11223344u);
    CHECK_EQ(d.dst, 0x55667788u);
    CHECK_EQ(d.seq, 0x1234u);
    CHECK_EQ(d.net_id, 7);
    CHECK_EQ(d.hop_limit, 5);
    CHECK_EQ(d.prev, 0xDEADBEEFu);
    CHECK_EQ(d.payload_len, 3);
    CHECK_EQ(memcmp(d.payload, payload, 3), 0);
}

static void test_frame_reject_short(void)
{
    uint8_t buf[4] = { 0 };
    tn_frame_t d;
    CHECK(!tn_frame_decode(buf, sizeof(buf), &d));
}

static void test_frame_encode_null_payload(void)
{
    /* A declared payload without bytes must be rejected, not encoded. */
    tn_frame_t f;
    memset(&f, 0, sizeof(f));
    f.version = TREENET_PROTOCOL_VERSION;
    f.type = TREENET_FRAME_DATA;
    f.src = 1;
    f.dst = 2;
    f.payload = NULL;
    f.payload_len = 4;
    uint8_t buf[TREENET_MTU];
    CHECK_EQ(tn_frame_encode(buf, sizeof(buf), &f), 0u);
}

static void test_frame_crc_vector(void)
{
    /* Standard check value for CRC-16/CCITT-FALSE over "123456789". */
    const uint8_t *s = (const uint8_t *)"123456789";
    CHECK_EQ(tn_frame_crc(s, 9), 0x29B1u);
}

static void test_beacon_roundtrip(void)
{
    tn_beacon_payload_t b = { 512, 0x01020304u, TN_BEACON_HAS_PARENT, 50 };
    uint8_t buf[TN_BEACON_PAYLOAD_LEN];
    tn_beacon_encode(buf, &b);

    tn_beacon_payload_t d;
    CHECK(tn_beacon_decode(buf, sizeof(buf), &d));
    CHECK_EQ(d.rank, 512);
    CHECK_EQ(d.parent, 0x01020304u);
    CHECK_EQ(d.flags, TN_BEACON_HAS_PARENT);
    CHECK_EQ(d.interval_100ms, 50);
}

static void test_dao_roundtrip(void)
{
    tn_dao_payload_t b = { 0xAABBCCDDu, 3 };
    uint8_t buf[TN_DAO_PAYLOAD_LEN];
    tn_dao_encode(buf, &b);
    tn_dao_payload_t d;
    CHECK(tn_dao_decode(buf, sizeof(buf), &d));
    CHECK_EQ(d.origin, 0xAABBCCDDu);
    CHECK_EQ(d.hops, 3);
}

static void test_airtime_monotonic(void)
{
    tn_lora_params_t p = { 9, 125000, 1, 16, 1, 1 };
    uint32_t small = tn_lora_toa_us(&p, 10);
    uint32_t large = tn_lora_toa_us(&p, 100);
    CHECK(small > 0);
    CHECK(large > small);
    /* A 100 byte SF9/125k frame is on the order of a couple hundred ms. */
    CHECK(large > 100000u);
    CHECK(large < 600000u);
}

static void test_context_size(void)
{
    /* The context holds all fixed tables; the examples size their buffer for
     * the default configuration, so guard against an accidental shrink. */
    CHECK(treenet_context_size() >= 8192u);
}

static void test_airtime_invalid_params(void)
{
    /* Out-of-range LoRa parameters must not trigger an undefined shift; the
     * estimate falls back to the generic linear model. */
    tn_lora_params_t bad = { 200, 125000, 1, 16, 1, 1 };
    CHECK(tn_lora_toa_us(&bad, 100) > 0);
    CHECK(tn_airtime_estimate_us(&bad, 100) > 0);

    tn_lora_params_t zero = { 0, 0, 0, 0, 0, 0 };
    CHECK(tn_lora_toa_us(&zero, 100) > 0);
    CHECK(tn_airtime_estimate_us(&zero, 100) > 0);
}

void run_core_tests(void)
{
    RUN_TEST(test_time_wrap);
    RUN_TEST(test_clamp);
    RUN_TEST(test_snr_score);
    RUN_TEST(test_ewma);
    RUN_TEST(test_ringbuf);
    RUN_TEST(test_ringbuf_overflow);
    RUN_TEST(test_timer);
    RUN_TEST(test_timer_periodic);
    RUN_TEST(test_dupcache);
    RUN_TEST(test_dupcache_check_mark);
    RUN_TEST(test_frame_roundtrip);
    RUN_TEST(test_frame_reject_short);
    RUN_TEST(test_frame_encode_null_payload);
    RUN_TEST(test_frame_crc_vector);
    RUN_TEST(test_beacon_roundtrip);
    RUN_TEST(test_dao_roundtrip);
    RUN_TEST(test_airtime_monotonic);
    RUN_TEST(test_airtime_invalid_params);
    RUN_TEST(test_context_size);
}
