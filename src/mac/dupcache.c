/**
 * @file dupcache.c
 * @brief Implementation of the duplicate frame cache.
 */
#include "dupcache.h"

#include <string.h>

#include "core/util.h"

void tn_dupcache_init(tn_dupcache_t *c, uint32_t ttl_ms)
{
    memset(c->entries, 0, sizeof(c->entries));
    c->ttl_ms = ttl_ms;
}

/* Deterministic hash over the (src, seq) pair. */
static size_t dup_slot(treenet_addr_t src, uint16_t seq)
{
    uint32_t h = src * 2654435761u; /* Knuth multiplicative hashing */
    h ^= (uint32_t)seq * 40503u;
    return (size_t)(h % TREENET_DUP_CACHE_SIZE);
}

bool tn_dupcache_seen(tn_dupcache_t *c, treenet_addr_t src, uint16_t seq,
                      uint32_t now)
{
    size_t idx = dup_slot(src, seq);
    tn_dup_entry_t *e = &c->entries[idx];

    if (e->valid && e->src == src && e->seq == seq &&
        !tn_time_after(now, e->seen_ms + c->ttl_ms)) {
        return true; /* fresh duplicate */
    }

    /* Insert (or refresh) the entry. */
    e->valid = true;
    e->src = src;
    e->seq = seq;
    e->seen_ms = now;
    return false;
}
