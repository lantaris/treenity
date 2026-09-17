# treenity API

[Русский](../ru/api.md) | **English**

Public header: `include/treenet/treenet.h`.
Supporting headers: `include/treenet/types.h`, `include/treenet/port.h`,
`include/treenet/config.h`.

Conventions:

- Every function takes an instance pointer `treenet_t *` (or its `const` form).
- An address is a `treenet_addr_t` (32 bits): `TREENET_ADDR_BROADCAST`
  (`0xFFFFFFFF`) means everyone, `TREENET_ADDR_INVALID` (`0x00000000`) means
  "no address / no parent".
- The library is non-blocking; all work happens in `treenet_poll()`.

---

## 1. Lifecycle

### `size_t treenet_context_size(void)`

- **Purpose:** return the exact context size in bytes, so you can reserve a
  buffer for `treenet_init`.
- **Parameters:** none.
- **Returns:** size in bytes (depends on the `config.h` settings).
- **Example:** `static uint8_t ctx[16384]; /* >= treenet_context_size() */`
  The default configuration needs about 11.4 KB; the exact value depends on
  `config.h` and is returned by `treenet_context_size()`.

### `treenet_t *treenet_init(void *storage, size_t storage_size, const treenet_config_t *cfg, const treenet_port_t *port)`

- **Purpose:** initialise an instance in caller-provided memory (no `malloc`),
  copy the configuration and port, prepare the tables and timers.
- **Parameters:**
  - `storage` — context buffer; must outlive the node and be aligned to at least
    8 bytes;
  - `storage_size` — buffer size; must be `>= treenet_context_size()`;
  - `cfg` — node configuration (address, role, callbacks, ...);
  - `port` — the port (HAL) function table.
- **Returns:** the instance pointer, or `NULL` if: `storage == NULL`;
  `storage_size` too small; `cfg`/`port == NULL`; the mandatory `port.tx`,
  `port.now_ms`, `port.rnd` are missing; `cfg.addr` is `0` or `0xFFFFFFFF`.
- **Notes:** a Master is set to rank 0 immediately; an ordinary node broadcasts a
  `PROBE` to find a parent.

---

## 2. Runtime

### `void treenet_poll(treenet_t *t)`

- **Purpose:** advance the state machine: parse received frames, beacons,
  routing, deadline-driven maintenance, transmission, event delivery.
  Non-blocking.
- **Parameters:** `t` — the instance (`NULL` or uninitialised is a no-op).
- **Returns:** nothing.
- **Notes:** call it regularly (10–100 ms) **or** implement `port.timer_arm` and
  call it when the timer fires (tickless). Do not call it from an interrupt.

### `int treenet_rx(treenet_t *t, const uint8_t *buf, size_t len, int16_t rssi, int8_t snr)`

- **Purpose:** hand a frame received over the air to the library. Safe to call
  from an ISR (it only copies the frame into a ring buffer).
- **Parameters:**
  - `t` — the instance;
  - `buf` — the frame bytes (as received by the modem);
  - `len` — frame length;
  - `rssi` — frame RSSI in dBm (link to the **immediate** transmitter);
  - `snr` — frame SNR in dB.
- **Returns:** `0` — queued; negative — error (`t`/`buf` `NULL`, not initialised,
  `len == 0`, `len > TREENET_MTU`, buffer full).
- **Notes:** `rssi`/`snr` are essential for link quality estimation and parent
  selection.

---

## 3. Data plane

### `int treenet_send(treenet_t *t, treenet_addr_t dst, const void *data, size_t len)`

- **Purpose:** unicast a datagram to `dst` (routed through the Master tree).
  Datagrams larger than the MTU are fragmented automatically.
- **Parameters:**
  - `t` — the instance;
  - `dst` — destination address (broadcast behaves like `treenet_broadcast`);
  - `data` — application data;
  - `len` — data length (up to `TREENET_MAX_DATAGRAM`).
- **Returns:**
  - `0` — queued;
  - `-1` — invalid arguments / not initialised / `len == 0` /
    `len > TREENET_MAX_DATAGRAM` / `dst` is self or invalid / transmit queue full;
  - `-2` — does not fit and fragmentation is impossible (too many fragments or
    disabled);
  - `-3` — no route to `dst`.
- **Notes:** a datagram larger than the MTU is fragmented; if the transmit queue
  cannot hold **all** fragments the whole send is refused with `-1` (a partially
  queued datagram could never be reassembled).

### `int treenet_broadcast(treenet_t *t, const void *data, size_t len)`

- **Purpose:** broadcast to the whole mesh using managed flooding.
- **Parameters:** `t` — the instance; `data`/`len` — application data.
- **Returns:** `0` — queued; negative — error (same codes, except "no route").

### Hop-by-hop acknowledgement

A relay sends the hop-by-hop ACK only **after** the frame has actually been
queued for forwarding. A frame dropped because the transmit queue was full is
therefore not acknowledged: the previous hop retransmits it and it is forwarded
once the queue drains. `TX_DONE` still confirms only the first hop, not
end-to-end delivery (see `TODO.md`).

---

## 4. Introspection

### `treenet_addr_t treenet_addr(const treenet_t *t)`
- **Purpose:** the node's address.
- **Parameters:** `t`.
- **Returns:** the address, or `TREENET_ADDR_INVALID` if `t == NULL`.

### `treenet_role_t treenet_role(const treenet_t *t)`
- **Purpose:** the node's role.
- **Returns:** `TREENET_ROLE_NODE` / `MASTER` / `REPEATER` (defaults to `NODE`
  when `t == NULL`).

