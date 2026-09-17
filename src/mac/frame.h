/**
 * @file frame.h
 * @brief Wire format of a treenity frame: encoding, decoding and the small
 *        payload structures carried inside control frames.
 *
 * Every frame starts with a fixed 16 byte header transmitted as raw bytes (not
 * serialised through a schema) to keep time-on-air minimal. The header is
 * deliberately compact: a 4 bit protocol version, the frame type, flags, the
 * source and destination addresses, a sequence number for duplicate detection,
 * the network id and a hop limit.
 *
 * Layout (little-endian multi-byte fields):
 * @verbatim
 *   offset size field
 *   0      1    version(hi nibble) | type(lo nibble)
 *   1      1    flags
 *   2      4    src
 *   6      4    dst
 *   10     2    seq
 *   12     2    net_id
 *   14     1    hop_limit
 *   15     1    sec        (security hook / reserved)
 *   16     4    prev       (address of the immediate transmitter)
 *   20     N    payload
 * @endverbatim
 *
 * The @c prev field is rewritten by the MAC on every transmission (see
 * tn_tx_submit). It lets a receiver address a hop-by-hop ACK and lets routing
 * install a downward route through the node a frame actually came from, while
 * @c src keeps pointing at the original sender.
 */
#ifndef TREENET_FRAME_H
#define TREENET_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"
#include "treenet/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed header length in bytes. */
#define TN_FRAME_HDR 20u

/** Byte offset of the "previous hop" field inside the header. */
#define TN_FRAME_OFF_PREV 16u

/**
 * Trailer length: the CRC-16 appended after the payload when integrity
 * checking is enabled (see TREENET_ENABLE_FRAME_CRC).
 */
#if TREENET_ENABLE_FRAME_CRC
#define TN_FRAME_TRAILER 2u
#else
#define TN_FRAME_TRAILER 0u
#endif

/** Total per-frame overhead: header plus optional CRC trailer. */
#define TN_FRAME_OVERHEAD (TN_FRAME_HDR + TN_FRAME_TRAILER)

/* Header flag bits. */
#define TN_FLAG_WANT_ACK 0x01u /**< sender requests a hop-by-hop ACK */
#define TN_FLAG_FLOOD    0x02u /**< frame is being flooded (broadcast) */
#define TN_FLAG_FRAG     0x04u /**< payload begins with a fragment header */
#define TN_FLAG_SEC      0x08u /**< security hook: payload is protected */

/** Decoded view of a frame. Payload points into the caller's buffer. */
typedef struct {
    uint8_t        version;     /**< protocol version */
    uint8_t        type;        /**< @ref treenet_frame_type_t */
    uint8_t        flags;       /**< TN_FLAG_* bit mask */
    treenet_addr_t src;         /**< original sender */
    treenet_addr_t dst;         /**< final destination or broadcast */
    uint16_t       seq;         /**< originator sequence number */
    uint16_t       net_id;      /**< logical network id */
    uint8_t        hop_limit;   /**< remaining hops */
    uint8_t        sec;         /**< security hook byte */
    treenet_addr_t prev;        /**< address of the immediate transmitter */
    const uint8_t *payload;     /**< pointer into the input buffer */
    size_t         payload_len; /**< payload length in bytes */
} tn_frame_t;

/* ------------------------------------------------------------------------- */
/* Beacon payload (link-local hello)                                          */
/* ------------------------------------------------------------------------- */

/**
 * Beacon payload. Beacons are never forwarded; they let neighbours learn each
 * other's rank, parent and beacon interval so the DODAG can form and link
 * quality (packet delivery ratio) can be estimated accurately.
 *
 * @c interval_100ms carries the sender's current beacon interval in units of
 * 100 ms. Advertising it lets a receiver know exactly how many beacons to
 * expect between two arrivals, so a slowly growing Trickle interval does not
 * masquerade as packet loss.
 */
