# Встраивание в приложение

**Русский** | [English](../eng/integration.md)

## 1. Bare-metal (суперцикл)

```c
int main(void)
{
    hw_init();
    treenet_setup();                 /* см. porting.md */
    uint32_t next = millis();

    for (;;) {
        if ((uint32_t)(millis() - next) >= 20) { /* каждые 20 мс */
            next += 20;
            treenet_poll(g_node);
        }
        app_loop();
    }
}
```

Приём из ISR радио — через `treenet_rx` (см. porting.md).

## 2. RTOS (например, FreeRTOS)

Выделите отдельную задачу для `treenet_poll`. Не вызывайте `treenet_poll`
одновременно из нескольких задач.

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

`treenet_rx` можно вызывать из ISR (`...FromISR`-безопасно, т.к. только
копирует). Если ISR может прервать работу задачи, задайте
`critical_enter/exit` (например, `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`).

## 2.1. Tickless (энергосбережение)

Реализуйте `port.timer_arm` (см. porting.md) — и MCU будет спать до ближайшего
дедлайна библиотеки, а не просыпаться по фиксированному периоду. Приём радио —
отдельный источник пробуждения.

```c
static void my_timer_arm(uint32_t delay_ms)
{
    if (delay_ms == UINT32_MAX) return;   /* задач нет — не взводим */
    if (delay_ms == 0) delay_ms = 1;      /* назрело — опросить сразу */
    rtc_alarm_arm_ms(delay_ms);           /* one-shot */
}

/* Пробуждение по таймеру библиотеки. */
void rtc_alarm_isr(void) { wake_main_loop(); }

/* Пробуждение по радио (DIO): принять кадр и передать в библиотеку. */
void radio_dio_isr(void)
{
    radio_read_frame(&buf, &len, &rssi, &snr);
    treenet_rx(g_node, buf, len, rssi, snr);
    wake_main_loop();
}

/* Главный цикл: */
for (;;) {
    treenet_poll(g_node);   /* в конце перевзведёт timer_arm */
    enter_sleep();          /* STOP, пока не разбудит таймер или DIO */
}
```

Если `timer_arm` не реализован (`NULL`), используйте вариант с периодическим
опросом из раздела 1.

## 3. Приём данных

```c
static void app_on_recv(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                        size_t len, int16_t rssi, int8_t snr, uint8_t hops)
{
    /* rssi/snr — качество последнего перехода, hops — пройдено переходов */
    handle_message(src, data, len);
}
```

## 4. Отправка

```c
treenet_send(g_node, dest_addr, payload, payload_len);   /* unicast */
treenet_broadcast(g_node, payload, payload_len);          /* всем */
```

Если `cfg.reliable == true`, unicast использует hop-by-hop ACK и ретрансляции.

## 5. Диагностика и мониторинг

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

## 6. Роли узлов

- Ровно **один** узел в сети конфигурируется как `TREENET_ROLE_MASTER`
  (корень). Обычно это шлюз с питанием от сети.
- Остальные — `TREENET_ROLE_NODE`.
- `TREENET_ROLE_REPEATER` полезен для стационарных ретрансляторов: он имеет
  приоритет при flooding.
- `TREENET_ROLE_LEAF` — для датчиков/конечных устройств: они отправляют и
  принимают свои данные, но не пересылают чужой трафик, не ретранслируют
  flooding и не могут быть родителями (могут спать радио).

## 7. Настройка под масштаб

| Сценарий | Что менять |
|---|---|
| Мало узлов, экономия RAM | уменьшить `TREENET_MAX_NEIGHBORS`, `TREENET_MAX_ROUTES`, `TREENET_RX_RING_BYTES` |
| Master на 1000 узлов | `TREENET_MAX_ROUTES = 1024` (только для Master) |
| Экономия эфира | увеличить `TREENET_BEACON_MAX_MS` (и таймауты!), уменьшить `TREENET_FLOOD_CW_MS` |
| Быстрая реконвергенция | уменьшить `TREENET_BEACON_MAX_MS`, `TREENET_PARENT_TIMEOUT_MS` |
| Больше дальность/меньше скорость | SF/BW в `cfg.radio` (влияет на ToA) |

> Всегда проверяйте: `TREENET_BEACON_MAX_MS < TREENET_PARENT_TIMEOUT_MS <
> TREENET_NEIGHBOR_TIMEOUT_MS`.

## 8. Типичные ошибки интеграции

| Симптом | Причина |
|---|---|
| Узел не присоединяется | нет beacon-ов (неверный `now_ms`, редкий `poll`), неверный `net_id`, нет Master |
| Постоянные смены родителя | `BEACON_MAX_MS` >= таймаутов; плохие `rssi`/`snr` |
| Данные не доходят вниз | DAO не доходит до Master (проверьте надёжность, `hop_limit`) |
| Высокий трафик | слишком частые beacon-и; уменьшите overhead |
| `treenet_init` вернул `NULL` | мало памяти, не заданы обязательные функции порта, некорректный адрес |
