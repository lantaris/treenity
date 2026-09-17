# Integration into an application

[Русский](../ru/integration.md) | **English**

## 1. Bare-metal (super loop)

```c
int main(void)
{
    hw_init();
    treenet_setup();                 /* see porting.md */
    uint32_t next = millis();

    for (;;) {
        if ((uint32_t)(millis() - next) >= 20) { /* every 20 ms */
            next += 20;
            treenet_poll(g_node);
        }
        app_loop();
    }
}
```

Receiving from the radio ISR is done through `treenet_rx` (see porting.md).

## 2. RTOS (for example FreeRTOS)

Dedicate a task to `treenet_poll`. Never call `treenet_poll` from several tasks
at once.

```c
static void treenet_task(void *arg)
{
    (void)arg;
    for (;;) {
        treenet_poll(g_node);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

`treenet_rx` may be called from an ISR (it is `...FromISR`-safe because it only
copies). The receive ring is **lock-free**, so no critical section is needed;
`treenet_rx` is called from one context only.

## 2.1. Tickless (power saving)

Implement `port.timer_arm` (see porting.md) and the MCU will sleep until the
library's nearest deadline instead of waking on a fixed period. Radio reception
is a separate wake source.

```c
static void my_timer_arm(uint32_t delay_ms)
{
    if (delay_ms == UINT32_MAX) return;   /* nothing scheduled: don't arm */
    if (delay_ms == 0) delay_ms = 1;      /* due now: poll again immediately */
    rtc_alarm_arm_ms(delay_ms);           /* one-shot */
}

/* Wake up from the library's timer. */
void rtc_alarm_isr(void) { wake_main_loop(); }

/* Wake up from the radio (DIO): read the frame and hand it to the library. */
void radio_dio_isr(void)
{
    radio_read_frame(&buf, &len, &rssi, &snr);
    treenet_rx(g_node, buf, len, rssi, snr);
    wake_main_loop();
}

/* Main loop: */
for (;;) {
    treenet_poll(g_node);   /* re-arms timer_arm at the end */
    enter_sleep();          /* STOP until the timer or DIO wakes us */
}
```

If `timer_arm` is not implemented (`NULL`), use the periodic polling variant
from section 1.

## 2.2. Concurrency

- `treenet_poll()` — from **one** context (the main loop or one RTOS task). Do
  not call it from a timer ISR: set a flag and poll from the main loop (see 2.1).
- `treenet_rx()` — from the modem receive handler (ISR). The receive ring is
  **lock-free**, no critical section is needed; call it from one context only
  (the sole producer).
- `treenet_send`/`treenet_broadcast` and introspection — from the same context
  as `poll`.
- Instances are independent (no global state); when driven from different
  threads, port thread-safety is your responsibility.

## 3. Receiving data

```c
static void app_on_recv(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                        size_t len, int16_t rssi, int8_t snr, uint8_t hops)
{
    /* rssi/snr are the last hop quality, hops is the number of hops travelled */
    handle_message(src, data, len);
}
```

## 4. Sending

```c
treenet_send(g_node, dest_addr, payload, payload_len);   /* unicast */
treenet_broadcast(g_node, payload, payload_len);          /* to everyone */
```

If `cfg.reliable == true`, unicast uses hop-by-hop ACK and retransmissions.

## 5. Diagnostics and monitoring

```c
const treenet_stats_t *st = treenet_stats(g_node);
printf("tx=%u rx=%u retx=%u parent_changes=%u\n",
       st->frames_tx, st->frames_rx, st->retransmissions, st->parent_changes);

