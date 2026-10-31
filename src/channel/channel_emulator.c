#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "channel_emulator.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <errno.h>
#endif

#define MICROSECONDS_PER_SECOND UINT64_C(1000000)
#define MICROSECONDS_PER_MILLISECOND UINT64_C(1000)

/* Avoid passing extremely distant absolute deadlines to time_t. */
#define MAX_CONDITION_WAIT_US (UINT64_C(60) * MICROSECONDS_PER_SECOND)

typedef struct ChannelDatagram {
    ChannelDirection direction;
    uint8_t *data;
    size_t length;
    uint64_t delivery_time_us;

    /* Random outcomes are fixed at submit time, not in the worker. */
    bool should_drop;
    bool should_corrupt;
    bool should_duplicate;
    size_t corruption_byte_index;
    uint8_t corruption_bit_mask;

    struct ChannelDatagram *next;
} ChannelDatagram;

struct ChannelEmulator {
    ChannelEmulatorConfig config;
    ChannelEmulatorOutputCallback output_callback;
    void *callback_context;

    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool worker_started;
    bool stopping;

    ChannelDatagram *head;
    size_t queue_bytes;
    uint64_t next_bandwidth_time_us;
    uint32_t random_state;
};

static bool channel_now_us(uint64_t *result)
{
    if (result == NULL) {
        return false;
    }

#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t whole_seconds;
    uint64_t remainder;
    uint64_t whole_us;
    uint64_t fraction_us;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0 || counter.QuadPart < 0) {
        return false;
    }

    whole_seconds = (uint64_t)counter.QuadPart /
                    (uint64_t)frequency.QuadPart;
    remainder = (uint64_t)counter.QuadPart %
                (uint64_t)frequency.QuadPart;

    if (whole_seconds > UINT64_MAX / MICROSECONDS_PER_SECOND) {
        return false;
    }

    whole_us = whole_seconds * MICROSECONDS_PER_SECOND;
    /* QPC frequencies are well within range for this multiplication. */
    fraction_us = (remainder * MICROSECONDS_PER_SECOND) /
                  (uint64_t)frequency.QuadPart;

    if (UINT64_MAX - whole_us < fraction_us) {
        return false;
    }

    *result = whole_us + fraction_us;
    return true;
#else
    struct timespec now;
    uint64_t seconds;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return false;
    }

    seconds = (uint64_t)now.tv_sec;
    if (seconds > UINT64_MAX / MICROSECONDS_PER_SECOND) {
        return false;
    }

    {
        uint64_t whole_us = seconds * MICROSECONDS_PER_SECOND;
        uint64_t fraction_us = (uint64_t)now.tv_nsec / 1000U;

        if (UINT64_MAX - whole_us < fraction_us) {
            return false;
        }

        *result = whole_us + fraction_us;
    }
    return true;
#endif
}

/* Build an absolute CLOCK_REALTIME deadline for pthread_cond_timedwait. */
static bool channel_realtime_deadline_after_us(
    uint64_t wait_us,
    struct timespec *deadline)
{
    if (deadline == NULL) {
        return false;
    }

    if (wait_us > MAX_CONDITION_WAIT_US) {
        wait_us = MAX_CONDITION_WAIT_US;
    }

#ifdef _WIN32
    {
        struct timeval now;

        if (gettimeofday(&now, NULL) != 0) {
            return false;
        }

        deadline->tv_sec = now.tv_sec +
            (time_t)(wait_us / MICROSECONDS_PER_SECOND);
        deadline->tv_nsec = (long)now.tv_usec * 1000L +
            (long)((wait_us % MICROSECONDS_PER_SECOND) * 1000U);
    }
#else
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return false;
    }

    deadline->tv_sec += (time_t)(wait_us / MICROSECONDS_PER_SECOND);
    deadline->tv_nsec +=
        (long)((wait_us % MICROSECONDS_PER_SECOND) * 1000U);
#endif

    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }

    return true;
}

static void channel_free_datagram(ChannelDatagram *datagram)
{
    if (datagram != NULL) {
        free(datagram->data);
        free(datagram);
    }
}

/* Called only after the worker has stopped. */
static void channel_free_queue(ChannelEmulator *emulator)
{
    ChannelDatagram *current = emulator->head;

    while (current != NULL) {
        ChannelDatagram *next = current->next;
        channel_free_datagram(current);
        current = next;
    }

    emulator->head = NULL;
    emulator->queue_bytes = 0;
}

/* xorshift32; zero is replaced with a nonzero state at creation. */
static uint32_t channel_random_next(ChannelEmulator *emulator)
{
    uint32_t value = emulator->random_state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    emulator->random_state = value;
    return value;
}

