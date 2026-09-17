/**
 * @file config.h
 * @brief Compile-time configuration for the treenity mesh networking library.
 *
 * treenity is designed for deeply embedded targets: no dynamic memory is used
 * anywhere in the core. Every table, queue and buffer has a fixed size that is
 * defined here. A port can override any of these values by defining the macro
 * before including the library headers, or by providing a project wide
 * `treenet_config.h` and defining TREENET_USER_CONFIG.
 *
 * All sizes are chosen to fit a network of ~100..1000 nodes behind a single
 * Master while keeping the RAM footprint of an individual node small.
 */
#ifndef TREENET_CONFIG_H
#define TREENET_CONFIG_H

#ifdef TREENET_USER_CONFIG
#include "treenet_config.h"
#endif

/* ------------------------------------------------------------------------- */
/* Version                                                                    */
/* ------------------------------------------------------------------------- */
#define TREENET_VERSION_MAJOR 0
#define TREENET_VERSION_MINOR 1
#define TREENET_VERSION_PATCH 0

/* ------------------------------------------------------------------------- */
/* Protocol constants                                                         */
/* ------------------------------------------------------------------------- */

/** Protocol version carried in every frame. Receivers drop other versions. */
#ifndef TREENET_PROTOCOL_VERSION
#define TREENET_PROTOCOL_VERSION 1u
#endif

/**
 * Maximum on-air frame size in bytes. The value must not exceed the payload a
 * single modem transaction can carry. For LoRa this is typically 255 bytes but
 * practical presets keep it lower to bound time-on-air (duty cycle!).
 */
#ifndef TREENET_MTU
#define TREENET_MTU 200u
#endif

/** Maximum number of hops a frame may travel. Bounds source-route length. */
#ifndef TREENET_MAX_HOPS
#define TREENET_MAX_HOPS 8u
#endif

/** Maximum number of fragments a single datagram may be split into. */
#ifndef TREENET_MAX_FRAGMENTS
#define TREENET_MAX_FRAGMENTS 8u
#endif

/** Largest application payload accepted by treenet_send(). */
#ifndef TREENET_MAX_DATAGRAM
#define TREENET_MAX_DATAGRAM 1024u
#endif

/* ------------------------------------------------------------------------- */
/* Table sizes                                                                */
/* ------------------------------------------------------------------------- */

/** Neighbour table size. Each entry keeps link quality state. */
#ifndef TREENET_MAX_NEIGHBORS
#define TREENET_MAX_NEIGHBORS 32u
#endif

/** Maximum children a node may serve. Bounds the downward routing table. */
#ifndef TREENET_MAX_CHILDREN
#define TREENET_MAX_CHILDREN 16u
#endif

/** Duplicate-detection cache size (frames in flight window). */
#ifndef TREENET_DUP_CACHE_SIZE
#define TREENET_DUP_CACHE_SIZE 64u
#endif

/**
 * Downward route table size (storing mode). Each node stores a route for every
 * node in its subtree, so a Master of a 1000 node network needs this raised to
 * at least 1024. Ordinary leaves need very few entries.
 */
#ifndef TREENET_MAX_ROUTES
#define TREENET_MAX_ROUTES 128u
#endif

/** Receive ring buffer size in bytes (holds pending frames + metadata). */
#ifndef TREENET_RX_RING_BYTES
#define TREENET_RX_RING_BYTES 1024u
#endif

/** Pending TX (awaiting ACK / scheduled) queue depth. */
#ifndef TREENET_TX_QUEUE_SIZE
#define TREENET_TX_QUEUE_SIZE 8u
#endif

/** Event queue depth delivered to the application. */
#ifndef TREENET_EVENT_QUEUE_SIZE
#define TREENET_EVENT_QUEUE_SIZE 16u
#endif

/** Reassembly buffer count (concurrent inbound fragmented datagrams). */
#ifndef TREENET_REASSEMBLY_SLOTS
#define TREENET_REASSEMBLY_SLOTS 2u
#endif

/* ------------------------------------------------------------------------- */
/* Timing (milliseconds)                                                      */
/* ------------------------------------------------------------------------- */

/** Minimum interval between two beacons from the same node. */
#ifndef TREENET_BEACON_MIN_MS
#define TREENET_BEACON_MIN_MS 5000u
#endif

/**
 * Maximum interval between two beacons (Trickle `I` upper bound).
 *
 * IMPORTANT: the neighbour and parent timeouts below must stay comfortably
 * larger than this value, otherwise a node would consider a perfectly healthy
 * neighbour dead in the silent gap between two beacons. A factor of three is
 * used here.
 */
#ifndef TREENET_BEACON_MAX_MS
#define TREENET_BEACON_MAX_MS 20000u
#endif

/**
 * A neighbour is considered dead after this long without a beacon.
 * Must be > TREENET_BEACON_MAX_MS (see the note above).
 */
#ifndef TREENET_NEIGHBOR_TIMEOUT_MS
#define TREENET_NEIGHBOR_TIMEOUT_MS 90000u
#endif

