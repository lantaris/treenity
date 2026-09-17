# treenity architecture

[Русский](../ru/architecture.md) | **English**

## 1. Goals and constraints

**Goals**

1. A mesh network with a single root, the **Master**, that builds and
   reconfigures itself for the best link quality.
2. Full hardware abstraction: the library knows nothing about the MCU, radio,
   RTOS or OS. All hardware access goes through the port.
3. Portability: C99, no dynamic memory, no platform dependency other than
   `string.h`/`stdint.h`.
4. Link quality based on the RSSI/SNR of every received frame.

**Constraints**

- Target scale: 100–1000 nodes per Master.
- The default transport is LoRa, but the library is not tied to a modem.
- Security is not implemented in v1, but extension points are in place
  (`TN_FLAG_SEC`, the `sec` header byte, `TREENET_ENABLE_SECURITY_HOOKS`).

## 2. Layers

```
┌──────────────────────────────────────────────────────────┐
│ Application: treenet_send / treenet_broadcast / callbacks │
├──────────────────────────────────────────────────────────┤
│ Routing  (src/routing): DODAG, Rank, Objective Function,  │
│          parent selection, DAO, downward route table      │
├──────────────────────────────────────────────────────────┤
│ Link     (src/link):    beaconing (Trickle), neighbour    │
│          table, LQI (RSSI/SNR/PDR/ETX/cost)               │
├──────────────────────────────────────────────────────────┤
│ MAC      (src/mac):     codec+CRC, CSMA/CA, dup-cache,    │
│          fragmentation, time-on-air, ACK                  │
├──────────────────────────────────────────────────────────┤
│ Core     (src/core):    ring buffer, timers, EWMA,        │
│          event queue, utilities, context                  │
├──────────────────────────────────────────────────────────┤
│ PORT     (implemented by the user): tx/now_ms/rnd/...     │
│          receive: treenet_rx(buf, len, rssi, snr)         │
└──────────────────────────────────────────────────────────┘
```

The layers share a single context, `struct treenet` (see `src/core/internal.h`),
and never use dynamic memory.

## 3. Memory model

- All memory is provided by the caller: `treenet_init(void *storage,
  size_t size, ...)`. The size is `treenet_context_size()`.
- The context holds fixed-size arrays:
  - neighbour table (`TREENET_MAX_NEIGHBORS`, default 32);
  - downward route table (`TREENET_MAX_ROUTES`, default 128);
  - duplicate cache (`TREENET_DUP_CACHE_SIZE`, 64);
  - outbound frame queue (`TREENET_TX_QUEUE_SIZE`, 8);
  - receive ring buffer (`TREENET_RX_RING_BYTES`, 1024 bytes);
  - fragment reassembly slots (`TREENET_REASSEMBLY_SLOTS`, 2);
  - event queue (`TREENET_EVENT_QUEUE_SIZE`, 16).
- `malloc` is never used. Neither is `free`.

> **Master memory.** In storing mode (the default) each node keeps a route for
> every node in its subtree. For a Master of a 1000 node network raise
> `TREENET_MAX_ROUTES` to `1024`. Ordinary leaves are fine with the default.

## 4. Time model

- Time is monotonic milliseconds from `port.now_ms()`, type `uint32_t`.
- All time comparisons are wrap-safe (`tn_time_after`, `tn_elapsed`); the
  ~49 day wrap is handled correctly.
- Timers (`tn_timer_t`) do not use interrupts: they are evaluated inside
  `treenet_poll()`.
- **Tickless.** Periodic processes (route expiry, neighbour ageing, reassembly
  cleanup, DAO refresh, PROBE) are not driven by a fixed tick but by **computed
  deadlines**. At the end of every `treenet_poll()` the library reports the
  nearest deadline to the port through the optional `port.timer_arm(delay_ms)`,
  so the MCU can sleep until it is actually needed.

## 5. Threading and ISR model

The library is **single threaded**, but the receive path is designed to be
called from an interrupt.

- **One context for `treenet_poll()`.** All state (neighbours, routes, parent,
  transmit slots, events) is mutated only inside `poll`. Calling `poll` from two
  contexts (for example a timer ISR and the main loop) is not allowed — the
  library is not reentrant.
- **`treenet_rx()` comes from the modem interrupt.** The receive ring is a
  **lock-free SPSC** buffer: exactly one producer (`treenet_rx`) and one consumer
  (`poll`); no critical section is needed. The function only copies the frame
  into the buffer.
- **`treenet_send`/`treenet_broadcast` and introspection** must run in the same
  context as `poll` (otherwise races on the transmit slots/routes and torn
  reads).
- **Instances are independent** — the library has no global state, so different
  contexts may be driven from different threads as long as the port is
  thread-safe.
- **Target MCUs are 32-bit (or wider)**: lock-free requires atomic word-sized
  index accesses; this is enforced by a compile-time check in `config.h`.
- Call `treenet_poll()` often enough (10–100 ms) or from `timer_arm` (tickless).

**Multi-threading: the caller synchronizes.** If the application calls the
library from several threads/tasks, it serialises the **non-ISR** entry points
with **its own mutex**:

- under the mutex: `treenet_poll`, `treenet_send`, `treenet_broadcast` and
  introspection (`treenet_neighbors`, `treenet_stats`, ...);
- **outside the mutex**: `treenet_rx` (called from an ISR; the ring is lock-free
  and taking a lock in an interrupt is not allowed).

Rules: the `on_recv`/`on_event` callbacks and the port callbacks (`tx`, `now_ms`,
`rnd`, `timer_arm`) run **inside** `poll`, i.e. under your mutex — do not take the
mutex again and do not call library functions from the callbacks (the library is
not reentrant); defer the work (flag/queue) until `poll` returns. `treenet_init`
runs before the threads start, so it needs no mutex.

## 6. Roles

| Role | Description |
|---|---|
| `TREENET_ROLE_MASTER` | Root of the DODAG. Rank = 0, no parent, always `connected`. |
| `TREENET_ROLE_NODE` | Ordinary node. Has a parent and a route to the Master. |
| `TREENET_ROLE_REPEATER` | Infrastructure repeater: like a NODE, but rebroadcasts floods with the highest priority (zero delay). |
| `TREENET_ROLE_LEAF` | End device / sensor. Sends and receives its **own** traffic but is **not a router**: it never forwards other nodes' frames, never rebroadcasts floods and is never chosen as a parent. |

## 7. Addressing

- `treenet_addr_t` is 32 bits.
- `0xFFFFFFFF` is broadcast, `0x00000000` is the invalid address.
- The address is assigned by the application in `treenet_config_t.addr` (for
  example from a hardware MAC/ID). The library never assigns addresses.
- `net_id` (16 bits) is a logical network identifier; frames with a different
  `net_id` are silently ignored (before link metrics, so foreign nodes never
  appear in the neighbour table), which lets several networks share a channel.

## 8. Extension points

- **Security.** The `sec` header byte and the `TN_FLAG_SEC` flag are reserved.
  A port may encrypt the payload before `tx` and decrypt it after
  `treenet_rx`.
- **Radio reconfiguration.** `port.set_radio` is called when the upper layer
  wants to change SF/BW/power.
- **Metrics.** `treenet_neighbors()` and `treenet_stats()` give the application
  access to live link quality estimates and counters.

## 9. Limitations of the current version

- Routes are stored (storing mode). Non-storing (source routing from the Master)
  is the direction of growth for networks above ~500 nodes.
- No encryption or authentication (extension points only).
- No power saving / sleep.
- One Master per network.
