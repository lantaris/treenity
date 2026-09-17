/**
 * @file dupcache.h
 * @brief Duplicate frame suppression.
 *
 * Managed flooding and multi-hop forwarding can deliver the same frame several
 * times. A small fixed hash table keyed by (source, sequence) remembers frames
 * seen recently so they are processed and re-broadcast at most once. This is
 * the "duplicate-ID cache" used by other LoRa meshes and is essential to keep
 * the channel from collapsing under repeated broadcasts.
 */
#ifndef TREENET_DUPCACHE_H
#define TREENET_DUPCACHE_H

#include <stdint.h>
#include <stdbool.h>

#include "treenet/config.h"
#include "treenet/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One cache slot. */
typedef struct {
    treenet_addr_t src;    /**< originator address */
    uint16_t       seq;    /**< originator sequence number */
    uint32_t       seen_ms;/**< when the frame was first seen */
    bool           valid;  /**< slot in use */
} tn_dup_entry_t;

/** Fixed hash table of recently seen frames. */
typedef struct {
    tn_dup_entry_t entries[TREENET_DUP_CACHE_SIZE];
    uint32_t       ttl_ms; /**< how long an entry stays valid */
} tn_dupcache_t;

/** @brief Initialise the cache with a given entry lifetime. */
void tn_dupcache_init(tn_dupcache_t *c, uint32_t ttl_ms);

/**
 * @brief Check whether a frame was seen before and record it.
 *
 * @return true if the frame is a duplicate (already seen and still fresh)
 */
bool tn_dupcache_seen(tn_dupcache_t *c, treenet_addr_t src, uint16_t seq,
                      uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_DUPCACHE_H */
