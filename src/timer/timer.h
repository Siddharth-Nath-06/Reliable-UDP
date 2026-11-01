#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/types.h"

/*
 * Timer Manager
 *
 * Timers use a monotonic clock and store durations in milliseconds.
 * The implementation is thread-safe.
 *
 * Timer IDs range from 1 through the implementation's fixed capacity.
 * timer_create() returns 0 when no timer is available.
 */

TimerId timer_create(void);

/*
 * Start a created, inactive timer.
 * Returns false if the ID is invalid, the timer is inactive,
 * the timer is already running, or the clock cannot be read.
 */
bool timer_start(TimerId id, uint64_t duration_ms);

/*
 * Restart any active timer, whether currently running or stopped.
 * Returns false if the ID is invalid, the timer is inactive,
 * or the clock cannot be read.
 */
bool timer_restart(TimerId id, uint64_t duration_ms);

/*
 * Stop a running timer without destroying it.
 * Returns false if the ID is invalid or the timer is not running.
 */
bool timer_cancel(TimerId id);

/*
 * Destroy an active timer and release its slot.
 * Returns false if the ID is invalid or the timer is inactive.
 */
bool timer_destroy(TimerId id);

/*
 * Return true if a running timer's duration has elapsed.
 *
 * Expiration does not automatically stop the timer. Repeated calls
 * continue returning true until the timer is restarted or cancelled.
 *
 * Returns false for an invalid ID, inactive/stopped timer,
 * or clock-read failure.
 */
bool timer_expired(TimerId id);

#endif