treenet_neighbor_info_t nb[16];
size_t n = treenet_neighbors(g_node, nb, 16);
for (size_t i = 0; i < n; i++) {
    printf("  %u rssi=%d snr=%d etx=%u cost=%u%s\n",
           nb[i].addr, nb[i].lq.rssi_dbm, nb[i].lq.snr_db, nb[i].lq.etx_q8,
           nb[i].lq.link_cost, nb[i].is_parent ? " (parent)" : "");
}
```

## 6. Node roles

- Exactly **one** node in the network is configured as `TREENET_ROLE_MASTER`
  (the root). It is usually a mains-powered gateway.
- The rest are `TREENET_ROLE_NODE`.
- `TREENET_ROLE_REPEATER` is useful for fixed relays: it has priority during
  flooding.
- `TREENET_ROLE_LEAF` is for sensors / end devices: they send and receive their
  own data but never forward other traffic, never rebroadcast floods and are
  never chosen as parents (they may sleep their radio).

## 7. Tuning for scale

| Scenario | What to change |
|---|---|
| Few nodes, save RAM | lower `TREENET_MAX_NEIGHBORS`, `TREENET_MAX_ROUTES`, `TREENET_RX_RING_BYTES` |
| Master for 1000 nodes | `TREENET_MAX_ROUTES = 1024` (Master only) |
| Save airtime | increase `TREENET_BEACON_MAX_MS` (and the timeouts!), lower `TREENET_FLOOD_CW_MS` |
| Fast reconvergence | lower `TREENET_BEACON_MAX_MS`, `TREENET_PARENT_TIMEOUT_MS` |
| More range / less speed | SF/BW in `cfg.radio` (affects time-on-air) |

> Always check: `TREENET_BEACON_MAX_MS < TREENET_PARENT_TIMEOUT_MS <
> TREENET_NEIGHBOR_TIMEOUT_MS`.

**The defaults are the "balanced" profile:** beacon 3→12 s, parent timeout 36 s
(failure detection ~36 s). Faster reaction needs a shorter beacon interval, but
the airtime load grows as `N × ToA / interval` (beacon ToA ≈ 80 ms at SF7,
≈ 280 ms at SF9). Rough guidance at a ~30 % beacon airtime budget:

| Interval | SF7 | SF9 |
|---|---|---|
| 12 s | ~45 nodes | ~13 nodes |
| 20 s | ~75 nodes | ~21 nodes |

For larger networks raise `BEACON_MAX_MS` (and the timeouts) or narrow the BW
(250/500 kHz reduce ToA). Trickle without redundancy suppression does not scale
to thousands of nodes — that is a separate task.

**Fast repair.** `TREENET_ACK_FAIL_THRESHOLD` (default 2) is how many reliable
frames without an ACK from the next hop mark it "suspect";
`TREENET_LINK_SUSPECT_MS` (30000) is how long it is excluded from parent
selection. Nodes that are actively sending switch in seconds instead of waiting
for `PARENT_TIMEOUT`.

## 8. Common integration mistakes

| Symptom | Cause |
|---|---|
| Node never joins | no beacons (wrong `now_ms`, rare `poll`), wrong `net_id`, no Master |
| Constant parent changes | `BEACON_MAX_MS` >= timeouts; bad `rssi`/`snr` |
| Downward data never arrives | the DAO does not reach the Master (check reliability, `hop_limit`) |
| High traffic | beacons too frequent; reduce overhead |
| `treenet_init` returned `NULL` | not enough memory, mandatory port functions missing, invalid address |

## 9. Coexisting networks

Several networks can operate in the same area. They are separated by `net_id`
(16 bits): frames with another `net_id` are ignored **before** link metrics are
updated, so foreign nodes never even appear in the neighbour table.

Keep in mind:

- **The channel is shared.** `net_id` does not prevent collisions or airtime
  contention — the networks still interfere physically. For real isolation
  separate them by **frequency** or by **LoRa sync word** (configured on the
  port side; the library does not need it).
- **One instance = one network.** A node belongs to exactly one network
  (`net_id` plus its own context). To have a node in two networks, run **two
  instances** (preferably two radios; on a single radio the port must dispatch
  received frames by `net_id` and serialise transmission).
- **Each network has its own Master.**

This is verified by `test_coexisting_networks` (two networks in one area: nodes
join only their own Master and a foreign broadcast is not delivered).
