/**
 * @file timer.c
 * @brief Implementation of wrap-safe software timers.
 */
#include "timer.h"

#include "util.h"

void tn_timer_start(tn_timer_t *tm, uint32_t now, uint32_t delay_ms)
{
    tm->next_ms = now + delay_ms;
    tm->interval = 0;
    tm->active = true;
}

void tn_timer_start_periodic(tn_timer_t *tm, uint32_t now, uint32_t interval_ms)
{
    tm->interval = interval_ms;
    tm->next_ms = now + interval_ms;
    tm->active = true;
}

void tn_timer_stop(tn_timer_t *tm)
{
    tm->active = false;
    tm->interval = 0;
}

bool tn_timer_expired(const tn_timer_t *tm, uint32_t now)
{
    if (!tm->active) return false;
    return !tn_time_after(tm->next_ms, now);
}

bool tn_timer_fire(tn_timer_t *tm, uint32_t now)
{
    if (!tn_timer_expired(tm, now)) {
        return false;
    }
    if (tm->interval > 0) {
        /* Re-arm for the next period. If we fell far behind, resynchronise to
         * now instead of scheduling a burst of missed deadlines. */
        tm->next_ms += tm->interval;
        if (tn_time_after(now, tm->next_ms + tm->interval)) {
            tm->next_ms = now + tm->interval;
        }
    } else {
        tm->active = false;
    }
    return true;
}

uint32_t tn_timer_remaining(const tn_timer_t *tm, uint32_t now)
{
    if (!tm->active) return UINT32_MAX;
    if (tn_time_after(now, tm->next_ms)) return 0;
    return (uint32_t)(tm->next_ms - now);
}