typedef struct {
    uint16_t       rank;          /**< sender's accumulated rank */
    treenet_addr_t parent;        /**< sender's current parent, or INVALID */
    uint8_t        flags;         /**< bit0: sender offers a path to Master */
    uint16_t       interval_100ms;/**< beacon interval in 100 ms units */
} tn_beacon_payload_t;

/** Serialised size of a beacon payload. */
#define TN_BEACON_PAYLOAD_LEN 9u

/** Beacon flag: the sender currently has a usable parent. */
#define TN_BEACON_HAS_PARENT 0x01u

/* ------------------------------------------------------------------------- */
/* Fragment header                                                            */
/* ------------------------------------------------------------------------- */

/** Header prepended to the payload of a fragmented datagram. */
typedef struct {
    uint16_t dgram_id; /**< datagram identifier (origin seq) */
    uint8_t  index;    /**< fragment index, 0 based */
    uint8_t  count;    /**< total number of fragments */
} tn_frag_hdr_t;

/** Serialised size of a fragment header. */
#define TN_FRAG_HDR_LEN 4u

/* ------------------------------------------------------------------------- */
/* DAO payload (destination advertisement)                                    */
/* ------------------------------------------------------------------------- */

/**
 * Destination advertisement. A node sends a DAO towards the Master whenever it
 * (re)joins or changes parent; every node on the upward path installs a route
 * back to @ref origin through the immediate sender, which builds the downward
 * routing table (storing mode).
 *
 * The frame source address is rewritten at every hop to the current sender so
 * that the receiver knows its next hop, while @ref origin identifies the node
 * the route is for.
 */
typedef struct {
    treenet_addr_t origin; /**< node the route points to */
    uint8_t        hops;   /**< distance from the origin in hops */
} tn_dao_payload_t;

/** Serialised size of a DAO payload. */
#define TN_DAO_PAYLOAD_LEN 5u

/**
 * @brief Compute the frame integrity check.
 *
 * CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF, no reflection,
 * no final XOR). Exposed so tests and ports can verify the algorithm.
 *
 * @param data bytes to cover (header plus payload, excluding the trailer)
 * @param len  number of bytes
 * @return 16 bit CRC
 */
uint16_t tn_frame_crc(const uint8_t *data, size_t len);

/* ------------------------------------------------------------------------- */
/* Encoding / decoding                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Encode a frame into @p buf.
 *
 * @param buf   destination buffer
 * @param cap   capacity of @p buf
 * @param frame frame to encode (payload must already be prepared)
 * @return total frame length, or 0 if it does not fit
 */
size_t tn_frame_encode(uint8_t *buf, size_t cap, const tn_frame_t *frame);

/**
 * @brief Decode a frame from @p buf.
 *
 * Performs only structural validation (length, version). Semantic checks such
 * as the network id are done by the caller.
 *
 * @return true when the frame is well formed
 */
bool tn_frame_decode(const uint8_t *buf, size_t len, tn_frame_t *out);

/** @brief Serialise a beacon payload into @p out (needs 7 bytes). */
void tn_beacon_encode(uint8_t *out, const tn_beacon_payload_t *b);

/** @brief Parse a beacon payload. @return true on success. */
bool tn_beacon_decode(const uint8_t *in, size_t len, tn_beacon_payload_t *b);

/** @brief Serialise a fragment header into @p out (needs 4 bytes). */
void tn_frag_encode(uint8_t *out, const tn_frag_hdr_t *f);

/** @brief Parse a fragment header. @return true on success. */
bool tn_frag_decode(const uint8_t *in, size_t len, tn_frag_hdr_t *f);

/** @brief Serialise a DAO payload into @p out (needs 5 bytes). */
void tn_dao_encode(uint8_t *out, const tn_dao_payload_t *d);

/** @brief Parse a DAO payload. @return true on success. */
bool tn_dao_decode(const uint8_t *in, size_t len, tn_dao_payload_t *d);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_FRAME_H */
