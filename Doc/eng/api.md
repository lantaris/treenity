# treenity API

[Русский](../ru/api.md) | **English**

Public header: `include/treenet/treenet.h`.
Supporting headers: `include/treenet/types.h`, `include/treenet/port.h`,
`include/treenet/config.h`.

## 1. Lifecycle

### `size_t treenet_context_size(void)`

Size of the context in bytes. Use it to size a static buffer.

### `treenet_t *treenet_init(void *storage, size_t storage_size, const treenet_config_t *cfg, const treenet_port_t *port)`

Initialises an instance in caller-provided storage. Returns `NULL` if:

- `storage == NULL` or `storage_size < treenet_context_size()`;
- `cfg`/`port` == `NULL`;
- the mandatory `port.tx`, `port.now_ms`, `port.rnd` are not set;
- `cfg.addr` is `0` or `0xFFFFFFFF`.

The storage must be aligned to at least 8 bytes and must stay valid for the
whole lifetime of the node.

### `void treenet_poll(treenet_t *t)`

Advances the state machine: receive, beacons, routing, transmission, events.
Non-blocking. Call it from the main loop or a timer (10–100 ms).

### `int treenet_rx(treenet_t *t, const uint8_t *buf, size_t len, int16_t rssi, int8_t snr)`

Hands a received frame to the library. Safe to call from an ISR (it only copies
the frame into a ring buffer). `rssi` (dBm) and `snr` (dB) describe the link to
the immediate transmitter. Returns `0` on success, negative if the buffer is
full or the arguments are invalid.

## 2. Configuration

```c
typedef struct {
    treenet_addr_t addr;      /* unique node address */
    treenet_role_t role;      /* MASTER / NODE / REPEATER */
    uint16_t       net_id;    /* logical network id */

    void (*on_recv)(treenet_t*, treenet_addr_t src, const uint8_t *data,
                    size_t len, int16_t rssi, int8_t snr, uint8_t hops);
    void (*on_event)(treenet_t*, treenet_event_t ev, void *arg);

    void *user;               /* arbitrary pointer, echoed back in callbacks */
    bool  reliable;           /* request hop-by-hop ACK for unicast */
    treenet_radio_cfg_t radio;/* optional: LoRa parameters for time-on-air */
} treenet_config_t;
```

- `on_recv` is called when a complete datagram is received; `rssi`/`snr` are the
  quality of the frame that delivered it (the last hop).
- `on_event` delivers asynchronous events. For `PARENT_CHANGED`, `arg` points to
  the new parent address.
- `radio.spreading_factor != 0` enables time-on-air estimation with the LoRa
  formula.

## 3. Data plane

### `int treenet_send(treenet_t *t, treenet_addr_t dst, const void *data, size_t len)`

Unicast send. Returns:

- `0` — queued;
- `-1` — invalid arguments / not initialised;
- `-2` — datagram larger than `TREENET_MAX_DATAGRAM` with fragmentation disabled;
- `-3` — no route to `dst`.

### `int treenet_broadcast(treenet_t *t, const void *data, size_t len)`

Broadcast via managed flooding.

## 4. Introspection

| Function | Returns |
|---|---|
| `treenet_addr(t)` | node address |
| `treenet_role(t)` | role |
| `treenet_parent(t)` | current parent or `TREENET_ADDR_INVALID` |
| `treenet_rank(t)` | current rank |
| `treenet_is_connected(t)` | `true` when a path to the Master exists |
| `treenet_stats(t)` | pointer to the statistics block |
| `treenet_neighbors(t, out, max)` | neighbour snapshots with link quality |
| `treenet_version()` | version string |

```c
typedef struct {
    treenet_addr_t         addr;
    treenet_link_quality_t lq;      /* rssi_dbm, snr_db, pdr_q8, etx_q8, link_cost */
    uint16_t               rank;
    uint32_t               age_ms;
    bool                   is_parent;
} treenet_neighbor_info_t;
```

## 5. Events (`treenet_event_t`)

| Event | When |
|---|---|
| `NETWORK_READY` | the node joined and has a path to the Master |
| `JOINED` | a parent was chosen for the first time |
| `PARENT_CHANGED` | the parent changed |
| `NEIGHBOR_ADDED` / `NEIGHBOR_REMOVED` | a neighbour appeared / was lost |
| `ROUTE_LOST` | the parent was lost |
| `DISCONNECTED` | the node lost contact with the Master subtree |
| `TX_DONE` / `TX_FAILED` | a reliable frame was acknowledged / exhausted retries |

## 6. Statistics (`treenet_stats_t`)

```c
uint32_t frames_tx, frames_rx, frames_dropped;
uint32_t retransmissions, beacons_tx, beacons_rx;
uint32_t parent_changes, datagrams_tx, datagrams_rx;
uint32_t airtime_ms;
```

## 7. Return codes (summary)

| Code | Meaning |
|---|---|
| `0` | success |
| `-1` | invalid arguments / not initialised |
| `-2` | frame/datagram does not fit |
| `-3` | no route |