static uint64_t channel_random_u64(ChannelEmulator *emulator)
{
    return ((uint64_t)channel_random_next(emulator) << 32) |
           (uint64_t)channel_random_next(emulator);
}

static double channel_random_probability(ChannelEmulator *emulator)
{
    return (double)channel_random_next(emulator) / 4294967296.0;
}

static bool channel_random_event(
    ChannelEmulator *emulator,
    double probability)
{
    if (probability <= 0.0) {
        return false;
    }
    if (probability >= 1.0) {
        return true;
    }
    return channel_random_probability(emulator) < probability;
}

static bool channel_add_u64(uint64_t a, uint64_t b, uint64_t *sum)
{
    if (UINT64_MAX - a < b) {
        return false;
    }
    *sum = a + b;
    return true;
}

static void channel_queue_insert(
    ChannelEmulator *emulator,
    ChannelDatagram *datagram)
{
    ChannelDatagram **current = &emulator->head;

    /* Stable ordering for datagrams with equal delivery times. */
    while (*current != NULL &&
           (*current)->delivery_time_us <= datagram->delivery_time_us) {
        current = &(*current)->next;
    }

    datagram->next = *current;
    *current = datagram;
    emulator->queue_bytes += datagram->length;
}

static ChannelDatagram *channel_queue_pop(ChannelEmulator *emulator)
{
    ChannelDatagram *datagram = emulator->head;

    if (datagram == NULL) {
        return NULL;
    }

    emulator->head = datagram->next;
    datagram->next = NULL;
    emulator->queue_bytes -= datagram->length;
    return datagram;
}

/*
 * ceil(length * 8 bits * 1,000,000 us/s / bandwidth_bps),
 * computed with quotient/remainder to avoid an overflowing product.
 */
static bool channel_transmission_time_us(
    const ChannelEmulator *emulator,
    size_t length,
    uint64_t *duration_us)
{
    uint64_t bits;
    uint64_t whole_seconds;
    uint64_t remainder_bits;
    uint64_t whole_us;
    uint64_t fractional_numerator;
    uint64_t fractional_us;

    if (duration_us == NULL || length > UINT64_MAX / 8U) {
        return false;
    }

    bits = (uint64_t)length * 8U;
    whole_seconds = bits / emulator->config.bandwidth_bps;
    remainder_bits = bits % emulator->config.bandwidth_bps;

    if (whole_seconds > UINT64_MAX / MICROSECONDS_PER_SECOND) {
        return false;
    }

    whole_us = whole_seconds * MICROSECONDS_PER_SECOND;
    fractional_numerator = remainder_bits * MICROSECONDS_PER_SECOND;
    fractional_us = fractional_numerator /
                    emulator->config.bandwidth_bps;

    if (fractional_numerator % emulator->config.bandwidth_bps != 0) {
        fractional_us++;
    }

    if (UINT64_MAX - whole_us < fractional_us) {
        return false;
    }

    *duration_us = whole_us + fractional_us;
    if (*duration_us == 0) {
        *duration_us = 1;
    }
    return true;
}

/*
 * Schedules transmission on the one shared, full-duplex-aggregated
 * bandwidth timeline. jitter_ms means a symmetric +/- deviation
 * around delay_ms, with the resulting propagation delay clamped to 0.
 * Caller must hold emulator->mutex.
 */
