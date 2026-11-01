#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "timer.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <time.h>
#endif

#define TIMER_CAPACITY 100U

typedef struct {
    bool active;
    bool running;
    uint64_t duration_ms;
    uint64_t start_time_ms;
} Timer;

static Timer timers[TIMER_CAPACITY];

/* All accesses to the shared timer pool are protected by this mutex. */
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * Read a monotonic timestamp in milliseconds.
 * Returns false rather than silently using an invalid timestamp.
 *
 * This helper is called only while timer_mutex is held, so
 * initialization of the Windows performance-counter frequency
 * does not race between callers.
 */
static bool get_time_ms(uint64_t *result)
{
    if (result == NULL) {
        return false;
    }

#ifdef _WIN32
    static bool frequency_initialized = false;
    static uint64_t frequency = 0;

    LARGE_INTEGER current;

    if (!frequency_initialized) {
        LARGE_INTEGER measured_frequency;

        if (!QueryPerformanceFrequency(&measured_frequency) ||
            measured_frequency.QuadPart <= 0) {
            return false;
        }

        frequency = (uint64_t)measured_frequency.QuadPart;
        frequency_initialized = true;
    }

    if (!QueryPerformanceCounter(&current) || current.QuadPart < 0) {
        return false;
    }

    {
        uint64_t ticks = (uint64_t)current.QuadPart;
        uint64_t whole_seconds = ticks / frequency;
        uint64_t remaining_ticks = ticks % frequency;

        if (whole_seconds > UINT64_MAX / 1000U) {
            return false;
        }

        uint64_t whole_ms = whole_seconds * 1000U;
        uint64_t fractional_ms;

        /*
         * Avoid multiplying a potentially large tick remainder by
         * 1000 unless that multiplication is safe.
         */
        if (remaining_ticks <= UINT64_MAX / 1000U) {
            fractional_ms =
                (remaining_ticks * 1000U) / frequency;
        } else {
            long double fraction =
                ((long double)remaining_ticks * 1000.0L) /
                (long double)frequency;

            fractional_ms = (uint64_t)fraction;
        }

        if (UINT64_MAX - whole_ms < fractional_ms) {
            return false;
        }

        *result = whole_ms + fractional_ms;
        return true;
    }

#else
    struct timespec current;

    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0 ||
        current.tv_sec < 0) {
        return false;
    }

    {
        uint64_t seconds = (uint64_t)current.tv_sec;

        if (seconds > UINT64_MAX / 1000U) {
            return false;
        }

        uint64_t whole_ms = seconds * 1000U;
        uint64_t fractional_ms =
            (uint64_t)current.tv_nsec / 1000000U;

        if (UINT64_MAX - whole_ms < fractional_ms) {
            return false;
        }

        *result = whole_ms + fractional_ms;
        return true;
    }
#endif
}


static Timer *find_timer(TimerId id)
{
    if (id == 0 || id > TIMER_CAPACITY) {
        return NULL;
    }

    return &timers[id - 1U];
}


TimerId timer_create(void)
{
    TimerId id = 0;

    pthread_mutex_lock(&timer_mutex);

    for (TimerId candidate = 1;
         candidate <= TIMER_CAPACITY;
         candidate++) {
        Timer *timer = &timers[candidate - 1U];

        if (!timer->active) {
            timer->active = true;
            timer->running = false;
            timer->duration_ms = 0;
            timer->start_time_ms = 0;

            id = candidate;
            break;
        }
    }

    pthread_mutex_unlock(&timer_mutex);
    return id;
}


bool timer_start(TimerId id, uint64_t duration_ms)
{
    bool success = false;
    uint64_t now_ms;
    Timer *timer;

    pthread_mutex_lock(&timer_mutex);

    timer = find_timer(id);

    if (timer != NULL &&
        timer->active &&
        !timer->running &&
        get_time_ms(&now_ms)) {
        timer->duration_ms = duration_ms;
        timer->start_time_ms = now_ms;
        timer->running = true;
        success = true;
    }

    pthread_mutex_unlock(&timer_mutex);
    return success;
}


bool timer_restart(TimerId id, uint64_t duration_ms)
{
    bool success = false;
    uint64_t now_ms;
    Timer *timer;

    pthread_mutex_lock(&timer_mutex);

    timer = find_timer(id);

    if (timer != NULL &&
        timer->active &&
        get_time_ms(&now_ms)) {
        timer->duration_ms = duration_ms;
        timer->start_time_ms = now_ms;
        timer->running = true;
        success = true;
    }

    pthread_mutex_unlock(&timer_mutex);
    return success;
}


bool timer_cancel(TimerId id)
{
    bool success = false;
    Timer *timer;

    pthread_mutex_lock(&timer_mutex);

    timer = find_timer(id);

    if (timer != NULL && timer->active && timer->running) {
        timer->running = false;
        success = true;
    }

    pthread_mutex_unlock(&timer_mutex);
    return success;
}


bool timer_expired(TimerId id)
{
    bool expired = false;
    uint64_t now_ms;
    Timer *timer;

    pthread_mutex_lock(&timer_mutex);

    timer = find_timer(id);

    if (timer != NULL &&
        timer->active &&
        timer->running &&
        get_time_ms(&now_ms) &&
        now_ms >= timer->start_time_ms) {
        expired =
            (now_ms - timer->start_time_ms) >= timer->duration_ms;
    }

    pthread_mutex_unlock(&timer_mutex);
    return expired;
}


bool timer_destroy(TimerId id)
{
    bool success = false;
    Timer *timer;

    pthread_mutex_lock(&timer_mutex);

    timer = find_timer(id);

    if (timer != NULL && timer->active) {
        timer->active = false;
        timer->running = false;
        timer->duration_ms = 0;
        timer->start_time_ms = 0;
        success = true;
    }

    pthread_mutex_unlock(&timer_mutex);
    return success;
}