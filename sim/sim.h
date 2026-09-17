/**
 * @file sim.h
 * @brief Deterministic network simulator for treenity.
 *
 * The simulator lets the whole mesh run on a desktop without any radio. It
 * models a shared wireless channel:
 *
 *  - nodes are placed on a 2D plane and hear each other according to a
 *    log-distance path-loss model;
 *  - RSSI and SNR are derived from the distance, so link quality estimation
 *    and parent selection behave like they would on real hardware;
 *  - frames transmitted in the same millisecond are resolved for collisions;
 *  - time is virtual and advanced in fixed steps, so a scenario is fully
 *    reproducible from a seed.
 *
 * It is used by the unit/scenario tests and by the desktop example, and is not
 * part of the embedded library.
 */
#ifndef TREENET_SIM_H
#define TREENET_SIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treenet/treenet.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of simulated nodes. */
#define SIM_MAX_NODES 32

/** Maximum frames queued in the virtual channel per millisecond. */
#define SIM_CHANNEL_CAPACITY 512

/** Opaque simulator instance. */
typedef struct sim sim_t;

/** Public per-node information. */
typedef struct {
    treenet_addr_t addr;
    treenet_role_t role;
    double x, y;
    bool active;
    treenet_t *net;
    /* Observation counters filled in by the simulator callbacks. */
    uint32_t datagrams_rx;   /**< application datagrams delivered up */
    uint32_t events;         /**< number of network events observed */
    uint32_t last_src;       /**< source of the last delivered datagram */
    uint32_t last_len;       /**< length of the last delivered datagram */
    int16_t  last_rssi;      /**< RSSI of the last delivered datagram */
    int8_t   last_snr;       /**< SNR of the last delivered datagram */
    uint32_t last_timer_arm; /**< last delay requested via port.timer_arm */
    uint32_t max_timer_arm;  /**< largest delay ever requested via timer_arm */
    uint32_t timer_arm_count;/**< number of port.timer_arm calls */
} sim_node_t;

/**
 * @brief Create a simulator.
 *
 * @param seed           PRNG seed for reproducibility
 * @param tx_power_dbm   transmit power of every node
 * @param sensitivity_dbm receiver sensitivity (frames below this are lost)
 * @param noise_floor_dbm thermal noise floor used to compute SNR
 */
sim_t *sim_create(uint32_t seed, double tx_power_dbm, double sensitivity_dbm,
                  double noise_floor_dbm);

/** @brief Destroy the simulator and all nodes. */
void sim_destroy(sim_t *s);

/**
 * @brief Add a node to the simulation.
 *
 * @param addr  unique address (must be non-zero)
 * @param role  node role
 * @param x,y   initial position in metres
 * @param reliable request hop-by-hop ACKs for this node's sends
 * @return pointer to the node, or NULL if the simulator is full
 */
sim_node_t *sim_add_node(sim_t *s, treenet_addr_t addr, treenet_role_t role,
                         double x, double y, bool reliable);

/** @brief Move an existing node (models mobility). */
void sim_move_node(sim_t *s, treenet_addr_t addr, double x, double y);

/** @brief Mark a node as failed (radio off) or bring it back. */
void sim_set_active(sim_t *s, treenet_addr_t addr, bool active);

/** @brief Find a node by address, or NULL. */
sim_node_t *sim_find(sim_t *s, treenet_addr_t addr);

/** @return number of nodes. */
size_t sim_node_count(const sim_t *s);

/** @return the node at index @p i, or NULL. */
sim_node_t *sim_node_at(const sim_t *s, size_t i);

/**
 * @brief Optional observation hooks invoked in addition to the built-in
 *        counters. Useful for examples and debugging.
 *
 * @param recv  called whenever a datagram is delivered up to a node
 * @param event called whenever a node reports a network event
 */
typedef void (*sim_recv_cb)(sim_node_t *node, treenet_addr_t src,
                            const uint8_t *data, size_t len, int16_t rssi,
                            int8_t snr, uint8_t hops);
typedef void (*sim_event_cb)(sim_node_t *node, treenet_event_t ev);

/** @brief Install observation hooks (either may be NULL). */
void sim_set_callbacks(sim_t *s, sim_recv_cb recv, sim_event_cb event);

/** @return current virtual time in milliseconds. */
uint32_t sim_now(const sim_t *s);

/**
 * @brief Advance virtual time by @p ms milliseconds.
 *
 * On every step all active nodes are polled and the frames queued in the
 * channel are delivered.
 */
void sim_run(sim_t *s, uint32_t ms);

/**
 * @brief Set the physical range scale (path-loss reference at 1 m).
 *
 * Higher values mean a shorter effective range. The default models a typical
 * LoRa link of a few hundred metres to a few kilometres.
 */
void sim_set_path_loss(sim_t *s, double ref_loss_db, double exponent);

/**
 * @brief Set a hard communication range in metres.
 *
 * Frames between nodes farther apart than this are never received. Combined
 * with a mild path-loss model this yields crisp, deterministic topologies for
 * tests (a node is either clearly in range or clearly out of it) while still
 * exercising realistic RSSI/SNR values. Default is effectively unlimited.
 */
void sim_set_range(sim_t *s, double range_m);

/**
 * @brief Set a bit error rate applied to every delivered frame.
 *
 * Before a received frame is handed to treenet_rx(), each of its bits is
 * flipped with probability @p ber. This models a noisy link and lets tests
 * verify that the frame integrity check rejects corrupted frames and that the
 * network keeps working. Default 0.0 (no corruption).
 */
void sim_set_bit_error_rate(sim_t *s, double ber);

/**
 * @brief Set the LoRa parameters reported to nodes added afterwards.
 *
 * Only used for the library's time-on-air estimate; the simulator's collision
 * model is step based and does not depend on it. Defaults to SF9 / 125 kHz.
 */
void sim_set_radio(sim_t *s, uint8_t sf, uint32_t bw_hz);

/**
 * @brief Set the logical network id for nodes added afterwards.
 *
 * Lets one simulation host several co-located networks (different net_id):
 * they share the channel physically but the library ignores frames from other
 * networks. Defaults to 1.
 */
void sim_set_net_id(sim_t *s, uint16_t net_id);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_SIM_H */
