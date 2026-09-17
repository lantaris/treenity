# treenity — detailed development plan

[Русский](../ru/treenity-plan.md) | **English**

> Document version: 2.0 (after the v0.1.0 implementation).
> Status: the core, the simulator and the tests are implemented and pass; the
> hardware port, security and scaling are still ahead.

---

## 1. The task

The library builds a mesh network around a single root node, the **Master**.
Requirements:

1. The network is built around one Master.
2. Full abstraction from microcontrollers; easy portability through a
   user-supplied port (API + callbacks).
3. The primary goal is **seamless reconfiguration** of the topology for the best
   link quality.
4. When a packet arrives from the user layer, the library receives it together
   with the sender's **RSSI and SNR**; the values are used for link estimation
   and exposed to the application.
5. The transport is LoRa, but the library is not tied to a modem.

## 2. Decisions made

| Question | Decision | Rationale |
|---|---|---|
| Core language | **C99** | maximum portability, minimal runtime |
| Memory | **static only**, no `malloc` | predictability on an MCU |
| Scale | 100–1000 nodes per Master | drives table sizes |
| Simulator | **mandatory** | mesh logic cannot be tested without it |
| Security | hooks, implemented later | does not block v1 |
| Topology | **DODAG rooted at the Master** | the RPL standard for rooted networks |
| Metric | composite: **ETX + SNR** | ETX beats hop count; SNR is informative on LoRa |
| Broadcast | **managed flooding** with SNR priority | as in Meshtastic |
| Port | a struct of function pointers | Contiki-NG NETSTACK, RadioHead |
| Control traffic | **Trickle** | adaptive beacon interval |

Sources: RPL (RFC 6550), Trickle (RFC 6206), ETX (MIT), Meshtastic mesh-algo,
Contiki-NG NETSTACK, Semtech AN1200.13 (time-on-air).

---

## 3. Architecture (implemented)

Layers (bottom-up): **PORT → Core → MAC → Link → Routing → API**.

- **PORT** (`include/treenet/port.h`) — implemented by the user: `tx`, `now_ms`,
  `rnd` are mandatory; `channel_free`, `set_radio`, `critical_enter/exit`, `log`
  are optional. Receive — `treenet_rx(buf,len,rssi,snr)`.
- **Core** (`src/core`): receive ring buffer, timers, EWMA, utilities, context.
- **MAC** (`src/mac`): frame codec, CRC, dup-cache, CSMA/CA, fragmentation,
  time-on-air.
- **Link** (`src/link`): beaconing (Trickle), neighbour table, LQI.
- **Routing** (`src/routing`): DODAG, Rank, objective function, parent
  selection, DAO, downward route table.
- **API** (`src/api/treenet.c`): lifecycle, poll, receive, forwarding, API.

Details in [architecture.md](architecture.md) and [protocol.md](protocol.md).

---

## 4. Implemented modules

| Module | Files | Status |
|---|---|---|
| Configuration | `include/treenet/config.h` | ✅ |
| Types | `include/treenet/types.h` | ✅ |
| Port (HAL) | `include/treenet/port.h` | ✅ |
| Public API | `include/treenet/treenet.h` | ✅ |
| Internal context | `src/core/internal.h` | ✅ |
| Utilities | `src/core/util.h` | ✅ |
| EWMA | `src/core/ewma.h` | ✅ |
| Ring buffer | `src/core/ringbuf.[ch]` | ✅ |
| Timers | `src/core/timer.[ch]` | ✅ |
| Frame/codec | `src/mac/frame.[ch]` | ✅ |
| Integrity check (CRC-16) | `src/mac/frame.[ch]` | ✅ |
| Dup-cache | `src/mac/dupcache.[ch]` | ✅ |
| Time-on-air | `src/mac/airtime.[ch]` | ✅ |
| Neighbours and LQI | `src/link/neighbor.[ch]` | ✅ |
| Beaconing | `src/link/beacon.c` | ✅ |
| Routing | `src/routing/routing.[ch]` | ✅ |
| Core/API | `src/api/treenet.c` | ✅ |
| Simulator | `sim/sim.[ch]` | ✅ |
| Unit/scenario tests | `tests/unit/*` | ✅ |
| Fuzzing | `tests/fuzz/fuzz_frame.c` | ✅ |
| Example | `examples/desktop_mesh/main.c` | ✅ |
| Build | `CMakeLists.txt`, `Makefile` | ✅ |

---

## 5. Key algorithms (implemented)

1. **Rank** = `parent.rank + TREENET_RANK_STEP + link_cost(parent)`; the strict
   increase prevents loops.
2. **Objective function** `link_cost = (W_SNR·snr_score + W_ETX·etx_pen)/256`;
   ETX dominates, so a good multi-hop path beats a single bad link.
3. **Parent selection** by minimum `candidate_rank`; **hysteresis** (25 %) and
   **dwell-time** (30 s) prevent flapping.
4. **LQI**: EWMA RSSI/SNR; PDR from beacons using the advertised interval;
   ETX = 1/PDR.
5. **Beaconing (Trickle)**: interval 5→20 s, random offset, reset on a parent
   change.
6. **DAO**: reliable (`WANT_ACK`) route advertisement up to the Master; downward
   route table (storing mode).
7. **Managed flooding**: the rebroadcast delay shrinks as SNR drops; a REPEATER
   jumps the queue.
8. **Hop-by-hop ACK** and retransmissions for reliable frames.
9. **Fragmentation/reassembly** of datagrams > MTU.
10. **Integrity check** — CRC-16/CCITT-FALSE (frame trailer, enabled by
    default); semantic frame validation; a corrupted frame never changes state.
