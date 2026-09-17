# Симулятор сети

**Русский** | [English](../eng/simulator.md)

Desktop-симулятор (`sim/sim.h`, `sim/sim.c`) позволяет прогонять полноценную
mesh-сеть без радио. Он используется тестами и примером.

## 1. Что моделируется

- **Размещение узлов** на плоскости (метры).
- **Общий радиоканал**: передача одного узла слышна всем в пределах дальности.
- **Потери по расстоянию**: log-distance path loss + вероятность потери в
  зависимости от запаса до чувствительности.
- **Жёсткая дальность** (`sim_set_range`): за её пределами приём невозможен.
- **Коллизии**: если узел в одном миллисекундном шаге слышит более одного
  кадра, все они считаются потерянными.
- **Виртуальное время**: шаг 10 мс, воспроизводимость по seed.

## 2. Создание и запуск

```c
#include "sim.h"

sim_t *s = sim_create(seed, tx_power_dbm, sensitivity_dbm, noise_floor_dbm);
sim_set_path_loss(s, ref_loss_db, exponent);   /* модель потерь */
sim_set_range(s, range_m);                     /* жёсткая дальность */

sim_add_node(s, /*addr*/ 1, TREENET_ROLE_MASTER, /*x*/ 0.0,  /*y*/ 0.0,  /*reliable*/ true);
sim_add_node(s, /*addr*/ 2, TREENET_ROLE_NODE,   /*x*/ 200.0, /*y*/ 0.0, true);

sim_run(s, 120000);   /* прогнать 120 секунд виртуального времени */

sim_node_t *n = sim_find(s, 2);
printf("rank=%u parent=%u connected=%d\n",
       treenet_rank(n->net), treenet_parent(n->net),
       treenet_is_connected(n->net));

sim_destroy(s);
```

## 3. API

| Функция | Назначение |
|---|---|
| `sim_create(seed, tx_dbm, sens_dbm, noise_dbm)` | создать симулятор |
| `sim_destroy(s)` | освободить ресурсы и контексты узлов |
| `sim_add_node(s, addr, role, x, y, reliable)` | добавить узел |
| `sim_move_node(s, addr, x, y)` | переместить узел (мобильность) |
| `sim_set_active(s, addr, bool)` | «выключить»/«включить» радио узла |
| `sim_find(s, addr)` | найти узел |
| `sim_node_at(s, i)` / `sim_node_count(s)` | перебор узлов |
| `sim_now(s)` | текущее виртуальное время |
| `sim_run(s, ms)` | продвинуть время |
| `sim_set_path_loss(s, ref, exp)` | модель потерь |
| `sim_set_range(s, m)` | жёсткая дальность |
| `sim_set_bit_error_rate(s, ber)` | вероятность битовой ошибки в принятом кадре |
| `sim_set_callbacks(s, recv, event)` | наблюдательные колбэки |

## 4. Наблюдение

У каждого узла есть счётчики:
`datagrams_rx`, `events`, `last_src`, `last_len`, `last_rssi`, `last_snr`.

Опциональные колбэки `sim_set_callbacks` вызываются при доставке датаграммы и
событии — удобно для логов (см. `examples/desktop_mesh/main.c`).

## 5. Подбор параметров под нужную топологию

Модель потерь: `loss = ref_loss + 10·exp·log10(d)`, `rssi = tx_power − loss`,
`snr = rssi − noise_floor`. Приём невозможен, если `rssi < sensitivity`,
`snr < −20 дБ` или `d > range`.

Для **чётких детерминированных** топологий в тестах используется мягкая модель
потерь (`ref=40, exp=3.0`) плюс жёсткая дальность (`range=250 м`): в пределах
дальности линк почти идеален, за ней — мёртв.

Примеры:
- цепочка: узлы через 200 м, `range=250` → каждый слышит только соседей;
- «сетка»: Master(0,0), R1(200,0), R2(0,200), N(200,200), `range=250` → N не
  слышит Master (283 м) и выбирает ретранслятор.

## 6. Готовый пример

`examples/desktop_mesh/main.c` строит сеть Master + 5 узлов на ~1 км,
печатает топологию, шлёт unicast вниз и вверх, broadcast и статистику:

```
cmake --build build
./build/treenet_example
```
