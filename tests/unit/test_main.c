/**
 * @file test_main.c
 * @brief Entry point for the treenity unit and scenario test suite.
 */
#include "test.h"

int g_checks = 0;
int g_failures = 0;

void run_core_tests(void);
void run_mesh_tests(void);
void run_corrupt_tests(void);

int main(void)
{
    printf("treenity tests\n");

    printf("[core]\n");
    run_core_tests();

    printf("[mesh]\n");
    run_mesh_tests();

    printf("[corruption]\n");
    run_corrupt_tests();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
