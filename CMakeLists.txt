# ---------------------------------------------------------------------------
# treenity - portable Master-rooted mesh networking library (C99)
#
# Targets:
#   treenet            static library (the portable core)
#   treenet_sim        static library (desktop network simulator)
#   treenet_tests      unit + scenario test runner
#   treenet_example    desktop example
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.13)
project(treenity C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# --- Core library ----------------------------------------------------------
add_library(treenet STATIC
    src/core/ringbuf.c
    src/core/timer.c
    src/mac/frame.c
    src/mac/dupcache.c
    src/mac/airtime.c
    src/link/neighbor.c
    src/link/beacon.c
    src/routing/routing.c
    src/api/treenet.c
)
target_include_directories(treenet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# --- Simulator (host only) -------------------------------------------------
add_library(treenet_sim STATIC sim/sim.c)
target_include_directories(treenet_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sim)
target_link_libraries(treenet_sim PUBLIC treenet m)

# --- Tests -----------------------------------------------------------------
enable_testing()
add_executable(treenet_tests
    tests/unit/test_main.c
    tests/unit/test_core.c
    tests/unit/test_mesh.c
    tests/unit/test_corrupt.c
)
target_link_libraries(treenet_tests PRIVATE treenet_sim)
add_test(NAME treenity_tests COMMAND treenet_tests)

# --- Example ---------------------------------------------------------------
add_executable(treenet_example examples/desktop_mesh/main.c)
target_link_libraries(treenet_example PRIVATE treenet_sim)

# --- Fuzz / robustness harness ---------------------------------------------
add_executable(treenet_fuzz tests/fuzz/fuzz_frame.c)
target_link_libraries(treenet_fuzz PRIVATE treenet_sim)
add_test(NAME treenity_fuzz COMMAND treenet_fuzz 50000)

# --- Optional freestanding cross-build check -------------------------------
# Example: cmake -S . -B build-arm -DCMAKE_C_COMPILER=arm-none-eabi-gcc
# The core library has no dependency on the C library beyond string.h/stdint.h.
