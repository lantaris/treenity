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
copies). If the ISR can preempt the task, provide `critical_enter/exit` (for
example `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`).

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

## 8. Common integration mistakes

| Symptom | Cause |
|---|---|
| Node never joins | no beacons (wrong `now_ms`, rare `poll`), wrong `net_id`, no Master |
| Constant parent changes | `BEACON_MAX_MS` >= timeouts; bad `rssi`/`snr` |
| Downward data never arrives | the DAO does not reach the Master (check reliability, `hop_limit`) |
| High traffic | beacons too frequent; reduce overhead |
| `treenet_init` returned `NULL` | not enough memory, mandatory port functions missing, invalid address |
