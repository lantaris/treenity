# treenity

**Русский** | [English](README.md)

Портируемая mesh-сеть на C99 с единым корневым узлом **Master**. Библиотека
полностью абстрагирована от микроконтроллеров и радио: всё взаимодействие с
железом идёт через пользовательский порт (HAL). Сеть сама строится и
**бесшовно перестраивается** под лучшее качество связи, используя RSSI/SNR
каждого принятого пакета.

- Язык: **C99**, без динамической памяти, без зависимости от RTOS/ОС
- Транспорт: **LoRa** (и любой другой модем через порт)
- Топология: **Master-rooted DODAG** в духе RPL
- Масштаб: 100–1000 узлов на один Master
- Версия: **0.1.0**

---

## Содержание

- [Возможности](#возможности)
- [Архитектура](#архитектура)
- [Требования](#требования)
- [Сборка и установка](#сборка-и-установка)
- [Быстрый старт](#быстрый-старт)
- [Структура репозитория](#структура-репозитория)
- [Тестирование](#тестирование)
- [Конфигурация](#конфигурация)
- [Документация](#документация)
- [Дорожная карта](#дорожная-карта)
- [Лицензия](#лицензия)
- [Вклад](#вклад)

---

## Возможности

- **Абстракция от железа.** Минимум порта — три функции (`tx`, `now_ms`,
  `rnd`); остальное опционально. Один и тот же код работает на любом MCU и на
  хосте.
- **Статическая память.** Никакого `malloc`: все таблицы фиксированного размера
  задаются в `config.h`.
- **Кооперативная модель.** Неблокирующий `treenet_poll()`; приём
  (`treenet_rx`) безопасен для вызова из прерывания.
- **Lock-free приём из ISR.** Кольцевой буфер приёма — lock-free
  single-producer/single-consumer, поэтому `treenet_rx` не требует критических
  секций (один производитель, один потребитель).
- **Качество связи по RSSI/SNR.** EWMA RSSI/SNR, PDR по beacon-ам, ETX и
  композитная стоимость линка; значения доступны приложению.
- **Умный выбор родителя.** Целевая функция + гистерезис + dwell-time
  устраняют флаппинг и обеспечивают быструю реконфигурацию.
- **Роли участников.** `MASTER` (корень), `NODE`/`REPEATER` (роутеры) и `LEAF`
  (конечное устройство: отправляет и принимает свои данные, но не пересылает
  чужой трафик и не может быть родителем).
- **Managed flooding** для широковещательных сообщений с приоритетом по SNR.
- **Надёжность.** Hop-by-hop ACK и ретрансляции; надёжное распространение
  маршрутов (DAO). ACK принимается от любого соседа, переславшего кадр, а
  next hop, переставший подтверждать, обнаруживается за секунды (быстрый ремонт).
- **Контроль целостности.** CRC-16 каждого кадра (по умолчанию включён):
  повреждённые кадры отбрасываются до обработки, состояние сети не меняется.
- **Фрагментация** датаграмм, превышающих MTU.
- **Учёт времени в эфире** по формуле Semtech AN1200.13.
- **Tickless-планирование.** Опциональный `port.timer_arm()` сообщает
  пользователю, через сколько разбудить MCU; периодические процессы выражены
  вычисляемыми дедлайнами.
- **Desktop-симулятор** сети с воспроизводимыми сценариями и сбором метрик.

## Архитектура

```
┌───────────────────────────────────────────────────────┐
│ Приложение: treenet_send / broadcast / колбэки         │
├───────────────────────────────────────────────────────┤
│ Routing : DODAG, Rank, Objective Function, DAO, routes │
├───────────────────────────────────────────────────────┤
│ Link    : beaconing (Trickle), соседи, LQI (RSSI/SNR)  │
├───────────────────────────────────────────────────────┤
│ MAC     : кадры, CSMA/CA, dup-cache, ACK, фрагментация │
├───────────────────────────────────────────────────────┤
│ Core    : ring-buffer, таймеры, EWMA, события          │
├───────────────────────────────────────────────────────┤
│ PORT    : tx / now_ms / rnd / ... (реализует пользов.) │
└───────────────────────────────────────────────────────┘
```

Подробнее: [Doc/ru/architecture.md](Doc/ru/architecture.md),
[Doc/ru/protocol.md](Doc/ru/protocol.md).

## Требования

- Компилятор C99 (проверено на GCC и `arm-none-eabi-gcc`).
- Для ядра: только `stdint.h`, `stddef.h`, `stdbool.h`, `string.h`.
- Для сборки тестов/симулятора/примера: CMake ≥ 3.13 **или** GNU Make, и
  стандартная библиотека C (`libm`).

## Сборка и установка

### CMake (рекомендуется)

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Собираются цели: `treenet` (ядро), `treenet_sim` (симулятор),
`treenet_tests`, `treenet_fuzz`, `treenet_example`.

### Make

```sh
make            # ядро -> build/libtreenet.a
make test       # собрать и запустить тесты
make run        # собрать и запустить пример
make fuzz       # fuzzing
make repeaters  # стресс-отчёт: 1 Master + 30 повторителей
make stress     # многопоточные тесты конкурентности
```

### Кросс-сборка под ARM

```sh
cmake -S . -B build-arm -DCMAKE_C_COMPILER=arm-none-eabi-gcc
cmake --build build-arm
```

### Подключение к своему проекту

Добавьте `include/` в пути поиска заголовков и соберите исходники из `src/`:

```
include/
src/core/  src/mac/  src/link/  src/routing/  src/api/
```

## Быстрый старт

```c
#include "treenet/treenet.h"

/* 1. Порт (см. Doc/ru/porting.md) */
static int      my_tx(const uint8_t *b, size_t n) { /* ... */ return 0; }
static uint32_t my_now(void) { /* монотонные мс */ return 0; }
static uint32_t my_rnd(void) { /* любой ГПСЧ */ return 0; }

static const treenet_port_t port = {
    .tx = my_tx, .now_ms = my_now, .rnd = my_rnd,
};

/* 2. Контекст и конфигурация */
static uint8_t ctx[4096];   /* >= treenet_context_size() */

static void on_recv(treenet_t *t, treenet_addr_t src, const uint8_t *data,
                    size_t len, int16_t rssi, int8_t snr, uint8_t hops)
{
    /* обработка принятой датаграммы */
}

static treenet_t *node;

void app_init(void)
{
    treenet_config_t cfg = { 0 };
    cfg.addr = 0x1234;               /* уникальный, не 0 и не 0xFFFFFFFF */
    cfg.role = TREENET_ROLE_NODE;    /* ровно один узел — MASTER */
    cfg.net_id = 1;
    cfg.reliable = true;
    cfg.on_recv = on_recv;

    node = treenet_init(ctx, sizeof ctx, &cfg, &port);
}

/* 3. Из обработчика приёма радио */
void radio_rx_isr(const uint8_t *b, size_t n, int16_t rssi, int8_t snr)
{
    treenet_rx(node, b, n, rssi, snr);
}

/* 4. Главный цикл */
void app_loop(void)
{
    treenet_poll(node);          /* вызывать каждые 10..100 мс */
}

/* 5. Отправка */
void app_send(void)
{
    treenet_send(node, 0x0001, "hello", 5);   /* unicast */
    treenet_broadcast(node, "all", 3);        /* всем */
}
```

Полный рабочий пример: [`examples/desktop_mesh/main.c`](examples/desktop_mesh/main.c).

## Структура репозитория

```
treenity/
  include/treenet/     публичные заголовки (config, types, port, treenet)
  src/core/            утилиты, ring-buffer, таймеры, EWMA, контекст
  src/mac/             кадры, dup-cache, время в эфире
  src/link/            beaconing, таблица соседей, LQI
  src/routing/         DODAG, целевая функция, маршруты
  src/api/             жизненный цикл, poll, приём, пересылка
  sim/                 desktop-симулятор сети
  tests/unit/          юнит- и сценарные тесты
  tests/fuzz/          fuzzing парсера и пути приёма
  examples/            примеры приложений
  Doc/ru/  Doc/eng/    документация (русский / английский)
  CMakeLists.txt  Makefile
```

## Тестирование

```sh
make test          # юнит + сценарные тесты (1904 проверки)
make fuzz          # fuzzing 200000 итераций
```

Сценарные тесты проверяют формирование сети, порядок Rank, unicast вниз/вверх,
broadcast, бесшовное перестроение при отказе ретранслятора и устойчивость к
повреждённым кадрам. Подробнее: [Doc/ru/testing.md](Doc/ru/testing.md).

## Конфигурация

Все параметры — в [`include/treenet/config.h`](include/treenet/config.h) и
переопределяются макросами. Ключевое соотношение таймингов:

```
TREENET_BEACON_MAX_MS  <  TREENET_PARENT_TIMEOUT_MS  <  TREENET_NEIGHBOR_TIMEOUT_MS
```

| Параметр | По умолчанию |
|---|---|
| `TREENET_MTU` | 200 |
| `TREENET_ENABLE_FRAME_CRC` | 1 (CRC-16 кадра, +2 байта) |
| `TREENET_MAX_NEIGHBORS` | 32 |
| `TREENET_MAX_ROUTES` | 128 (для Master 1000 узлов → 1024) |
| `TREENET_BEACON_MIN_MS` / `MAX_MS` | 3000 / 12000 |
| `TREENET_PARENT_HYSTERESIS_PCT` | 25 |

## Документация

Полный индекс — [Doc/ru/README.md](Doc/ru/README.md) (русский) и
[Doc/eng/README.md](Doc/eng/README.md) (English).

| Документ | О чём |
|---|---|
| [Doc/ru/architecture.md](Doc/ru/architecture.md) | слои, память, время, модель ISR |
| [Doc/ru/protocol.md](Doc/ru/protocol.md) | формат кадра и все алгоритмы |
| [Doc/ru/api.md](Doc/ru/api.md) | публичный API |
| [Doc/ru/porting.md](Doc/ru/porting.md) | реализация порта под своё железо |
| [Doc/ru/integration.md](Doc/ru/integration.md) | встраивание в приложение |
| [Doc/ru/simulator.md](Doc/ru/simulator.md) | симулятор сети |
| [Doc/ru/testing.md](Doc/ru/testing.md) | сборка и запуск тестов |
| [Doc/ru/treenity-plan.md](Doc/ru/treenity-plan.md) | детальный план разработки |

## Дорожная карта

- [x] Ядро: MAC, Link, Routing, API
- [x] Симулятор сети и тесты
- [x] Tickless-планирование и роль `LEAF`
- [ ] Аппаратный эталонный порт (SX1262 + ESP32/STM32) и полевые испытания
- [ ] Non-storing mode для сетей > 500 узлов
- [ ] Безопасность (AES-CCM/ChaCha, управление ключами)
- [ ] Энергосбережение и контроль duty-cycle
- [ ] Channel hopping, QoS, multi-master

## Лицензия

Проект распространяется под лицензией MIT. См. [LICENSE](LICENSE).

Copyright (c) 2026 lantaris.

## Вклад

- Стиль кода: C99, комментарии на английском, сборка без предупреждений
  (`-Wall -Wextra`).
- Перед отправкой изменений: `make test` и `make fuzz` должны проходить.
- Документируйте новые параметры и поведение в `Doc/`.
