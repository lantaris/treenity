# treenity — documentation

[Русский](../ru/README.md) | **English**

**treenity** is a portable C99 library for building a mesh network around a
single root node, the **Master**. The library is fully abstracted from
microcontrollers and radios: all hardware interaction happens through a
user-supplied port (HAL). The primary goal is seamless reconfiguration of the
topology for the best possible link quality, using the RSSI/SNR of every
received frame.

## Contents

| Document | About |
|---|---|
| [architecture.md](architecture.md) | Overall architecture, layers, memory and time model |
| [protocol.md](protocol.md) | Frame format, MAC/Link/Routing algorithms, behaviour |
| [api.md](api.md) | Public API and callbacks |
| [porting.md](porting.md) | How to implement a port for your hardware |
| [integration.md](integration.md) | Integrating into an application (bare-metal, RTOS) |
| [simulator.md](simulator.md) | Desktop network simulator and scenarios |
| [testing.md](testing.md) | Building and running tests and fuzzing |
| [treenity-plan.md](treenity-plan.md) | Detailed development plan |

## Quick start

```c
#include "treenet/treenet.h"

/* 1. Implement the port (see porting.md) */
static treenet_port_t port = { .tx = my_tx, .now_ms = my_now, .rnd = my_rnd };

/* 2. Reserve a static context */
static uint8_t ctx[16384]; /* >= treenet_context_size(); ~11.4 KB by default */

/* 3. Initialise the node */
static treenet_config_t cfg = { .addr = 0x1234, .role = TREENET_ROLE_NODE,
                                .on_recv = on_recv, .on_event = on_event };
treenet_t *node = treenet_init(ctx, sizeof ctx, &cfg, &port);

/* 4. From the radio receive handler: */
treenet_rx(node, buf, len, rssi, snr);

/* 5. In the main loop: */
treenet_poll(node);

/* 6. Send data: */
treenet_send(node, 0x0001, data, len);
treenet_broadcast(node, data, len);
```

A complete working example: `examples/desktop_mesh/main.c`.

## Key properties

- **C99, no dynamic memory.** All tables are fixed size and configured in
  `include/treenet/config.h`.
- **No RTOS or OS dependency.** Non-blocking `treenet_poll()`.
- **Port built from function pointers.** Three functions are mandatory (`tx`,
  `now_ms`, `rnd`); the rest are optional.
- **RSSI/SNR of every received packet** are passed into the library, used for
  link quality estimation and exposed to the application in `on_recv`.
- **Master-rooted DODAG** (RPL-style) with strictly increasing Rank, a composite
  objective function, hysteresis and dwell-time to prevent flapping.
- **Managed flooding** for broadcasts with SNR-based priority.
- **Hop-by-hop ACK** and reliable delivery of control messages.
- **Frame integrity check** (CRC-16) so corrupted frames are dropped before
  processing.
- **Desktop simulator** for reproducible testing without hardware.
