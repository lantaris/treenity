# treenity — документация

**Русский** | [English](../eng/README.md)

**treenity** — портируемая библиотека на C99 для построения mesh-сети вокруг
одного корневого узла **Master**. Библиотека полностью абстрагирована от
микроконтроллеров и радио: всё взаимодействие с железом выполняется через
пользовательский порт (HAL). Основной критерий — бесшовная реконфигурация
топологии ради максимального качества связи с учётом RSSI/SNR.

## Содержание

| Документ | О чём |
|---|---|
| [architecture.md](architecture.md) | Общая архитектура, слои, модель памяти и времени |
| [protocol.md](protocol.md) | Формат кадра, алгоритмы MAC/Link/Routing, порядок работы |
| [api.md](api.md) | Публичный API и колбэки |
| [porting.md](porting.md) | Как реализовать порт под своё железо |
| [integration.md](integration.md) | Встраивание в приложение (bare-metal, RTOS) |
| [simulator.md](simulator.md) | Desktop-симулятор сети и сценарии |
| [testing.md](testing.md) | Сборка и запуск тестов и fuzzing |
| [treenity-plan.md](treenity-plan.md) | Детальный план разработки |

## Быстрый старт

```c
#include "treenet/treenet.h"

/* 1. Реализуйте порт (см. porting.md) */
static treenet_port_t port = { .tx = my_tx, .now_ms = my_now, .rnd = my_rnd };

/* 2. Выделите статический контекст */
static uint8_t ctx[16384]; /* >= treenet_context_size(); ~11.4 КБ по умолчанию */

/* 3. Инициализируйте узел */
static treenet_config_t cfg = { .addr = 0x1234, .role = TREENET_ROLE_NODE,
                                .on_recv = on_recv, .on_event = on_event };
treenet_t *node = treenet_init(ctx, sizeof ctx, &cfg, &port);

/* 4. Из обработчика приёма радио: */
treenet_rx(node, buf, len, rssi, snr);

/* 5. В главном цикле: */
treenet_poll(node);

/* 6. Отправка: */
treenet_send(node, 0x0001, data, len);
treenet_broadcast(node, data, len);
```

Полный рабочий пример: `examples/desktop_mesh/main.c`.

## Ключевые свойства

- **C99, без динамической памяти.** Все таблицы фиксированного размера,
  размеры задаются в `include/treenet/config.h`.
- **Нет зависимости от RTOS и ОС.** Неблокирующий `treenet_poll()`.
- **Порт из указателей на функции.** Минимум — три функции (`tx`, `now_ms`,
  `rnd`), остальные опциональны.
- **RSSI/SNR каждого принятого пакета** передаются в библиотеку и используются
  для оценки качества связи, а также доступны приложению в `on_recv`.
- **Master-rooted DODAG** (в духе RPL) со строго возрастающим Rank,
  композитной целевой функцией, гистерезисом и dwell-time против флаппинга.
- **Управляемый flooding** для broadcast с приоритетом по SNR.
- **Hop-by-hop ACK** и надёжная доставка служебных сообщений.
- **Контроль целостности** (CRC-16): повреждённые кадры отбрасываются до
  обработки.
- **Desktop-симулятор** для воспроизводимого тестирования без железа.