static bool channel_schedule_delivery(
    ChannelEmulator *emulator,
    size_t length,
    uint64_t *delivery_time_us)
{
    uint64_t now_us;
    uint64_t start_us;
    uint64_t duration_us;
    uint64_t finish_us;
    uint64_t base_delay_us;
    uint64_t jitter_limit_us;
    uint64_t delivery_delay_us;
    uint64_t candidate_us;
    int64_t jitter_offset_us = 0;

    if (delivery_time_us == NULL || !channel_now_us(&now_us)) {
        return false;
    }

    start_us = now_us;
    if (emulator->next_bandwidth_time_us > start_us) {
        start_us = emulator->next_bandwidth_time_us;
    }

    if (!channel_transmission_time_us(emulator, length, &duration_us) ||
        !channel_add_u64(start_us, duration_us, &finish_us)) {
        return false;
    }

    base_delay_us =
        (uint64_t)emulator->config.delay_ms * MICROSECONDS_PER_MILLISECOND;
    jitter_limit_us =
        (uint64_t)emulator->config.jitter_ms * MICROSECONDS_PER_MILLISECOND;

    if (jitter_limit_us != 0) {
        uint64_t range = jitter_limit_us * 2U + 1U;
        jitter_offset_us = (int64_t)(channel_random_u64(emulator) % range) -
                           (int64_t)jitter_limit_us;
    }

    if (jitter_offset_us < 0) {
        uint64_t subtract_us = (uint64_t)(-jitter_offset_us);
        delivery_delay_us = subtract_us > base_delay_us
            ? 0U
            : base_delay_us - subtract_us;
    } else {
        if (!channel_add_u64(base_delay_us,
                             (uint64_t)jitter_offset_us,
                             &delivery_delay_us)) {
            return false;
        }
    }

    if (!channel_add_u64(finish_us, delivery_delay_us, &candidate_us)) {
        return false;
    }

    if (channel_random_event(
            emulator,
            emulator->config.reordering_probability)) {
        uint64_t extra_us =
            (uint64_t)emulator->config.reordering_delay_ms *
            MICROSECONDS_PER_MILLISECOND;

        if (!channel_add_u64(candidate_us, extra_us, &candidate_us)) {
            return false;
        }
    }

    emulator->next_bandwidth_time_us = finish_us;
    *delivery_time_us = candidate_us;
    return true;
}

static void channel_prepare_random_outcomes(
    ChannelEmulator *emulator,
    ChannelDatagram *datagram)
{
    /* Called under the mutex in submission order for reproducibility. */
    datagram->should_drop = channel_random_event(
        emulator, emulator->config.loss_probability);

    datagram->should_corrupt = channel_random_event(
        emulator, emulator->config.corruption_probability);

    if (datagram->should_corrupt && datagram->length > 0) {
        datagram->corruption_byte_index =
            (size_t)(channel_random_u64(emulator) % datagram->length);
        datagram->corruption_bit_mask = (uint8_t)(
            1U << (channel_random_next(emulator) % 8U));
    }

    datagram->should_duplicate = channel_random_event(
        emulator, emulator->config.duplication_probability);
}

static void channel_apply_corruption(ChannelDatagram *datagram)
{
    if (datagram != NULL && datagram->should_corrupt &&
        datagram->length > 0 &&
        datagram->corruption_byte_index < datagram->length) {
        datagram->data[datagram->corruption_byte_index] ^=
            datagram->corruption_bit_mask;
    }
}

static void *channel_worker(void *argument)
{
    ChannelEmulator *emulator = argument;

    for (;;) {
        ChannelDatagram *datagram;
        uint64_t now_us;

        pthread_mutex_lock(&emulator->mutex);

        while (emulator->head == NULL && !emulator->stopping) {
            pthread_cond_wait(&emulator->condition, &emulator->mutex);
        }

        if (emulator->stopping) {
            pthread_mutex_unlock(&emulator->mutex);
            break;
        }

        datagram = emulator->head;
        if (!channel_now_us(&now_us)) {
            /* Avoid a hot loop if the clock API transiently fails. */
            struct timespec deadline;
            if (channel_realtime_deadline_after_us(10000U, &deadline)) {
                pthread_cond_timedwait(
                    &emulator->condition, &emulator->mutex, &deadline);
            } else {
                pthread_cond_wait(&emulator->condition, &emulator->mutex);
            }
            pthread_mutex_unlock(&emulator->mutex);
            continue;
        }

        if (datagram->delivery_time_us > now_us) {
            uint64_t wait_us = datagram->delivery_time_us - now_us;
            struct timespec deadline;

            if (channel_realtime_deadline_after_us(wait_us, &deadline)) {
                pthread_cond_timedwait(
                    &emulator->condition,
                    &emulator->mutex,
                    &deadline);
            } else {
                /* Clock failure: retry after another queue event. */
                pthread_cond_wait(&emulator->condition, &emulator->mutex);
            }

            pthread_mutex_unlock(&emulator->mutex);
            continue;
        }

        datagram = channel_queue_pop(emulator);
        if (datagram == NULL) {
            pthread_mutex_unlock(&emulator->mutex);
            continue;
        }

        pthread_mutex_unlock(&emulator->mutex);

        if (datagram->should_drop) {
            channel_free_datagram(datagram);
            continue;
        }

        channel_apply_corruption(datagram);

        emulator->output_callback(
            emulator->callback_context,
            datagram->direction,
            datagram->data,
            datagram->length);

        if (datagram->should_duplicate) {
            emulator->output_callback(
                emulator->callback_context,
                datagram->direction,
                datagram->data,
                datagram->length);
        }

        channel_free_datagram(datagram);
    }

    return NULL;
}

