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

- The library is single threaded. All state is mutated inside `treenet_poll()`.
- `treenet_rx()` may be called from an interrupt: it only copies the frame into
  a ring buffer. To protect against races the port may provide
  `critical_enter`/`critical_exit` (briefly disabling interrupts).
- `treenet_poll()` must be called often enough. The recommended period is
  10–100 ms. Calling it too rarely increases retransmission and timer latency.

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
  `net_id` are silently ignored, so several networks can share a channel.

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