/**
 * A parent is considered lost after this long without a beacon.
 * Must be > TREENET_BEACON_MAX_MS (see the note above).
 */
#ifndef TREENET_PARENT_TIMEOUT_MS
#define TREENET_PARENT_TIMEOUT_MS 60000u
#endif

/** Minimum time a node keeps its parent before it is allowed to switch. */
#ifndef TREENET_PARENT_DWELL_MS
#define TREENET_PARENT_DWELL_MS 30000u
#endif

/** Channel-access (CSMA/CA) slot time. */
#ifndef TREENET_SLOT_MS
#define TREENET_SLOT_MS 10u
#endif

/** Maximum CSMA/CA contention window (in slots). */
#ifndef TREENET_CW_MAX
#define TREENET_CW_MAX 32u
#endif

/** Time to wait for an ACK before retransmitting. */
#ifndef TREENET_ACK_TIMEOUT_MS
#define TREENET_ACK_TIMEOUT_MS 2000u
#endif

/** Maximum MAC retransmissions for a reliable (want-ack) frame. */
#ifndef TREENET_MAX_RETRIES
#define TREENET_MAX_RETRIES 3u
#endif

/** Interval between periodic route (DAO) refreshes sent towards the Master. */
#ifndef TREENET_ROUTE_REFRESH_MS
#define TREENET_ROUTE_REFRESH_MS 60000u
#endif

/** A downward route entry expires after this long without a refresh. */
#ifndef TREENET_ROUTE_TIMEOUT_MS
#define TREENET_ROUTE_TIMEOUT_MS 180000u
#endif

/** Duplicate-cache entry lifetime. */
#ifndef TREENET_DUP_TTL_MS
#define TREENET_DUP_TTL_MS 60000u
#endif

/**
 * Contention window used when rebroadcasting a flooded frame. Nodes with a
 * weaker (lower SNR) link to the sender pick a shorter delay so the message
 * spreads outwards first; well connected nodes defer and stay silent when they
 * hear the rebroadcast. This is the managed-flooding behaviour popularised by
 * other LoRa meshes.
 */
#ifndef TREENET_FLOOD_CW_MS
#define TREENET_FLOOD_CW_MS 500u
#endif

/* ------------------------------------------------------------------------- */
/* Link-quality / objective-function weights (Q8 fixed point, sum == 256)     */
/* ------------------------------------------------------------------------- */

/** Weight of the SNR component of the link cost. */
#ifndef TREENET_OF_W_SNR
#define TREENET_OF_W_SNR 120u
#endif

/** Weight of the ETX (packet delivery ratio) component. */
#ifndef TREENET_OF_W_ETX
#define TREENET_OF_W_ETX 96u
#endif

/** Weight of the hop-count component. */
#ifndef TREENET_OF_W_HOPS
#define TREENET_OF_W_HOPS 40u
#endif

/**
 * Hysteresis threshold in percent. A candidate parent must beat the current
 * parent's path cost by at least this fraction before the node re-parents.
 * Prevents route flapping in the presence of fading.
 */
#ifndef TREENET_PARENT_HYSTERESIS_PCT
#define TREENET_PARENT_HYSTERESIS_PCT 25u
#endif

/** EWMA smoothing factor numerator/denominator for link metrics (1/8). */
#ifndef TREENET_EWMA_SHIFT
#define TREENET_EWMA_SHIFT 3u
#endif

/* ------------------------------------------------------------------------- */
/* Feature switches                                                           */
/* ------------------------------------------------------------------------- */

/** Include runtime statistics counters. */
#ifndef TREENET_ENABLE_STATS
#define TREENET_ENABLE_STATS 1
#endif

/** Include the human readable logging helper. */
#ifndef TREENET_ENABLE_LOG
#define TREENET_ENABLE_LOG 1
#endif

/** Include the payload fragmentation/reassembly layer. */
#ifndef TREENET_ENABLE_FRAGMENTATION
#define TREENET_ENABLE_FRAGMENTATION 1
#endif

/**
 * Enable the frame integrity check (CRC-16/CCITT-FALSE).
 *
 * When enabled every frame carries a 2 byte trailer covering the whole header
 * (including the rewritten previous-hop field) and the payload. The receiver
 * verifies it before parsing and silently drops any frame that fails. This
 * makes treenity robust against corrupted frames even when the modem does not
 * expose a CRC to the port.
 *
 * IMPORTANT: this setting must be identical on every node of a network. Nodes
 * with different settings cannot interoperate.
 */
#ifndef TREENET_ENABLE_FRAME_CRC
#define TREENET_ENABLE_FRAME_CRC 1
#endif

/**
 * Enable security hooks. When 1 the frame layout reserves a security header
 * field and the port may plug in encryption later. No cryptography is provided
 * by v1 of the library.
 */
#ifndef TREENET_ENABLE_SECURITY_HOOKS
#define TREENET_ENABLE_SECURITY_HOOKS 1
#endif

#endif /* TREENET_CONFIG_H */
