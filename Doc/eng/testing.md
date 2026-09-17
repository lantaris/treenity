# Testing

[Русский](../ru/testing.md) | **English**

## 1. Building and running

### CMake (recommended)

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Targets:

- `treenet` — the library core;
- `treenet_sim` — the simulator;
- `treenet_tests` — unit and scenario tests;
- `treenet_fuzz` — parser and receive-path fuzzing;
- `treenet_example` — the example.

### Make

```sh
make test      # build and run the tests
make example   # build the example
make run       # build and run the example
make fuzz      # fuzzing (200000 iterations)
```

### ARM cross-build

```sh
cmake -S . -B build-arm -DCMAKE_C_COMPILER=arm-none-eabi-gcc
cmake --build build-arm
```

The core depends only on `stdint.h`/`stddef.h`/`stdbool.h`/`string.h` and needs
no OS.

## 2. What the tests cover

### Unit tests (`tests/unit/test_core.c`)

- wrap-safe time arithmetic;
- clamp and SNR/ETX scoring;
- EWMA convergence;
- ring buffer (push/pop/overflow);
- timers (one-shot, periodic);
- duplicate cache;
- frame, beacon, DAO and fragment codecs (round-trip and rejection of short
  input);
- time-on-air monotonicity.

### Scenario tests (`tests/unit/test_mesh.c`)

On the simulator:

- network formation (all nodes join);
- rank ordering (strictly increasing along a chain);
- unicast down (Master → leaf) and up (leaf → Master);
- broadcast reaches everyone;
- **seamless reconfiguration**: a relay fails → the node switches to another
  parent and stays connected;
- **beacon interval reset on re-parenting** (fast reconvergence);
- neighbour metrics (RSSI/SNR/cost/parent).

### Corruption tests (`tests/unit/test_corrupt.c`)

- the CRC catches **every single bit** in a frame of any type and **any
  truncation**;
- a wrong protocol version and invalid addresses (`src=0/broadcast`, `dst=0`)
  are rejected;
- a corrupted frame **creates no state** (a snapshot of neighbours/routes/
  parent/rank before and after injection is identical);
- a foreign `net_id` is ignored;
- semantically broken frames: a short beacon payload, an impossible beacon
  interval, a DAO with `hops > MAX_HOPS`, malformed fragments (`count=0`,
  `index>=count`, a short non-last fragment, a mismatched `count`).

### Bit-error scenario (`test_bit_errors_tolerated`)

The simulator injects bit errors (`sim_set_bit_error_rate`); the network stays
connected, the CRC drops corrupted frames, and no node picks a parent it cannot
hear.

### Fuzzing (`tests/fuzz/fuzz_frame.c`)

Two modes in a single run:

1. **Mutation** — starting from a corpus of valid frames (one per type plus a
   fragment), random mutations are applied: bit flip, random byte, truncation,
   swap. This reaches parser branches that pure random input almost never
   touches (the CRC rejects it).
2. **Pure random bytes.**

After every input the invariants of the live instance are checked: neighbour and
route table sizes, `rank`/`connected` consistency, absence of a self-parent,
correct Master state. A violation calls `abort()`. It builds both as a normal
executable and with libFuzzer (`-DTREENET_LIBFUZZER`).

## 3. How to read the result

```
treenity tests
[core]
  test_frame_roundtrip ... ok
[mesh]
  test_seamless_reparenting ... ok

1855 checks, 0 failures
```

A non-zero exit code means failure — convenient for CI.

## 4. Adding a test

1. Write a `static void test_x(void)` function and use `CHECK`/`CHECK_EQ`.
2. Call it from `run_core_tests()` or `run_mesh_tests()` via `RUN_TEST`.

## 5. Recommendations

- Always set a seed for scenario tests — scenarios must be reproducible.
- Define the topology through `sim_set_range` and a mild loss model so the
  "heard / not heard" boundaries are crisp.
- Check not only "it works" but also stability: the number of parent changes
  (`parent_changes`) must not grow without bound.
