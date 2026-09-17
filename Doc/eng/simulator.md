# Network simulator

[Русский](../ru/simulator.md) | **English**

The desktop simulator (`sim/sim.h`, `sim/sim.c`) runs a complete mesh network
without any radio. It is used by the tests and the example.

## 1. What is modelled

- **Node placement** on a plane (metres).
- **Shared radio channel**: one node's transmission is heard by everyone within
  range.
- **Distance based loss**: log-distance path loss plus a loss probability that
  depends on the margin to sensitivity.
- **Hard range** (`sim_set_range`): beyond it reception is impossible.
- **Collisions**: if a node hears more than one frame in the same millisecond
  step, all of them are considered lost.
- **Virtual time**: 10 ms steps, reproducible from a seed.

## 2. Creating and running

```c
#include "sim.h"

sim_t *s = sim_create(seed, tx_power_dbm, sensitivity_dbm, noise_floor_dbm);
sim_set_path_loss(s, ref_loss_db, exponent);   /* loss model */
sim_set_range(s, range_m);                     /* hard range */

sim_add_node(s, /*addr*/ 1, TREENET_ROLE_MASTER, /*x*/ 0.0,  /*y*/ 0.0,  /*reliable*/ true);
sim_add_node(s, /*addr*/ 2, TREENET_ROLE_NODE,   /*x*/ 200.0, /*y*/ 0.0, true);

sim_run(s, 120000);   /* run 120 seconds of virtual time */

sim_node_t *n = sim_find(s, 2);
printf("rank=%u parent=%u connected=%d\n",
       treenet_rank(n->net), treenet_parent(n->net),
       treenet_is_connected(n->net));

sim_destroy(s);
```

## 3. API

| Function | Purpose |
|---|---|
| `sim_create(seed, tx_dbm, sens_dbm, noise_dbm)` | create the simulator |
| `sim_destroy(s)` | free resources and node contexts |
| `sim_add_node(s, addr, role, x, y, reliable)` | add a node |
| `sim_move_node(s, addr, x, y)` | move a node (mobility) |
| `sim_set_active(s, addr, bool)` | turn a node's radio off/on |
| `sim_find(s, addr)` | find a node |
| `sim_node_at(s, i)` / `sim_node_count(s)` | iterate over nodes |
| `sim_now(s)` | current virtual time |
| `sim_run(s, ms)` | advance time |
| `sim_set_path_loss(s, ref, exp)` | loss model |
| `sim_set_range(s, m)` | hard range |
| `sim_set_bit_error_rate(s, ber)` | per-bit corruption probability of a received frame |
| `sim_set_callbacks(s, recv, event)` | observation callbacks |

## 4. Observation

Every node has counters:
`datagrams_rx`, `events`, `last_src`, `last_len`, `last_rssi`, `last_snr`.

The optional `sim_set_callbacks` hooks fire on datagram delivery and on events —
handy for logging (see `examples/desktop_mesh/main.c`).

## 5. Tuning parameters for a desired topology

Loss model: `loss = ref_loss + 10·exp·log10(d)`, `rssi = tx_power − loss`,
`snr = rssi − noise_floor`. Reception is impossible if `rssi < sensitivity`,
`snr < −20 dB` or `d > range`.

For **crisp, deterministic** topologies the tests use a mild loss model
(`ref=40, exp=3.0`) plus a hard range (`range=250 m`): within range the link is
almost perfect, beyond it is dead.

Examples:
- chain: nodes every 200 m, `range=250` → each node hears only its neighbours;
- grid: Master(0,0), R1(200,0), R2(0,200), N(200,200), `range=250` → N does not
  hear the Master (283 m) and picks a relay.

## 6. Ready-made example

`examples/desktop_mesh/main.c` builds a Master + 5 nodes network over ~1 km,
prints the topology, sends unicast down and up, a broadcast and the statistics:

```
cmake --build build
./build/treenet_example
```
