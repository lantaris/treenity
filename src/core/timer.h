/**
 * @file timer.h
 * @brief Minimal wrap-safe software timers driven by treenet_poll().
 *
 * treenet has no threads and no RTOS dependency; all periodic activity
 * (beaconing, neighbour ageing, retransmission) is expressed with these timers
 * and evaluated whenever the application calls the poll function.
 */
#ifndef TREENET_TIMER_H
#define TREENET_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One software timer. */
typedef struct {
    uint32_t next_ms;  /**< absolute deadline in the port time base */
    uint32_t interval; /**< reload interval, 0 for a one-shot timer */
    bool     active;   /**< timer armed */
} tn_timer_t;

/** @brief Arm a one-shot timer to fire @p delay_ms from @p now. */
void tn_timer_start(tn_timer_t *tm, uint32_t now, uint32_t delay_ms);

/** @brief Arm (or re-arm) a periodic timer. */
void tn_timer_start_periodic(tn_timer_t *tm, uint32_t now, uint32_t interval_ms);

/** @brief Disarm a timer. */
void tn_timer_stop(tn_timer_t *tm);

/** @return true when the timer is armed and its deadline has passed. */
bool tn_timer_expired(const tn_timer_t *tm, uint32_t now);

/**
 * @brief Consume an expired timer.
 *
 * If the timer is periodic it is automatically re-armed for the next interval;
 * a one-shot timer is disarmed.
 *
 * @return true when the timer fired
 */
bool tn_timer_fire(tn_timer_t *tm, uint32_t now);

/** @return milliseconds until the timer fires, or UINT32_MAX if disarmed. */
uint32_t tn_timer_remaining(const tn_timer_t *tm, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* TREENET_TIMER_H */
