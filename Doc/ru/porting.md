# Портируемость: реализация порта

**Русский** | [English](../eng/porting.md)

Библиотека не обращается к железу напрямую. Всё, что нужно, выражается
структурой `treenet_port_t` из указателей на функции (dependency injection).
Это тот же приём, что NETSTACK в Contiki-NG и драйверный слой RadioHead.

## 1. Обязательный минимум

```c
typedef struct {
    treenet_tx_fn           tx;       /* передать кадр */
    treenet_now_fn          now_ms;   /* монотонные миллисекунды */
    treenet_rnd_fn          rnd;      /* псевдослучайное число */
    /* ... опциональные ниже ... */
} treenet_port_t;
```

Без этих трёх `treenet_init()` вернёт `NULL`.

### `tx(const uint8_t *buf, size_t len) -> int`

Передайте байты модему одним пакетом. Функция должна быть неблокирующей или
ограниченной по времени. CSMA/CA библиотека делает сама, но можно сообщать о
занятости эфира через `channel_free`.

### `now_ms(void) -> uint32_t`

Монотонные миллисекунды, свободно переполняющиеся. Библиотека корректно
обрабатывает переполнение через 49 дней.

### `rnd(void) -> uint32_t`

Дешёвый ГПСЧ (xorshift/LFSR). Криптостойкость не нужна — только джиттер
backoff и расписание beacon-ов.

## 2. Опциональные функции

| Функция | Назначение | Если `NULL` |
|---|---|---|
| `channel_free()` | CAD: `true`, если эфир свободен | считается, что всегда свободен |
| `set_radio(cfg)` | смена SF/BW/мощности | изменения игнорируются |
| `critical_enter()/exit()` | защита кольцевого буфера приёма | без защиты |
| `log(level,msg)` | вывод диагностики | лог отключён |
| `timer_arm(delay_ms)` | взвести таймер пробуждения (tickless) | `treenet_poll()` вызываете сами |

## 3. Приём: ISR → `treenet_rx`

Из обработчика приёма радио вызовите:

```c
void radio_rx_isr(const uint8_t *data, size_t len, int16_t rssi, int8_t snr)
{
    treenet_rx(node, data, len, rssi, snr); /* только копирует в буфер */
}
```

`treenet_rx` не выполняет тяжёлой работы и безопасен в прерывании. Если
прерывание может прервать `treenet_poll` в момент работы с буфером, задайте
`critical_enter/exit` — они используются вокруг записи в буфер.

## 4. Пример порта (SX126x + HAL)

```c
static treenet_t *g_node;   /* устанавливается после treenet_init */

static int my_tx(const uint8_t *buf, size_t len)
{
    /* Проверка CAD не обязательна: treenet спрашивает channel_free() */
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

/* В обработчике приёма радио: */
void radio_on_rx(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr)
{
    treenet_rx(g_node, buf, len, rssi, snr);
}
```

## 5. Конфигурация узла

```c
static uint8_t ctx[4096]; /* >= treenet_context_size() */
static treenet_t *g_node_storage;

void treenet_setup(void)
{
    treenet_config_t cfg = { 0 };
    cfg.addr = read_unique_id();       /* НЕ 0 и НЕ 0xFFFFFFFF */
    cfg.role = TREENET_ROLE_NODE;      /* или MASTER */
    cfg.net_id = 1;
    cfg.reliable = true;               /* hop-by-hop ACK для unicast */
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

### Размер контекста

`treenet_context_size()` возвращает точный размер во время выполнения. Для
статического буфера либо используйте заведомо достаточный размер (например,
4096 байт) и передайте его в `treenet_init`, либо получите размер один раз и
распечатайте при портировании. Размер зависит от значений в `config.h`
(таблицы соседей, маршрутов, MTU и т. д.).

## 6. Главный цикл и энергосбережение

Библиотека **неблокирующая** и не использует аппаратные таймеры. Два способа
«тикать» её:

**A. Периодический опрос (просто, без энергосбережения):**
```c
for (;;) {
    treenet_poll(g_node);   /* вызывать каждые 10..100 мс */
    app_do_work();
}
```

**B. Tickless (рекомендуется, для батарейных узлов):** реализуйте
`port.timer_arm(delay_ms)`. Библиотека в конце каждого `treenet_poll()`
вызовет его со временем до ближайшего дедлайна (beacon, обслуживание,
передача/ACK, таймаут родителя). Взведите **one-shot** таймер и по его
срабатыванию снова вызовите `treenet_poll()`.

```c
static void my_timer_arm(uint32_t delay_ms)
{
    if (delay_ms == UINT32_MAX) return;      /* задач нет — не взводим */
    if (delay_ms == 0) delay_ms = 1;         /* назрело — опросить сразу */
    rtc_alarm_arm_ms(delay_ms);              /* ваш RTC/таймер */
}
```
По срабатыванию таймера: `treenet_poll(g_node);`

Важно:
- `timer_arm` — это **установить/заменить**, а не «добавить»: старое значение
  перезаписывается.
- **Приём радио — отдельный источник пробуждения.** Таймер покрывает только
  дедлайны библиотеки. DIO-прерывание модема должно будить MCU и вызывать
  `treenet_rx(...)`, после чего — `treenet_poll()`.
- `now_ms` должен идти во сне (RTC-счётчик), иначе дедлайны «замерзнут».
- Если `timer_arm == NULL`, библиотека ведёт себя как раньше — вы опрашиваете её
  сами (вариант A).

Не вызывайте `treenet_poll` из прерывания (единственная ISR-безопасная функция —
`treenet_rx`).

## 7. Контроль целостности

При `TREENET_ENABLE_FRAME_CRC = 1` (по умолчанию) библиотека **сама** проверяет
целостность каждого кадра по CRC-16 и отбрасывает повреждённые — порт не обязан
ничего для этого делать. Если модем уже проверяет CRC на физическом уровне
(например, LoRa), это остаётся первой линией защиты, но библиотека ей не
доверяет и проверяет кадр повторно.

Порт не должен удалять CRC-трейлер из принятого кадра: передавайте в
`treenet_rx` полный кадр, включая последние 2 байта. Аналогично `port.tx`
получает кадр целиком, с уже посчитанным CRC.

Настройка CRC должна совпадать на всех узлах сети — иначе они не поймут друг
друга (задокументируйте это при развёртывании).

## 8. Ключевые правила

1. **Время должно идти.** Без монотонного `now_ms` не работают ни beacon-и, ни
   таймауты.
2. **`treenet_poll` — регулярно.** Редкий вызов задерживает ретрансляции и
   обработку таймеров.
3. **`rssi`/`snr` — реальные.** От них зависит выбор родителя. Если модем не
   умеет SNR, передавайте хотя бы правдоподобный RSSI.
4. **Адрес уникален** в пределах `net_id`.
5. **Согласуйте тайминги.** Если меняете `TREENET_BEACON_MAX_MS`, убедитесь, что
   `TREENET_PARENT_TIMEOUT_MS` и `TREENET_NEIGHBOR_TIMEOUT_MS` больше него.
6. **Один экземпляр — один контекст.** Не разделяйте контекст между задачами
   без внешней синхронизации.
