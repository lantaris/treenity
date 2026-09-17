# Portability: implementing a port

[Русский](../ru/porting.md) | **English**

The library never touches hardware directly. Everything it needs is expressed
through a `treenet_port_t` structure of function pointers (dependency
injection). This is the same approach used by NETSTACK in Contiki-NG and by the
RadioHead driver layer.

## 1. Mandatory minimum

```c
typedef struct {
    treenet_tx_fn           tx;       /* transmit a frame */
    treenet_now_fn          now_ms;   /* monotonic milliseconds */
    treenet_rnd_fn          rnd;      /* pseudo random number */
    /* ... optional members below ... */
} treenet_port_t;
```

Without these three, `treenet_init()` returns `NULL`.

### `tx(const uint8_t *buf, size_t len) -> int`

Hand the bytes to the modem as a single packet. The function should be
non-blocking or bounded in time. treenet performs CSMA/CA itself, but you may
report medium occupancy through `channel_free`.

### `now_ms(void) -> uint32_t`

Monotonic milliseconds that wrap around freely. The library handles the 49 day
wrap correctly.

### `rnd(void) -> uint32_t`

A cheap PRNG (xorshift/LFSR). It does not need to be cryptographically secure —
only backoff jitter and beacon scheduling use it.

## 2. Optional functions

| Function | Purpose | If `NULL` |
|---|---|---|
| `channel_free()` | CAD: `true` when the medium is free | assumed always free |
| `set_radio(cfg)` | change SF/BW/power | changes are ignored |
| `critical_enter()/exit()` | protect the receive ring buffer | no protection |
| `log(level,msg)` | diagnostics output | logging disabled |

## 3. Receive: ISR -> `treenet_rx`

From the radio receive handler call:

```c
void radio_rx_isr(const uint8_t *data, size_t len, int16_t rssi, int8_t snr)
{
    treenet_rx(node, data, len, rssi, snr); /* only copies into a buffer */
}
```

`treenet_rx` does no heavy work and is safe in an interrupt. If the interrupt
can preempt `treenet_poll` while it works with the buffer, provide
`critical_enter/exit` — they are used around the buffer write.

## 4. Example port (SX126x + HAL)

```c
static treenet_t *g_node;   /* set after treenet_init */

static int my_tx(const uint8_t *buf, size_t len)
{
    /* A CAD check is not required: treenet asks channel_free() */
    return sx126x_transmit(buf, len) == 0 ? 0 : -1;
}

static uint32_t my_now(void)      { return hal_millis(); }
static uint32_t my_rnd(void)      { return hal_rng32(); }

static bool my_channel_free(void)
{
    return sx126x_cad() == CAD_FREE;
}

static void my_set_radio(const treenet_radio_cfg_t *cfg)
{
    sx126x_set_sf_bw(cfg->spreading_factor, cfg->bandwidth_hz);
    sx126x_set_tx_power(cfg->tx_power_dbm);
}

static void my_crit_enter(void) { hal_irq_disable(); }
static void my_crit_exit(void)  { hal_irq_enable();  }
static void my_log(int level, const char *msg) { uart_printf("[%d] %s\n", level, msg); }

static const treenet_port_t port = {
    .tx = my_tx, .now_ms = my_now, .rnd = my_rnd,
    .channel_free = my_channel_free, .set_radio = my_set_radio,
    .critical_enter = my_crit_enter, .critical_exit = my_crit_exit,
    .log = my_log,
};

/* In the radio receive handler: */
void radio_on_rx(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr)
{
    treenet_rx(g_node, buf, len, rssi, snr);
}
```

## 5. Node configuration

```c
static uint8_t ctx[4096]; /* >= treenet_context_size() */
static treenet_t *g_node_storage;

void treenet_setup(void)
{
    treenet_config_t cfg = { 0 };
    cfg.addr = read_unique_id();       /* NOT 0 and NOT 0xFFFFFFFF */
    cfg.role = TREENET_ROLE_NODE;      /* or MASTER */
    cfg.net_id = 1;
    cfg.reliable = true;               /* hop-by-hop ACK for unicast */
    cfg.on_recv = app_on_recv;
    cfg.on_event = app_on_event;
    cfg.radio.spreading_factor = 9;
    cfg.radio.bandwidth_hz = 125000;
    cfg.radio.coding_rate = 1;
    cfg.radio.tx_power_dbm = 14;

    g_node = treenet_init(ctx, sizeof ctx, &cfg, &port);
    g_node_storage = g_node;
}
```

### Context size

`treenet_context_size()` returns the exact size at run time. For a static buffer
either use a size that is definitely large enough (for example 4096 bytes) and
pass it to `treenet_init`, or obtain the size once and print it while porting.
The size depends on the values in `config.h` (neighbour table, route table, MTU
and so on).

## 6. Main loop

```c
for (;;) {
    treenet_poll(g_node);   /* call every 10..100 ms */
    app_do_work();
}
```

Do not call `treenet_poll` from an interrupt.

## 7. Integrity check

With `TREENET_ENABLE_FRAME_CRC = 1` (the default) the library **itself** checks
the integrity of every frame with CRC-16 and drops corrupted ones — the port
does not have to do anything for this. If the modem already verifies a CRC at
the physical layer (for example LoRa), that remains the first line of defence,
but the library does not trust it and checks the frame again.

The port must not strip the CRC trailer from a received frame: pass the full
frame, including the last 2 bytes, to `treenet_rx`. Likewise `port.tx` receives
the whole frame with the CRC already computed.

The CRC setting must match on every node of the network — otherwise they cannot
understand each other (document this when deploying).

## 8. Key rules

1. **Time must advance.** Without a monotonic `now_ms`, neither beacons nor
   timeouts work.
2. **`treenet_poll` regularly.** A rare call delays retransmissions and timer
   processing.
3. **`rssi`/`snr` must be real.** Parent selection depends on them. If the modem
   cannot report SNR, at least provide a plausible RSSI.
4. **The address must be unique** within the `net_id`.
5. **Keep the timings consistent.** If you change `TREENET_BEACON_MAX_MS`, make
   sure `TREENET_PARENT_TIMEOUT_MS` and `TREENET_NEIGHBOR_TIMEOUT_MS` are larger.
6. **One instance, one context.** Do not share a context between tasks without
   external synchronisation.