static bool channel_config_is_valid(
    const ChannelEmulatorConfig *config,
    ChannelEmulatorOutputCallback output_callback)
{
    if (config == NULL || output_callback == NULL ||
        config->bandwidth_bps == 0 || config->queue_limit_bytes == 0) {
        return false;
    }

    if (!isfinite(config->loss_probability) ||
        !isfinite(config->duplication_probability) ||
        !isfinite(config->corruption_probability) ||
        !isfinite(config->reordering_probability)) {
        return false;
    }

    return config->loss_probability >= 0.0 &&
           config->loss_probability <= 1.0 &&
           config->duplication_probability >= 0.0 &&
           config->duplication_probability <= 1.0 &&
           config->corruption_probability >= 0.0 &&
           config->corruption_probability <= 1.0 &&
           config->reordering_probability >= 0.0 &&
           config->reordering_probability <= 1.0;
}

ChannelEmulator *channel_emulator_create(
    const ChannelEmulatorConfig *config,
    ChannelEmulatorOutputCallback output_callback,
    void *callback_context)
{
    ChannelEmulator *emulator;

    if (!channel_config_is_valid(config, output_callback)) {
        return NULL;
    }

    emulator = calloc(1, sizeof(*emulator));
    if (emulator == NULL) {
        return NULL;
    }

    emulator->config = *config;
    emulator->output_callback = output_callback;
    emulator->callback_context = callback_context;
    emulator->random_state = config->random_seed;

    if (emulator->random_state == 0) {
        uint64_t now_us = 0;
        (void)channel_now_us(&now_us);
        emulator->random_state = (uint32_t)time(NULL) ^
                                 (uint32_t)(uintptr_t)emulator ^
                                 (uint32_t)now_us;
        if (emulator->random_state == 0) {
            emulator->random_state = 1U;
        }
    }

    if (pthread_mutex_init(&emulator->mutex, NULL) != 0) {
        free(emulator);
        return NULL;
    }

    if (pthread_cond_init(&emulator->condition, NULL) != 0) {
        pthread_mutex_destroy(&emulator->mutex);
        free(emulator);
        return NULL;
    }

    if (pthread_create(&emulator->worker, NULL, channel_worker, emulator) != 0) {
        pthread_cond_destroy(&emulator->condition);
        pthread_mutex_destroy(&emulator->mutex);
        free(emulator);
        return NULL;
    }

    emulator->worker_started = true;
    return emulator;
}

bool channel_emulator_submit(
    ChannelEmulator *emulator,
    ChannelDirection direction,
    const uint8_t *data,
    size_t length)
{
    ChannelDatagram *datagram;
    uint64_t delivery_time_us;

    if (emulator == NULL || data == NULL || length == 0 ||
        (direction != CHANNEL_DIRECTION_OUTBOUND &&
         direction != CHANNEL_DIRECTION_INBOUND)) {
        return false;
    }

    if (length > emulator->config.queue_limit_bytes) {
        return false;
    }

    datagram = calloc(1, sizeof(*datagram));
    if (datagram == NULL) {
        return false;
    }

    datagram->data = malloc(length);
    if (datagram->data == NULL) {
        free(datagram);
        return false;
    }

    memcpy(datagram->data, data, length);
    datagram->length = length;
    datagram->direction = direction;

    pthread_mutex_lock(&emulator->mutex);

    if (emulator->stopping ||
        length > emulator->config.queue_limit_bytes - emulator->queue_bytes) {
        pthread_mutex_unlock(&emulator->mutex);
        channel_free_datagram(datagram);
        return false;
    }

    if (!channel_schedule_delivery(emulator, length, &delivery_time_us)) {
        pthread_mutex_unlock(&emulator->mutex);
        channel_free_datagram(datagram);
        return false;
    }

    datagram->delivery_time_us = delivery_time_us;
    channel_prepare_random_outcomes(emulator, datagram);
    channel_queue_insert(emulator, datagram);

    pthread_cond_signal(&emulator->condition);
    pthread_mutex_unlock(&emulator->mutex);
    return true;
}

void channel_emulator_destroy(ChannelEmulator *emulator)
{
    if (emulator == NULL) {
        return;
    }

    pthread_mutex_lock(&emulator->mutex);
    emulator->stopping = true;
    pthread_cond_broadcast(&emulator->condition);
    pthread_mutex_unlock(&emulator->mutex);

    if (emulator->worker_started) {
        pthread_join(emulator->worker, NULL);
    }

    channel_free_queue(emulator);
    pthread_cond_destroy(&emulator->condition);
    pthread_mutex_destroy(&emulator->mutex);
    free(emulator);
}