### `treenet_addr_t treenet_parent(const treenet_t *t)`
- **Purpose:** the current parent.
- **Returns:** the parent address, or `TREENET_ADDR_INVALID` if there is none.

### `uint16_t treenet_rank(const treenet_t *t)`
- **Purpose:** the current rank (accumulated cost of the path to the Master).
- **Returns:** the rank; `0` for the Master; `TREENET_RANK_INFINITE` (0xFFFF) if
  there is no path.

### `bool treenet_is_connected(const treenet_t *t)`
- **Purpose:** whether a usable path to the Master exists.
- **Returns:** `true`/`false`.

### `const treenet_stats_t *treenet_stats(const treenet_t *t)`
- **Purpose:** access the counters for diagnostics/benchmarking.
- **Returns:** a read-only pointer to the statistics block, or `NULL` if
  `t == NULL`.

### `size_t treenet_neighbors(const treenet_t *t, treenet_neighbor_info_t *out, size_t max)`
- **Purpose:** take a snapshot of the neighbour table with link quality.
- **Parameters:** `t` — the instance; `out` — destination array; `max` — its
  capacity.
- **Returns:** the number of entries written (`<= max`); `0` if `t`/`out` is
  `NULL`.

### `const char *treenet_version(void)`
- **Purpose:** the library version string.
- **Returns:** a static string such as `"0.1.0"`.

---

## 5. Configuration (`treenet_config_t`)

Passed to `treenet_init`. Zero-initialise the structure and set what you need.

| Field | Purpose |
|---|---|
| `treenet_addr_t addr` | unique node address (not `0` and not `0xFFFFFFFF`) |
| `treenet_role_t role` | role: `MASTER` / `NODE` / `REPEATER` |
| `uint16_t net_id` | logical network id; frames with another `net_id` are ignored |
| `void (*on_recv)(...)` | datagram receive callback (below) |
| `void (*on_event)(...)` | asynchronous event callback (below) |
| `void *user` | arbitrary application pointer |
| `bool reliable` | request hop-by-hop ACK for unicast |
| `treenet_radio_cfg_t radio` | optional: LoRa parameters for time-on-air |

### Callback `on_recv`

```c
void (*on_recv)(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                size_t len, int16_t rssi, int8_t snr, uint8_t hops);
```
- **Purpose:** notify the application of a received datagram.
- **Parameters:** `t` — the instance; `src` — original sender; `data`/`len` —
  payload; `rssi`/`snr` — quality of the last hop; `hops` — number of hops
  travelled.
- **Returns:** nothing. Called from inside `treenet_poll()`.

### Callback `on_event`

```c
void (*on_event)(treenet_t *t, treenet_event_t ev, void *arg);
```
- **Purpose:** notify the application of a network event.
- **Parameters:** `t` — the instance; `ev` — event code; `arg` — event argument
  (for `PARENT_CHANGED` a pointer to the new parent address, otherwise `NULL`).
- **Returns:** nothing. Called from inside `treenet_poll()`.

---

## 6. Types, events, statistics

### Roles (`treenet_role_t`)
`TREENET_ROLE_NODE` (0), `TREENET_ROLE_MASTER` (1), `TREENET_ROLE_REPEATER` (2),
`TREENET_ROLE_LEAF` (3).

`NODE`, `MASTER` and `REPEATER` are routers (they forward traffic and can be
parents). `LEAF` is an end device: it sends/receives its own data but never
forwards other nodes' traffic, never rebroadcasts floods and is never chosen as
a parent.

### Events (`treenet_event_t`)

| Event | When |
|---|---|
| `NETWORK_READY` | the node joined and has a path to the Master |
| `JOINED` | a parent was chosen for the first time |
| `PARENT_CHANGED` | the parent changed |
| `NEIGHBOR_ADDED` / `NEIGHBOR_REMOVED` | a neighbour appeared / was lost |
| `ROUTE_LOST` | the parent was lost |
| `DISCONNECTED` | the node lost contact with the Master subtree |
| `TX_DONE` / `TX_FAILED` | a reliable frame was acknowledged / exhausted retries |

### Statistics (`treenet_stats_t`)

| Field | Meaning |
|---|---|
| `frames_tx` | frames handed to the port for transmission |
| `frames_rx` | frames received from the port |
| `frames_dropped` | frames dropped locally |
| `retransmissions` | MAC retransmissions |
| `beacons_tx` / `beacons_rx` | beacons transmitted / received |
| `parent_changes` | parent switches |
| `datagrams_tx` / `datagrams_rx` | datagrams accepted for TX / delivered up |
| `airtime_ms` | accumulated estimated time-on-air |

### Link quality (`treenet_link_quality_t`, inside `treenet_neighbor_info_t`)

| Field | Meaning |
|---|---|
| `rssi_dbm` | smoothed RSSI, dBm |
| `snr_db` | smoothed SNR, dB |
| `pdr_q8` | delivery ratio, Q8 (256 = 100%) |
| `etx_q8` | ETX, Q8 (256 = 1.0) |
| `link_cost` | composite link cost |

`treenet_neighbor_info_t` also has `addr`, `rank`, `age_ms`, `is_parent`.

---

## 7. Port (brief)

Mandatory port functions: `tx`, `now_ms`, `rnd`. Optional: `channel_free`,
`log`, `timer_arm`. Details in
[porting.md](porting.md).

---

## 8. Return codes (summary)

| Code | Meaning |
|---|---|
| `0` | success |
| `-1` | invalid arguments / not initialised / queue full |
| `-2` | does not fit / fragmentation impossible |
| `-3` | no route |
