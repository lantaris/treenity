/**
 * @file ringbuf.h
 * @brief Lock-free single-producer / single-consumer byte ring buffer used to
 *        move frames from the receive callback (modem ISR) into the
 *        cooperative poll loop.
 *
 * Concurrency contract
 * --------------------
 *  - exactly ONE producer: the modem receive path calling tn_ringbuf_push()
 *    (typically from an interrupt);
 *  - exactly ONE consumer: the poll loop calling tn_ringbuf_pop().
 *
 * No critical section is needed: the producer owns the `tail` index and the
 * consumer owns the `head` index, and each only reads the other's index. The
 * indices are monotonic 32 bit positions (wrap-safe); the buffer index is
 * `position & (capacity - 1)`, so the capacity MUST be a power of two.
 *
 * The design relies on word-sized index loads/stores being atomic, which holds
 * on the 32 bit microcontrollers treenity targets (enforced by a compile-time
 * check in config.h).
 */
#ifndef TREENET_RINGBUF_H
#define TREENET_RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Metadata attached to every queued frame. */
typedef struct {
    uint16_t len;      /**< frame length in bytes */
    int16_t  rssi_dbm; /**< RSSI of the received frame */
    int8_t   snr_db;   /**< SNR of the received frame */
} tn_rx_meta_t;

/**
 * Single-producer / single-consumer ring buffer.
 *
 * `head` and `tail` are monotonic positions; `tail - head` is the number of
 * bytes currently stored. Only the producer writes `tail` and `dropped`; only
 * the consumer writes `head`.
 */
typedef struct {
    uint8_t *buf;             /**< storage owned by the caller */
    uint32_t capacity;        /**< storage size in bytes, a power of two */
    volatile uint32_t head;   /**< consumer position */
    volatile uint32_t tail;   /**< producer position */
    volatile uint32_t dropped;/**< records dropped by the producer (buffer full) */
} tn_ringbuf_t;

/** @brief Initialise a ring buffer over caller provided storage. */
void tn_ringbuf_init(tn_ringbuf_t *rb, uint8_t *storage, size_t capacity);

/**
 * @brief Push one framed record (metadata + payload).
 *
 * The record layout is: [meta][payload bytes]. Producer side only. Returns
 * false without blocking when there is not enough space, so the caller (often
 * an ISR) never blocks.
 *
 * @return true on success
 */
bool tn_ringbuf_push(tn_ringbuf_t *rb, const tn_rx_meta_t *meta,
                     const uint8_t *data);

/**
 * @brief Pop the oldest record.
 *
 * Consumer side only.
 *
 * @param meta  receives the metadata
 * @param out   destination for the payload
 * @param max   capacity of @p out
 * @return true if a record was popped, false when the buffer is empty
 */
bool tn_ringbuf_pop(tn_ringbuf_t *rb, tn_rx_meta_t *meta, uint8_t *out,
                    size_t max);

/** @return true when no records are pending. */
static inline bool tn_ringbuf_empty(const tn_ringbuf_t *rb)
{
    return rb->head == rb->tail;
}

/** @return number of bytes currently stored. */
static inline size_t tn_ringbuf_used(const tn_ringbuf_t *rb)
{
    return (size_t)(rb->tail - rb->head);
}

#ifdef __cplusplus
}
#endif

#endif /* TREENET_RINGBUF_H */
