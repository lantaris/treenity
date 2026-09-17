# treenity

[Русский](README-ru.md) | **English**

A portable C99 mesh network with a single root node, the **Master**. The library
is fully abstracted from microcontrollers and radios: all hardware interaction
happens through a user-supplied port (HAL). The network builds itself and
**seamlessly reconfigures** for the best link quality, using the RSSI/SNR of
every received packet.

- Language: **C99**, no dynamic memory, no RTOS/OS dependency
- Transport: **LoRa** (and any other modem through the port)
- Topology: **Master-rooted DODAG** in the spirit of RPL
- Scale: 100–1000 nodes per Master
- Version: **0.1.0**

---

## Contents

- [Features](#features)
- [Architecture](#architecture)
- [Requirements](#requirements)
- [Build and install](#build-and-install)
- [Quick start](#quick-start)
- [Repository layout](#repository-layout)
- [Testing](#testing)
- [Configuration](#configuration)
- [Documentation](#documentation)
- [Roadmap](#roadmap)
- [License](#license)
- [Contributing](#contributing)

---

## Features

- **Hardware abstraction.** The port needs only three functions (`tx`,
  `now_ms`, `rnd`); the rest are optional. The same code runs on any MCU and on
  a host.
- **Static memory.** No `malloc`: all tables are fixed size and configured in
  `config.h`.
- **Cooperative model.** Non-blocking `treenet_poll()`; the receive path
  (`treenet_rx`) is safe to call from an interrupt.
- **Link quality from RSSI/SNR.** EWMA RSSI/SNR, PDR from beacons, ETX and a
  composite link cost; the values are exposed to the application.
- **Smart parent selection.** An objective function plus hysteresis and
  dwell-time eliminate flapping and enable fast reconfiguration.
- **Participant roles.** `MASTER` (root), `NODE`/`REPEATER` (routers) and `LEAF`
  (end device: sends and receives its own data but never forwards other traffic
  and is never chosen as a parent).
- **Managed flooding** for broadcasts with SNR-based priority.
- **Reliability.** Hop-by-hop ACK and retransmissions; reliable route
  advertisement (DAO). An ACK is accepted from any neighbour that forwarded the
  frame, and a next hop that stops acknowledging is detected in seconds (fast
  local repair).
- **Integrity check.** A CRC-16 on every frame (enabled by default): corrupted
  frames are dropped before processing and never change network state.
- **Fragmentation** of datagrams larger than the MTU.
- **Time-on-air accounting** using the Semtech AN1200.13 formula.
- **Tickless scheduling.** An optional `port.timer_arm()` tells the application
  when to wake the MCU; periodic processes are expressed as computed deadlines.
- **Desktop simulator** with reproducible scenarios and metric collection.

## Architecture

```
┌───────────────────────────────────────────────────────┐
│ Application: treenet_send / broadcast / callbacks      │
├───────────────────────────────────────────────────────┤
│ Routing : DODAG, Rank, Objective Function, DAO, routes │
├───────────────────────────────────────────────────────┤
│ Link    : beaconing (Trickle), neighbours, LQI         │
├───────────────────────────────────────────────────────┤
│ MAC     : frames, CSMA/CA, dup-cache, ACK, fragments   │
├───────────────────────────────────────────────────────┤
│ Core    : ring buffer, timers, EWMA, events            │
├───────────────────────────────────────────────────────┤
│ PORT    : tx / now_ms / rnd / ... (user implemented)   │
└───────────────────────────────────────────────────────┘
```

More: [Doc/eng/architecture.md](Doc/eng/architecture.md),
[Doc/eng/protocol.md](Doc/eng/protocol.md).

## Requirements

- A C99 compiler (tested with GCC and `arm-none-eabi-gcc`).
- For the core: only `stdint.h`, `stddef.h`, `stdbool.h`, `string.h`.
- To build the tests/simulator/example: CMake ≥ 3.13 **or** GNU Make, plus the
  standard C library (`libm`).

## Build and install

### CMake (recommended)

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Targets: `treenet` (core), `treenet_sim` (simulator), `treenet_tests`,
`treenet_fuzz`, `treenet_example`.

### Make

```sh
make            # core -> build/libtreenet.a
make test       # build and run the tests
make run        # build and run the example
make fuzz       # fuzzing
make repeaters  # stress report: 1 Master + 30 repeaters
```

### ARM cross-build

```sh
cmake -S . -B build-arm -DCMAKE_C_COMPILER=arm-none-eabi-gcc
cmake --build build-arm
```

### Using it in your own project

Add `include/` to your include paths and compile the sources under `src/`:

```
include/
src/core/  src/mac/  src/link/  src/routing/  src/api/
```

## Quick start

```c
#include "treenet/treenet.h"

/* 1. Port (see Doc/eng/porting.md) */
static int      my_tx(const uint8_t *b, size_t n) { /* ... */ return 0; }
static uint32_t my_now(void) { /* monotonic ms */ return 0; }
static uint32_t my_rnd(void) { /* any PRNG */ return 0; }

static const treenet_port_t port = {
    .tx = my_tx, .now_ms = my_now, .rnd = my_rnd,
};

/* 2. Context and configuration */
static uint8_t ctx[4096];   /* >= treenet_context_size() */

static void on_recv(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                    size_t len, int16_t rssi, int8_t snr, uint8_t hops)
{
    /* handle a received datagram */
}

static treenet_t *node;

void app_init(void)
{
    treenet_config_t cfg = { 0 };
    cfg.addr = 0x1234;               /* unique, not 0 and not 0xFFFFFFFF */
    cfg.role = TREENET_ROLE_NODE;    /* exactly one node is MASTER */
    cfg.net_id = 1;
    cfg.reliable = true;
    cfg.on_recv = on_recv;

    node = treenet_init(ctx, sizeof ctx, &cfg, &port);
}

/* 3. From the radio receive handler */
void radio_rx_isr(const uint8_t *b, size_t n, int16_t rssi, int8_t snr)
{
    treenet_rx(node, b, n, rssi, snr);
}

/* 4. Main loop */
void app_loop(void)
{
    treenet_poll(node);          /* call every 10..100 ms */
}

/* 5. Sending */
void app_send(void)
{
    treenet_send(node, 0x0001, "hello", 5);   /* unicast */
    treenet_broadcast(node, "all", 3);        /* to everyone */
}
```

A complete working example: [`examples/desktop_mesh/main.c`](examples/desktop_mesh/main.c).

## Repository layout

```
treenity/
  include/treenet/     public headers (config, types, port, treenet)
  src/core/            utilities, ring buffer, timers, EWMA, context
  src/mac/             frames, dup-cache, time-on-air
  src/link/            beaconing, neighbour table, LQI
  src/routing/         DODAG, objective function, routes
  src/api/             lifecycle, poll, receive, forwarding
  sim/                 desktop network simulator
  tests/unit/          unit and scenario tests
  tests/fuzz/          parser and receive-path fuzzing
  examples/            example applications
  Doc/ru/  Doc/eng/    documentation (Russian / English)
  CMakeLists.txt  Makefile
```

## Testing

```sh
make test          # unit + scenario tests (1904 checks)
make fuzz          # fuzzing, 200000 iterations
```

The scenario tests cover network formation, rank ordering, unicast down/up,
broadcast, seamless reconfiguration on relay failure and tolerance to corrupted
frames. More: [Doc/eng/testing.md](Doc/eng/testing.md).

## Configuration

All parameters live in [`include/treenet/config.h`](include/treenet/config.h) and
can be overridden with macros. The key timing relation:

```
TREENET_BEACON_MAX_MS  <  TREENET_PARENT_TIMEOUT_MS  <  TREENET_NEIGHBOR_TIMEOUT_MS
```

| Parameter | Default |
|---|---|
| `TREENET_MTU` | 200 |
| `TREENET_ENABLE_FRAME_CRC` | 1 (frame CRC-16, +2 bytes) |
| `TREENET_MAX_NEIGHBORS` | 32 |
| `TREENET_MAX_ROUTES` | 128 (Master of 1000 nodes → 1024) |
| `TREENET_BEACON_MIN_MS` / `MAX_MS` | 3000 / 12000 |
| `TREENET_PARENT_HYSTERESIS_PCT` | 25 |

## Documentation

Full index: [Doc/eng/README.md](Doc/eng/README.md) (English) and
[Doc/ru/README.md](Doc/ru/README.md) (Russian).

| Document | About |
|---|---|
| [Doc/eng/architecture.md](Doc/eng/architecture.md) | layers, memory, time, ISR model |
| [Doc/eng/protocol.md](Doc/eng/protocol.md) | frame format and all algorithms |
| [Doc/eng/api.md](Doc/eng/api.md) | public API |
| [Doc/eng/porting.md](Doc/eng/porting.md) | implementing a port for your hardware |
| [Doc/eng/integration.md](Doc/eng/integration.md) | integrating into an application |
| [Doc/eng/simulator.md](Doc/eng/simulator.md) | network simulator |
| [Doc/eng/testing.md](Doc/eng/testing.md) | building and running tests |
| [Doc/eng/treenity-plan.md](Doc/eng/treenity-plan.md) | detailed development plan |

## Roadmap

- [x] Core: MAC, Link, Routing, API
- [x] Network simulator and tests
- [x] Tickless scheduling and the `LEAF` role
- [ ] Reference hardware port (SX1262 + ESP32/STM32) and field trials
- [ ] Non-storing mode for networks above 500 nodes
- [ ] Security (AES-CCM/ChaCha, key management)
- [ ] Power saving and duty-cycle control
- [ ] Channel hopping, QoS, multi-master

## License

Released under the MIT License. See [LICENSE](LICENSE).

Copyright (c) 2026 lantaris.

## Contributing

- Code style: C99, comments in English, warning-free build (`-Wall -Wextra`).
- Before submitting changes: `make test` and `make fuzz` must pass.
- Document new parameters and behaviour under `Doc/`.
