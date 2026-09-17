/**
 * @file test_ringbuf_threads.c
 * @brief Concurrency stress test for the lock-free receive ring buffer.
 *
 * One producer thread pushes records carrying a sequence number; one consumer
 * thread pops them. Every record must arrive intact and in order, proving the
 * single-producer/single-consumer ring has no lost updates on its indices.
 */
#include <stdio.h>
#include <string.h>

#include "core/ringbuf.h"
#include "thread.h"

#define STRESS_RECORDS 1000000u

static tn_ringbuf_t g_rb;
static uint8_t      g_storage[TREENET_RX_RING_BYTES];

static uint32_t g_pushed;
static uint32_t g_received;
static uint32_t g_bad;

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < STRESS_RECORDS; i++) {
        uint8_t payload[4];
        payload[0] = (uint8_t)(i);
        payload[1] = (uint8_t)(i >> 8);
        payload[2] = (uint8_t)(i >> 16);
        payload[3] = (uint8_t)(i >> 24);

        tn_rx_meta_t m;
        m.len = sizeof(payload);
        m.rssi_dbm = (int16_t)(i & 0x7FFFu);
        m.snr_db = (int8_t)(i & 0x7F);

        /* Retry on a full buffer: every record must eventually be delivered,
         * which makes the consumer's sequence check exact. */
        while (!tn_ringbuf_push(&g_rb, &m, payload)) {
            /* spin until the consumer drains some space */
        }
        g_pushed++;
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    uint32_t expected = 0;
    while (expected < STRESS_RECORDS) {
        tn_rx_meta_t m;
        uint8_t out[16];
        if (!tn_ringbuf_pop(&g_rb, &m, out, sizeof(out))) {
            continue; /* spin */
        }
        uint32_t seq = (uint32_t)out[0] | ((uint32_t)out[1] << 8) |
                       ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24);
        if (seq != expected || m.len != 4) {
            g_bad++;
        }
        expected++;
        g_received++;
    }
    return NULL;
}

int run_ringbuf_stress(void)
{
    tn_ringbuf_init(&g_rb, g_storage, sizeof(g_storage));
    g_pushed = g_received = g_bad = 0;

    tn_stress_thread_t pt, ct;
    if (tn_stress_thread_start(&pt, producer, NULL) != 0) return -1;
    if (tn_stress_thread_start(&ct, consumer, NULL) != 0) return -1;
    tn_stress_thread_join(pt);
    tn_stress_thread_join(ct);

    printf("  pushed=%u received=%u out-of-order/corrupt=%u full-retries=%u\n",
           g_pushed, g_received, g_bad, (unsigned)g_rb.dropped);

    if (g_received != STRESS_RECORDS || g_bad != 0 || g_pushed != STRESS_RECORDS) {
        return -1;
    }
    return 0;
}
