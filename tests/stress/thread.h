/**
 * @file thread.h
 * @brief Minimal thread wrapper for the treenity stress tests.
 *
 * Uses Win32 threads on Windows and POSIX threads elsewhere. Only what the
 * stress tests need: start and join.
 */
#ifndef TREENET_STRESS_THREAD_H
#define TREENET_STRESS_THREAD_H

#include <stddef.h>

typedef void *(*tn_stress_fn)(void *);

#ifdef _WIN32

#include <windows.h>

typedef struct {
    tn_stress_fn fn;
    void        *arg;
    HANDLE       h;
} tn_stress_thread_t;

static DWORD WINAPI tn_stress_trampoline(LPVOID p)
{
    tn_stress_thread_t *th = (tn_stress_thread_t *)p;
    (void)th->fn(th->arg);
    return 0;
}

static inline int tn_stress_thread_start(tn_stress_thread_t *th,
                                         tn_stress_fn fn, void *arg)
{
    th->fn = fn;
    th->arg = arg;
    th->h = CreateThread(NULL, 0, tn_stress_trampoline, th, 0, NULL);
    return (th->h != NULL) ? 0 : -1;
}

static inline void tn_stress_thread_join(tn_stress_thread_t th)
{
    WaitForSingleObject(th.h, INFINITE);
    CloseHandle(th.h);
}

#else

#include <pthread.h>

typedef struct {
    pthread_t h;
} tn_stress_thread_t;

static inline int tn_stress_thread_start(tn_stress_thread_t *th,
                                         tn_stress_fn fn, void *arg)
{
    return pthread_create(&th->h, NULL, fn, arg);
}

static inline void tn_stress_thread_join(tn_stress_thread_t th)
{
    pthread_join(th.h, NULL);
}

#endif

#endif /* TREENET_STRESS_THREAD_H */
