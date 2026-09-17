/**
 * @file ringbuf.c
 * @brief Implementation of the fixed-capacity receive ring buffer.
 */
#include "ringbuf.h"

#include <string.h>

#include "util.h"

/** Size of the serialised record header: len(2) + rssi(2) + snr(1). */
#define TN_RX_HDR 5u

void tn_ringbuf_init(tn_ringbuf_t *rb, uint8_t *storage, size_t capacity)
{
    rb->buf = storage;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->used = 0;
    rb->dropped = 0;
}

/* Write @p n bytes into the ring at the tail, without wrap handling beyond the
 * modulo index arithmetic (capacity is small, memcpy is split if needed). */
static void ring_write(tn_ringbuf_t *rb, const uint8_t *src, size_t n)
{
    size_t first = rb->capacity - rb->tail;
    if (first > n) first = n;
    memcpy(rb->buf + rb->tail, src, first);
    if (n > first) {
        memcpy(rb->buf, src + first, n - first);
    }
    rb->tail = (rb->tail + n) % rb->capacity;
    rb->used += n;
}

/* Read @p n bytes from the ring at the head. */
static void ring_read(tn_ringbuf_t *rb, uint8_t *dst, size_t n)
{
    size_t first = rb->capacity - rb->head;
    if (first > n) first = n;
    memcpy(dst, rb->buf + rb->head, first);
    if (n > first) {
        memcpy(dst + first, rb->buf, n - first);
    }
    rb->head = (rb->head + n) % rb->capacity;
    rb->used -= n;
}

bool tn_ringbuf_push(tn_ringbuf_t *rb, const tn_rx_meta_t *meta,
                     const uint8_t *data)
{
    size_t need = TN_RX_HDR + (size_t)meta->len;

    if (rb->capacity - rb->used < need) {
        rb->dropped++;
        return false;
    }

    uint8_t hdr[TN_RX_HDR];
    tn_put_u16(&hdr[0], meta->len);
    tn_put_u16(&hdr[2], (uint16_t)meta->rssi_dbm);
    hdr[4] = (uint8_t)meta->snr_db;

    ring_write(rb, hdr, TN_RX_HDR);
    ring_write(rb, data, meta->len);
    return true;
}

bool tn_ringbuf_pop(tn_ringbuf_t *rb, tn_rx_meta_t *meta, uint8_t *out,
                    size_t max)
{
    if (rb->used < TN_RX_HDR) {
        return false;
    }

    uint8_t hdr[TN_RX_HDR];
    ring_read(rb, hdr, TN_RX_HDR);

    meta->len = tn_get_u16(&hdr[0]);
    meta->rssi_dbm = (int16_t)tn_get_u16(&hdr[2]);
    meta->snr_db = (int8_t)hdr[4];

    if ((size_t)meta->len > max) {
        /* Defensive: corrupted record. Drop the remainder and bail out. */
        size_t skip = TN_MIN((size_t)meta->len, rb->used);
        uint8_t scratch[1];
        for (size_t i = 0; i < skip; i++) {
            ring_read(rb, scratch, 1);
        }
        rb->dropped++;
        return false;
    }

    if (rb->used < meta->len) {
        /* Truncated record; treat as corruption. */
        rb->head = rb->tail;
        rb->used = 0;
        rb->dropped++;
        return false;
    }

    ring_read(rb, out, meta->len);
    return true;
}
