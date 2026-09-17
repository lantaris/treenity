# API treenity

**Русский** | [English](../eng/api.md)

Публичный заголовок: `include/treenet/treenet.h`.
Вспомогательные: `include/treenet/types.h`, `include/treenet/port.h`,
`include/treenet/config.h`.

## 1. Жизненный цикл

### `size_t treenet_context_size(void)`

Размер контекста в байтах. Используйте для статического буфера.

### `treenet_t *treenet_init(void *storage, size_t storage_size, const treenet_config_t *cfg, const treenet_port_t *port)`

Инициализирует экземпляр в памяти вызывающего. Возвращает `NULL`, если:

- `storage == NULL` или `storage_size < treenet_context_size()`;
- `cfg`/`port` == `NULL`;
- не заданы обязательные `port.tx`, `port.now_ms`, `port.rnd`;
- `cfg.addr` равен `0` или `0xFFFFFFFF`.

Память должна быть выровнена минимум по 8 байт и жить всё время работы узла.

### `void treenet_poll(treenet_t *t)`

Двигает состояние: приём, beacon-и, маршрутизация, передача, события.
Неблокирующий. Вызывайте из главного цикла или по таймеру (10–100 мс).

### `int treenet_rx(treenet_t *t, const uint8_t *buf, size_t len, int16_t rssi, int8_t snr)`

Передать принятый кадр в библиотеку. Безопасен для вызова из ISR (только
копирует кадр в кольцевой буфер). `rssi` (dBm) и `snr` (dB) относятся к связи с
непосредственным отправителем. Возвращает `0` при успехе, отрицательное — если
буфер полон или аргументы неверны.

## 2. Конфигурация

```c
typedef struct {
    treenet_addr_t addr;      /* уникальный адрес узла */
    treenet_role_t role;      /* MASTER / NODE / REPEATER */
    uint16_t       net_id;    /* логический id сети */

    void (*on_recv)(treenet_t*, treenet_addr_t src, const uint8_t *data,
                    size_t len, int16_t rssi, int8_t snr, uint8_t hops);
    void (*on_event)(treenet_t*, treenet_event_t ev, void *arg);

    void *user;               /* произвольный указатель, возвращается в колбэках */
    bool  reliable;           /* требовать hop-by-hop ACK для unicast */
    treenet_radio_cfg_t radio;/* опционально: параметры LoRa для расчёта ToA */
} treenet_config_t;
```

- `on_recv` вызывается при получении полной датаграммы; `rssi`/`snr` — качество
  кадра, доставившего датаграмму (последний переход).
- `on_event` — асинхронные события. Для `PARENT_CHANGED` `arg` указывает на
  новый адрес родителя.
- `radio.spreading_factor != 0` включает расчёт времени в эфире по LoRa-формуле.

## 3. Передача данных

### `int treenet_send(treenet_t *t, treenet_addr_t dst, const void *data, size_t len)`

Одноадресная отправка. Возвращает:

- `0` — поставлено в очередь;
- `-1` — неверные аргументы / не инициализировано;
- `-2` — датаграмма больше `TREENET_MAX_DATAGRAM` при отключённой фрагментации;
- `-3` — нет маршрута к `dst`.

### `int treenet_broadcast(treenet_t *t, const void *data, size_t len)`

Широковещательная рассылка через managed flooding.

## 4. Интроспекция

| Функция | Возвращает |
|---|---|
| `treenet_addr(t)` | адрес узла |
| `treenet_role(t)` | роль |
| `treenet_parent(t)` | текущий родитель или `TREENET_ADDR_INVALID` |
| `treenet_rank(t)` | текущий Rank |
| `treenet_is_connected(t)` | `true`, если есть путь к Master |
| `treenet_stats(t)` | указатель на блок статистики |
| `treenet_neighbors(t, out, max)` | снимки соседей с LQI |
| `treenet_version()` | строку версии |

```c
typedef struct {
    treenet_addr_t         addr;
    treenet_link_quality_t lq;      /* rssi_dbm, snr_db, pdr_q8, etx_q8, link_cost */
    uint16_t               rank;
    uint32_t               age_ms;
    bool                   is_parent;
} treenet_neighbor_info_t;
```

## 5. События (`treenet_event_t`)

| Событие | Когда |
|---|---|
| `NETWORK_READY` | узел присоединился и имеет путь к Master |
| `JOINED` | родитель выбран впервые |
| `PARENT_CHANGED` | смена родителя |
| `NEIGHBOR_ADDED` / `NEIGHBOR_REMOVED` | обнаружен/потерян сосед |
| `ROUTE_LOST` | родитель потерян |
| `DISCONNECTED` | узел отключился от поддерева Master |
| `TX_DONE` / `TX_FAILED` | надёжный кадр подтверждён / исчерпал попытки |

## 6. Статистика (`treenet_stats_t`)

```c
uint32_t frames_tx, frames_rx, frames_dropped;
uint32_t retransmissions, beacons_tx, beacons_rx;
uint32_t parent_changes, datagrams_tx, datagrams_rx;
uint32_t airtime_ms;
```

## 7. Возвращаемые коды (сводка)

| Код | Значение |
|---|---|
| `0` | успех |
| `-1` | неверные аргументы / не инициализировано |
| `-2` | кадр/датаграмма не помещается |
| `-3` | нет маршрута |
