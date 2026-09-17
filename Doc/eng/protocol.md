# treenity protocol

[Русский](../ru/protocol.md) | **English**

This document describes the wire format and the logic of every layer.
Implementation: `src/mac/frame.[ch]`, `src/mac/dupcache.[ch]`,
`src/link/neighbor.[ch]`, `src/link/beacon.c`, `src/routing/routing.[ch]`,
`src/api/treenet.c`.

## 1. Frame format

Every frame starts with a fixed 20 byte header transmitted as raw bytes (no
serialisation schema) to minimise time-on-air. Multi-byte fields are
little-endian.

| Offset | Size | Field | Purpose |
|---|---|---|---|
| 0 | 1 | `ver_type` | version (high nibble) + frame type (low nibble) |
| 1 | 1 | `flags` | flags (see below) |
| 2 | 4 | `src` | original sender address (never changed while relaying) |
| 6 | 4 | `dst` | final recipient or broadcast |
| 10 | 2 | `seq` | originator sequence number |
| 12 | 2 | `net_id` | network identifier |
| 14 | 1 | `hop_limit` | remaining hops |
| 15 | 1 | `sec` | security extension point / reserved |
| 16 | 4 | `prev` | address of the immediate transmitter (rewritten by the MAC) |
| 20 | N | `payload` | payload |
| 20+N | 2 | `crc` | CRC-16 (trailer, only with `TREENET_ENABLE_FRAME_CRC`) |

### Integrity check (CRC-16)

With `TREENET_ENABLE_FRAME_CRC` enabled (the default) every frame carries a
2 byte trailer: CRC-16/CCITT-FALSE (polynomial `0x1021`, init `0xFFFF`, no
reflection, no final XOR). The CRC covers **the whole header, including the
rewritten `prev` field, and the payload**.

- Sender: `tn_frame_encode` computes the CRC after building the header and
  payload and appends it.
- Receiver: `tn_frame_decode` verifies the CRC **before parsing any field**; on
  mismatch the frame is silently dropped (`frames_dropped++`) and no state
  changes.
- Cost: +2 bytes per frame; computed with a nibble table (16 entries) instead of
  a 256 byte table.
- **Interoperability:** the setting must match on every node of the network.
  Nodes with different settings cannot understand each other.

Beyond the CRC, the parser rejects semantically invalid frames (see
"Protection against corrupted frames" at the end of this document).

### Flags (`flags`)

| Bit | Name | Meaning |
|---|---|---|
| 0x01 | `TN_FLAG_WANT_ACK` | request a hop-by-hop acknowledgement |
| 0x02 | `TN_FLAG_FLOOD` | the frame is being flooded |
| 0x04 | `TN_FLAG_FRAG` | the payload begins with a fragment header |
| 0x08 | `TN_FLAG_SEC` | the payload is protected (extension point) |

### The `prev` field — a key detail

`src` always points at the **original** sender and is never changed while
relaying. `prev` is rewritten by the MAC on every transmission with the address
of the current transmitter (`tn_tx_submit`). This allows:

- addressing a hop-by-hop ACK to the immediate transmitter (`prev`);
- installing a downward route through the node the frame actually came from;
- estimating link quality against the neighbour (`prev`) rather than the origin
  of a multi-hop packet.

## 2. Frame types

| Type | Name | Purpose |
|---|---|---|
| 0 | `TREENET_FRAME_BEACON` | link-local hello: rank, parent, interval |
| 1 | `TREENET_FRAME_DATA` | unicast application datagram |
| 2 | `TREENET_FRAME_ACK` | single-hop acknowledgement |
| 3 | `TREENET_FRAME_FLOOD` | broadcast datagram (managed flooding) |
| 4 | `TREENET_FRAME_PROBE` | beacon solicitation (parent search) |
| 5 | `TREENET_FRAME_DAO` | downward route advertisement |

### Beacon payload (9 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | sender `rank` |
| 2 | 4 | sender `parent` (or `0xFFFFFFFF`) |
| 6 | 1 | `flags` (bit 0 `TN_BEACON_HAS_PARENT` — connected; bit 1 `TN_BEACON_ROUTER` — may be a parent) |
| 7 | 2 | `interval_100ms` — current beacon interval in 100 ms units |

The interval is advertised so a neighbour can compute the expected number of
beacons between two receptions and not mistake a growing Trickle interval for
packet loss.

### DAO payload (5 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `origin` — the node the route is for |
| 4 | 1 | `hops` — hop count from `origin` |

### Fragment header (4 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `dgram_id` — datagram identifier |
| 2 | 1 | `index` — fragment index (0 based) |
| 3 | 1 | `count` — total number of fragments |

## 3. MAC layer

### 3.1 CSMA/CA