11. **Tickless scheduling** — periodic processes (route expiry, neighbour
    ageing, reassembly, DAO refresh, PROBE) are expressed as computed deadlines;
    at the end of `treenet_poll()` the port receives the nearest deadline via
    `port.timer_arm()` so the MCU can sleep.

---

## 6. Verification (current)

- **1871 checks**, 0 failures (`make test`, `ctest`).
- Scenarios: network formation, rank ordering, unicast down/up, broadcast,
  seamless reconfiguration on relay failure, beacon interval reset on
  re-parenting, tolerance to bit errors, neighbour metrics.
- **Corruption tests:** the CRC catches any single bit and truncation;
  corrupted and semantically broken frames (beacon/DAO/fragments) do not change
  state; a foreign `net_id` is ignored.
- **Fuzzing** — mutation (from a valid-frame corpus) plus random input, 300 000
  iterations with no crashes or invariant violations.
- Builds without warnings with `-Wall -Wextra`.
- Example: a Master + 5 nodes network over ~1 km, rank 0/8/16/24/32/40, unicast
  both ways and broadcast delivered.

---

## 7. Configuration reference (`config.h`)

| Parameter | Default | Meaning |
|---|---|---|
| `TREENET_MTU` | 200 | maximum frame size |
| `TREENET_ENABLE_FRAME_CRC` | 1 | frame CRC-16 (+2 byte trailer) |
| `TREENET_MAX_HOPS` | 8 | hop limit |
| `TREENET_MAX_DATAGRAM` | 1024 | maximum application data |
| `TREENET_MAX_NEIGHBORS` | 32 | neighbour table size |
| `TREENET_MAX_ROUTES` | 128 | downward routes (Master of 1000 nodes → 1024) |
| `TREENET_DUP_CACHE_SIZE` | 64 | duplicate cache |
| `TREENET_TX_QUEUE_SIZE` | 8 | transmit queue |
| `TREENET_RX_RING_BYTES` | 1024 | receive ring buffer |
| `TREENET_BEACON_MIN_MS` | 5000 | minimum beacon interval |
| `TREENET_BEACON_MAX_MS` | 20000 | maximum beacon interval |
| `TREENET_PARENT_TIMEOUT_MS` | 60000 | parent timeout |
| `TREENET_NEIGHBOR_TIMEOUT_MS` | 90000 | neighbour timeout |
| `TREENET_PARENT_DWELL_MS` | 30000 | minimum parent hold time |
| `TREENET_PARENT_HYSTERESIS_PCT` | 25 | parent switch threshold |
| `TREENET_OF_W_SNR/W_ETX` | 120/96 | objective function weights |
| `TREENET_FLOOD_CW_MS` | 500 | flooding contention window |
| `TREENET_ACK_TIMEOUT_MS` | 2000 | ACK wait time |
| `TREENET_MAX_RETRIES` | 3 | transmissions of a reliable frame |
| `TREENET_ROUTE_REFRESH_MS` | 60000 | DAO period |
| `TREENET_ROUTE_TIMEOUT_MS` | 180000 | route lifetime |
| `TREENET_PROBE_INTERVAL_MS` | 5000 | PROBE period while searching for a parent |
| `TREENET_REASM_TIMEOUT_MS` | 8000 | reassembly slot lifetime |

**Mandatory relation:**
`BEACON_MAX_MS < PARENT_TIMEOUT_MS < NEIGHBOR_TIMEOUT_MS`.

---

## 8. What remains (roadmap)

### Phase 9. Reference hardware port (not started)
- Port for SX1262 + ESP32/STM32: `tx`, CAD, IRQ receive with RSSI/SNR, time.
- Demo firmware: Master on a gateway + nodes.
- Field trials: PDR, latency, convergence, range.
- Calibrate the simulator model against real measurements.

### Phase 10. Scale beyond 500 nodes (not started)
- Non-storing mode: source routing from the Master, O(1) memory per node.
- Control-traffic optimisation (Trickle `k`, suppression of redundant DAOs).
- Split tables: routes only on the Master / relevant ancestors.

### Phase 11. Security (not started)
- The `sec` header byte and `TN_FLAG_SEC` are already in place.
- AES-CCM/ChaCha20-Poly1305, key management, replay protection.
- Channel hash (as in Meshtastic) to separate networks.

### Phase 12. Power saving (not started)
- Sleep cycles with a long preamble, synchronised receive windows.
- Duty-cycle accounting in the MAC (the airtime budget is already tracked).
- Battery operation: adapt intervals to remaining energy.

### Phase 13. Additional
- Channel hopping / frequency plans.
- Traffic prioritisation and QoS.
- Multi-master / border router.
- Diagnostic protocol (topology and metric collection).

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Route flapping | hysteresis + dwell-time; consistent timeouts |
| Route loss on a noisy channel | reliable DAOs (`WANT_ACK`) |
| LoRa duty cycle | airtime accounting; Trickle; `FLOOD_CW` |
| Source-route / table growth | bounded by `MAX_ROUTES`; non-storing in the future |
| Unsafe RX path | `treenet_rx` only copies; critical sections |
| Simulator fidelity | calibration against field data |

---

## 10. Success criteria

| Metric | Target | Current |
|---|---|---|
| PDR in a static network | > 95 % | exercised by scenarios |
| Control traffic | < 5 % airtime | `beacons_tx`/`airtime_ms` |
| Convergence after a failure | seconds, no flapping | confirmed by a test |
| RAM per node | within budget, 0 malloc | ✅ |
| Simulator reproducibility | by seed | ✅ |
| Warning-free build | yes | ✅ |
