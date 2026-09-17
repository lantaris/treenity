/**
 * @file stress_main.c
 * @brief Entry point for the treenity concurrency stress tests.
 *
 * Build & run (from the treenity directory):
 *   make stress
 */
#include <stdio.h>

int run_ringbuf_stress(void);
int run_treenet_stress(void);

int main(void)
{
    printf("treenity concurrency stress\n");
    int fails = 0;

    printf("[ringbuf: 1 producer thread, 1 consumer thread]\n");
    if (run_ringbuf_stress() != 0) fails++;

    printf("[treenet: treenet_rx thread, treenet_poll thread]\n");
    if (run_treenet_stress() != 0) fails++;

    printf("\n%s\n", fails ? "FAILED" : "ok: no lost updates, no corruption");
    return fails ? 1 : 0;
}
