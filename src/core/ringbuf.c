/**
 * @file ringbuf.c
 * @brief Lock-free single-producer / single-consumer ring buffer.
 *
 * See ringbuf.h for the concurrency contract. The producer owns `tail`, the
 * consumer owns `head`; each publishes its own index with a compiler barrier
 * after writing/reading the payload so the two never observe a half-written
 * record.
 */
#include "ringbuf.h"

#include <string.h>

#include "util.h"

/** Size of the serialised record header: len(2) + rssi(2) + snr(1). */
#define TN_RX_HDR 5u

/* The wrap-around index arithmetic needs a power-of-two capacity. */
#if (TREENET_RX_RING_BYTES & (TREENET_RX_RING_BYTES - 1u)) != 0u
#error "TREENET_RX_RING_BYTES must be a power of two"
#endif

/** @return true when @p v is a power of two (and non-zero). */
static bool is_pow2(size_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

void tn_ringbuf_init(tn_ringbuf_t *rb, uint8_t *storage, size_t capacity)
{
    rb->buf = storage;
    rb->capacity = is_pow2(capacity) ? (uint32_t)capacity : 0u;
    rb->head = 0;
    rb->tail = 0;
    rb->dropped = 0;
}

/** Write @p n bytes at monotonic position @p pos, splitting at the wrap. */
static void ring_write_at(tn_ringbuf_t *rb, uint32_t pos, const uint8_t *src,
                          uint32_t n)
{
    uint32_t idx = pos & (rb->capacity - 1u);
    uint32_t first = rb->capacity - idx;
    if (first > n) first = n;
    memcpy(rb->buf + idx, src, first);
    if (n > first) {
        memcpy(rb->buf, src + first, n - first);
    }
}

/** Read @p n bytes from monotonic position @p pos, splitting at the wrap. */
static void ring_read_at(const tn_ringbuf_t *rb, uint32_t pos, uint8_t *dst,
                         uint32_t n)
{
    uint32_t idx = pos & (rb->capacity - 1u);
    uint32_t first = rb->capacity - idx;
    if (first > n) first = n;
    memcpy(dst, rb->buf + idx, first);
    if (n > first) {
        memcpy(dst + first, rb->buf, n - first);
    }
}

bool tn_ringbuf_push(tn_ringbuf_t *rb, const tn_rx_meta_t *meta,
                     const uint8_t *data)
{
    if (rb->capacity == 0u) {
        rb->dropped++;
        return false;
    }

    uint32_t tail = rb->tail; /* only the producer writes tail */
    uint32_t head = rb->head; /* snapshot of the consumer position */
    uint32_t need = TN_RX_HDR + (uint32_t)meta->len;

    if (rb->capacity - (tail - head) < need) {
        rb->dropped++;
        return false;
    }

    uint8_t hdr[TN_RX_HDR];
    tn_put_u16(&hdr[0], meta->len);
    tn_put_u16(&hdr[2], (uint16_t)meta->rssi_dbm);
    hdr[4] = (uint8_t)meta->snr_db;

    ring_write_at(rb, tail, hdr, TN_RX_HDR);
    ring_write_at(rb, tail + TN_RX_HDR, data, (uint32_t)meta->len);

    /* Publish the record only after its bytes are in place. */
    TN_PUBLISH();
    rb->tail = tail + need;
    return true;
}

bool tn_ringbuf_pop(tn_ringbuf_t *rb, tn_rx_meta_t *meta, uint8_t *out,
                    size_t max)
{
    if (rb->capacity == 0u) {
        return false;
    }

    uint32_t head = rb->head; /* only the consumer writes head */
    uint32_t tail = rb->tail; /* snapshot of the producer position */
    uint32_t avail = tail - head;

    if (avail < TN_RX_HDR) {
        return false;
    }

    /* Do not read the payload before the snapshot of tail. */
    TN_CONSUME();

    uint8_t hdr[TN_RX_HDR];
    ring_read_at(rb, head, hdr, TN_RX_HDR);
    meta->len = tn_get_u16(&hdr[0]);
    meta->rssi_dbm = (int16_t)tn_get_u16(&hdr[2]);
    meta->snr_db = (int8_t)hdr[4];

    if ((size_t)meta->len > max) {
        /* Defensive: a corrupted record. Drop everything and resynchronise.
         * This never happens with a well behaved producer. */
        rb->head = tail;
        return false;
    }
    if (avail < TN_RX_HDR + (uint32_t)meta->len) {
        /* Record not fully published yet; try again later. */
        return false;
    }

    ring_read_at(rb, head + TN_RX_HDR, out, meta->len);
    TN_BARRIER();
    rb->head = head + TN_RX_HDR + (uint32_t)meta->len;
    return true;
}