Before transmission a frame is placed into a slot (`tn_tx_submit`) and scheduled
for `now + delay + backoff`, where `backoff = random(0..TREENET_CW_MAX) *
TREENET_SLOT_MS`. In `tn_tx_poll`:

1. if the slot is reliable and has exhausted its attempts — it is freed and a
   `TX_FAILED` event is raised;
2. if the port provides `channel_free` and the medium is busy — the attempt is
   deferred by one slot;
3. otherwise the frame is transmitted through `port.tx`.

### 3.2 Duplicate cache

`tn_dupcache_seen(src, seq, now)` stores recently seen `(src, seq)` pairs in a
fixed-size hash table with a TTL of `TREENET_DUP_TTL_MS`. It suppresses
duplicates during forwarding and flooding.

> Important: when a duplicate of a reliable frame arrives, the ACK is still sent
> again so the sender stops retransmitting.

### 3.3 Hop-by-hop ACK

- A receiver of a frame with the `WANT_ACK` flag replies with an `ACK` frame to
  the `prev` address.
- The `ACK` reuses the `seq` field to carry the acknowledged sequence number.
- The sender matches the ACK by `(ack_dst, ack_seq)` against its slot and frees
  it (`TX_DONE` event).
- Without an ACK the frame is retransmitted, up to `TREENET_MAX_RETRIES`
  transmissions in total.

### 3.4 Time-on-air

`tn_airtime_estimate_us()` computes LoRa time-on-air using the Semtech
AN1200.13 formula (SF, BW, CR, preamble, CRC). If LoRa parameters are not set, a
conservative linear estimate is used. The accumulated time is kept in
`stats.airtime_ms`. This is the basis for future duty-cycle control.

### 3.5 Fragmentation

Datagrams larger than `TREENET_MTU - TN_FRAME_OVERHEAD` are split into fragments
with a `FRAG` header (the `TN_FLAG_FRAG` flag). Reassembly uses
`TREENET_REASSEMBLY_SLOTS` slots with a bitmap of received fragments; once
complete the datagram is delivered to `on_recv`. The maximum fragment count is
`TREENET_MAX_FRAGMENTS`.

## 4. Link layer

### 4.1 Beaconing (Trickle)

- The interval `I` starts at `TREENET_BEACON_MIN_MS` and doubles after each
  transmission up to `TREENET_BEACON_MAX_MS`.
- A transmission is scheduled at a random moment in the second half of the
  interval (`[I/2, I)`) so nodes do not synchronise.
- Selecting/changing a parent resets `I` to the minimum (`tn_beacon_reset`,
  called from `routing_set_parent`) so neighbours learn the new rank and route
  sooner — this speeds up reconvergence.
- Beacons are never forwarded (`hop_limit = 1`).

> **Critical:** `TREENET_BEACON_MAX_MS` must be smaller than
> `TREENET_PARENT_TIMEOUT_MS` and `TREENET_NEIGHBOR_TIMEOUT_MS` (a factor of
> three is used). Otherwise a node considers a healthy neighbour dead in the
> gap between beacons and flapping begins. The defaults are consistent: 20 s
> versus 60/90 s.

### 4.2 Link quality estimation (LQI)

For each neighbour:

- **RSSI, SNR** — EWMA (factor 1/8) over all received frames.
- **PDR** — from beacons: between two receptions `elapsed / advertised_interval`
  beacons are expected; every missed period is added as a zero, the current one
  as 256. The EWMA yields a smooth delivery ratio.
- **ETX** = `256·256 / PDR` (Q8; 256 == 1.0).
- **Link cost** (`link_cost`):

  ```
  snr_score = f(SNR)          # 0..1024, 0 is ideal, 1024 is marginal
  etx_pen   = clamp(ETX_q8-256, 0, 768)   # 0 at ETX 1.0, 768 at ETX 4.0
  link_cost = (W_SNR·snr_score + W_ETX·etx_pen) / 256
  ```

  ETX dominates, which makes the objective function prefer a good two-hop path
  over a single bad link.

## 5. Routing layer

### 5.1 Rank and the DODAG

- Master: rank = 0, no parent, always `connected`.
- Node: `rank = parent.rank + TREENET_RANK_STEP + link_cost(parent)`. The step
  (`TREENET_RANK_STEP`, default 8) guarantees a strictly increasing rank along a
  path, which prevents loops.
- `TREENET_RANK_INFINITE` (0xFFFF) means "no path".

### 5.2 Objective function and parent selection

A candidate is a neighbour that:
- is "fresh" (a beacon was heard within `TREENET_PARENT_TIMEOUT_MS`);
- advertises `TN_BEACON_ROUTER` (it is a router; leaves never set this);
- yields a finite `candidate_rank = n.rank + RANK_STEP + n.link_cost`.

