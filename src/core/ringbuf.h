/**
 * @file ringbuf.h
 * @brief Fixed-capacity byte ring buffer used to move frames from the receive
 *        callback (possibly an ISR) into the cooperative poll loop.
 *
 * The buffer stores variable length records: each record is prefixed with a
 * 4 byte header holding the frame length and the RSSI/SNR metadata, so a
 * consumer can pop one complete frame at a time.
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

/** Byte oriented ring buffer with power-of-two-free modular indexing. */
typedef struct {
    uint8_t *buf;      /**< storage owned by the caller */
    size_t   capacity; /**< total storage in bytes */
    size_t   head;     /**< read cursor */
    size_t   tail;     /**< write cursor */
    size_t   used;     /**< bytes currently stored */
    /* statistics */
    uint32_t dropped;  /**< records dropped because the buffer was full */
} tn_ringbuf_t;

/** @brief Initialise a ring buffer over caller provided storage. */
void tn_ringbuf_init(tn_ringbuf_t *rb, uint8_t *storage, size_t capacity);

/**
 * @brief Push one framed record (metadata + payload).
 *
 * The record layout is: [meta_t][payload bytes]. It is a no-op that returns
 * false when there is not enough contiguous space, so the caller (often an
 * ISR) never blocks.
 *
 * @return true on success
 */
bool tn_ringbuf_push(tn_ringbuf_t *rb, const tn_rx_meta_t *meta,
                     const uint8_t *data);

/**
 * @brief Pop the oldest record.
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
    return rb->used == 0;
}

/** @return number of bytes currently stored. */
static inline size_t tn_ringbuf_used(const tn_ringbuf_t *rb)
{
    return rb->used;
}

#ifdef __cplusplus
}
#endif

#endif /* TREENET_RINGBUF_H */