The candidate with the lowest `candidate_rank` is chosen. If there are no
candidates, the node loses its parent (`ROUTE_LOST`/`DISCONNECTED`).

### 5.3 Hysteresis and dwell time

A parent switch happens if:
- the current parent is dead or unusable, **or**
- `TREENET_PARENT_DWELL_MS` has elapsed since the current parent was chosen,
  **and** `best_rank < cur_rank · (100 − H)/100`, where `H` is
  `TREENET_PARENT_HYSTERESIS_PCT` (25 % by default).

This eliminates flapping under fading while keeping fast recovery from a real
failure.

### 5.4 Loop avoidance

Because the rank strictly increases, a parent always has a lower rank than its
child. Potential loops are further cut by the `candidate_rank < 0xFFFF` check
and by the fact that a descendant always has a higher rank.

### 5.5 Downward routes (storing mode) and DAO

- On (re)selecting a parent a node sends a `DAO` upward.
- The DAO travels to the parent with `src = origin` preserved and `prev` set to
  the current transmitter.
- Each intermediate node, on receiving a DAO, installs a route
  `dst = origin, next_hop = prev` and forwards the DAO to its own parent; the
  Master installs the route too and stops.
- This gives every ancestor a route to all nodes in its subtree.
- DAOs are sent reliably (`WANT_ACK`) so routes are not lost on a noisy channel.
- Periodic refresh every `TREENET_ROUTE_REFRESH_MS`; an entry expires after
  `TREENET_ROUTE_TIMEOUT_MS`.
- When a neighbour is lost, all routes through it are removed
  (`tn_route_remove_via`).

### 5.6 Route repair

When the parent is lost the node immediately tries to select a new one
(`select_parent`). If there are no candidates it periodically broadcasts a
`PROBE` (broadcast, `hop_limit=1`), to which neighbours reply with an immediate
beacon.

## 6. Data forwarding

### 6.1 Next hop selection

`tn_route_next_hop(dst)`:
1. `dst` is a direct neighbour → send to it (it delivers, no loop);
2. otherwise a route from the downward table (`next_hop`);
3. otherwise the parent (upward, reaching any ancestor including the Master).

### 6.2 Unicast delivery

`DATA` with a final `dst`. Each node:
- if `dst == me` → (with `WANT_ACK`) ACK to `prev`, then deliver to `on_recv`;
- otherwise check for duplicates, decrement `hop_limit`, send to the next hop.
  With `WANT_ACK` an ACK is also sent on intermediate nodes (per-hop
  acknowledgement).

> Leaves (`LEAF`) never forward other nodes' unicast: a frame not addressed to
> them is dropped.

### 6.3 Managed flooding (broadcast)

`FLOOD` with `dst = broadcast`:
- duplicate check;
- local delivery to the application;
- rebroadcast with a delay that depends on the SNR of the received frame: **a
  weak link (low SNR) rebroadcasts sooner**, a strong link later. This spreads
  the message outwards before the dense core echoes it.
- The `REPEATER` role rebroadcasts with zero delay.
- Leaves (`LEAF`) deliver broadcasts to themselves but **never rebroadcast** them.

## 7. Events and statistics

Events (`on_event`): `NETWORK_READY`, `JOINED`, `PARENT_CHANGED`,
`NEIGHBOR_ADDED`, `NEIGHBOR_REMOVED`, `ROUTE_LOST`, `TX_DONE`, `TX_FAILED`,
`DISCONNECTED`.

Counters (`treenet_stats`): `frames_tx/rx/dropped`, `retransmissions`,
`beacons_tx/rx`, `parent_changes`, `datagrams_tx/rx`, `airtime_ms`.

## 8. Protection against corrupted frames

Beyond the CRC, the library checks structural and semantic validity and
**never changes state because of an invalid frame**:

| Level | Check |
|---|---|
| Frame | length ≥ `TN_FRAME_OVERHEAD`; CRC matches; protocol version; `src != 0/broadcast`; `dst != 0` |
| Receive | `net_id` matches; `src != own address` |
| Beacon | payload ≥ 9; advertised interval in `[BEACON_MIN/2 .. BEACON_MAX]`; `parent != src`; with the `HAS_PARENT` flag — `rank < INFINITE` |
| DAO | payload ≥ 5; `origin != 0/broadcast`; `hops ≤ MAX_HOPS`; `prev != 0/broadcast` |
| Fragment | payload ≥ 4; `count ∈ [1..MAX_FRAGMENTS]`; `index < count`; a non-last fragment is full size; all fragments of a datagram agree on `count`; `offset+len ≤ MAX_DATAGRAM` |
| Frame type | unknown type → dropped |

Any violation → `frames_dropped++`, and the state (neighbours, parent, rank,
routes) is left unchanged. A corrupted beacon cannot lure a node to a bogus
parent, and a corrupted DAO cannot install a false route.
