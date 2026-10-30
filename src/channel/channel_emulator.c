
#include "channel_emulator.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/time.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <errno.h>
#endif

/*
 * Internal representation of a queued datagram.
 */
typedef struct ChannelDatagram {
    ChannelDirection direction;
    uint8_t *data;
    size_t length;

    uint64_t delivery_time_ms;

    struct ChannelDatagram *next;
} ChannelDatagram;

/*
 * Private Channel Emulator implementation.
 */
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

    uint64_t next_bandwidth_time_ms;

    uint32_t random_state;
};

/*
 * Return the current time in milliseconds.
 */
static uint64_t channel_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount();
#else
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return (uint64_t)now.tv_sec * 1000U +
           (uint64_t)now.tv_nsec / 1000000U;
#endif
}

/*
 * Release a single datagram and its owned data.
 */
static void channel_free_datagram(ChannelDatagram *datagram)
{
    if (datagram != NULL) {
        free(datagram->data);
        free(datagram);
    }
}

/*
 * Release every pending datagram.
 *
 * Called only when the worker has stopped and the queue
 * is no longer being modified.
 */
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


/*
 * Generate the next deterministic pseudo-random value.
 */
static uint32_t channel_random_next(ChannelEmulator *emulator)
{
    uint32_t value = emulator->random_state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;

    emulator->random_state = value;
    return value;
}

/*
 * Generate a random floating-point value in [0.0, 1.0).
 */
static double channel_random_probability(ChannelEmulator *emulator)
{
    return (double)channel_random_next(emulator) /
           4294967296.0;
}

/*
 * Return true with the configured probability.
 */
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

/*
 * Generate an integer in the inclusive range [minimum, maximum].
 */
static uint32_t channel_random_range(
    ChannelEmulator *emulator,
    uint32_t minimum,
    uint32_t maximum)
{
    uint64_t range;
    uint64_t random_value;

    if (maximum <= minimum) {
        return minimum;
    }

    range = (uint64_t)maximum - minimum + 1U;
    random_value = channel_random_next(emulator);

    return minimum + (uint32_t)(random_value % range);
}

/*
 * Insert a datagram into the queue in delivery-time order.
 *
 * The caller must hold emulator->mutex.
 */
static void channel_queue_insert(
    ChannelEmulator *emulator,
    ChannelDatagram *datagram)
{
    ChannelDatagram **current = &emulator->head;

    while (*current != NULL &&
           (*current)->delivery_time_ms <= datagram->delivery_time_ms) {
        current = &(*current)->next;
    }

    datagram->next = *current;
    *current = datagram;

    emulator->queue_bytes += datagram->length;
}

/*
 * Remove and return the earliest scheduled datagram.
 *
 * The caller must hold emulator->mutex.
 */
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
 * Calculate the transmission time required for a datagram.
 *
 * bandwidth_bps is measured in bits per second.
 */
static uint64_t channel_transmission_time_ms(
    const ChannelEmulator *emulator,
    size_t length)
{
    uint64_t bits;
    uint64_t duration;

    bits = (uint64_t)length * 8U;

    duration = (bits * 1000U +
                emulator->config.bandwidth_bps - 1U) /
               emulator->config.bandwidth_bps;

    /* Even a very small datagram consumes some transmission time. */
    return duration == 0 ? 1U : duration;
}

/*
 * Calculate the delivery time for a newly submitted datagram.
 *
 * The caller must hold emulator->mutex because the bandwidth
 * schedule and random generator are shared.
 */
static uint64_t channel_schedule_delivery(
    ChannelEmulator *emulator,
    size_t length)
{
    uint64_t now = channel_now_ms();
    uint64_t start_time;
    uint64_t finish_time;
    uint64_t delay;
    uint64_t jitter;

    start_time = now;

    if (emulator->next_bandwidth_time_ms > start_time) {
        start_time = emulator->next_bandwidth_time_ms;
    }

    finish_time = start_time +
        channel_transmission_time_ms(emulator, length);

    emulator->next_bandwidth_time_ms = finish_time;

    delay = emulator->config.delay_ms;

    jitter = channel_random_range(
        emulator,
        0U,
        emulator->config.jitter_ms
    );

    return finish_time + delay + jitter;
}




/*
 * Apply corruption by flipping one randomly selected bit.
 *
 * The caller must hold emulator->mutex.
 */
static void channel_apply_corruption(
    ChannelEmulator *emulator,
    ChannelDatagram *datagram)
{
    uint64_t random_value;
    size_t byte_index;
    uint8_t bit_mask;

    if (datagram == NULL || datagram->length == 0) {
        return;
    }

    random_value =
        ((uint64_t)channel_random_next(emulator) << 32) |
        (uint64_t)channel_random_next(emulator);

    byte_index = (size_t)(
        random_value % (uint64_t)datagram->length
    );

    bit_mask = (uint8_t)(
        1U << (channel_random_next(emulator) % 8U)
    );

    datagram->data[byte_index] ^= bit_mask;
}


/*
 * Decide whether a datagram should be dropped.
 *
 * The caller must hold emulator->mutex.
 */
static bool channel_should_drop(ChannelEmulator *emulator)
{
    return channel_random_event(
        emulator,
        emulator->config.loss_probability
    );
}

/*
 * Decide whether a datagram should be duplicated.
 *
 * The caller must hold emulator->mutex.
 */
static bool channel_should_duplicate(ChannelEmulator *emulator)
{
    return channel_random_event(
        emulator,
        emulator->config.duplication_probability
    );
}


/*
 * Apply the configured reordering delay to selected datagrams.
 *
 * The caller must hold emulator->mutex.
 */
static void channel_apply_reordering(
    ChannelEmulator *emulator,
    ChannelDatagram *datagram)
{
    if (datagram == NULL) {
        return;
    }

    if (channel_random_event(
            emulator,
            emulator->config.reordering_probability)) {

        datagram->delivery_time_ms +=
            emulator->config.reordering_delay_ms;
    }
}


/*
 * Worker thread responsible for asynchronous datagram delivery.
 */
static void *channel_worker(void *argument)
{
    ChannelEmulator *emulator = argument;

    for (;;) {
        ChannelDatagram *datagram;
        uint64_t now;
        bool duplicate;

        pthread_mutex_lock(&emulator->mutex);

        while (emulator->head == NULL && !emulator->stopping) {
            pthread_cond_wait(
                &emulator->condition,
                &emulator->mutex
            );
        }

        if (emulator->stopping) {
            pthread_mutex_unlock(&emulator->mutex);
            break;
        }

        datagram = emulator->head;
        now = channel_now_ms();

        if (datagram->delivery_time_ms > now) {
            uint64_t wait_ms = datagram->delivery_time_ms - now;
            struct timespec deadline;

#ifdef _WIN32
            /*
             * Windows pthread condition variables use realtime
             * absolute deadlines with this pthreads-w32 runtime.
             */
            {
    struct timeval realtime;

    gettimeofday(&realtime, NULL);

    deadline.tv_sec =
        realtime.tv_sec + (time_t)(wait_ms / 1000U);

    deadline.tv_nsec =
        (long)realtime.tv_usec * 1000L +
        (long)((wait_ms % 1000U) * 1000000U);

    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
}
#else
            clock_gettime(CLOCK_REALTIME, &deadline);

            deadline.tv_sec += (time_t)(wait_ms / 1000U);
            deadline.tv_nsec +=
                (long)((wait_ms % 1000U) * 1000000U);

            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
#endif

            pthread_cond_timedwait(
                &emulator->condition,
                &emulator->mutex,
                &deadline
            );

            pthread_mutex_unlock(&emulator->mutex);
            continue;
        }

        datagram = channel_queue_pop(emulator);

        if (datagram == NULL) {
            pthread_mutex_unlock(&emulator->mutex);
            continue;
        }

        if (channel_should_drop(emulator)) {
            pthread_mutex_unlock(&emulator->mutex);
            channel_free_datagram(datagram);
            continue;
        }

        if (channel_random_event(
                emulator,
                emulator->config.corruption_probability)) {
            channel_apply_corruption(emulator, datagram);
        }
        duplicate = channel_should_duplicate(emulator);

        pthread_mutex_unlock(&emulator->mutex);

        emulator->output_callback(
    emulator->callback_context,
    datagram->direction,
    datagram->data,
    datagram->length
);

if (duplicate) {
    emulator->output_callback(
        emulator->callback_context,
        datagram->direction,
        datagram->data,
        datagram->length
    );
}

channel_free_datagram(datagram);
    }

    return NULL;
}


/*
 * Validate the public channel configuration.
 */
static bool channel_config_is_valid(
    const ChannelEmulatorConfig *config,
    ChannelEmulatorOutputCallback output_callback)
{
    if (config == NULL || output_callback == NULL) {
        return false;
    }

    if (config->bandwidth_bps == 0 ||
        config->queue_limit_bytes == 0) {
        return false;
    }

    if (!isfinite(config->loss_probability) ||
        !isfinite(config->duplication_probability) ||
        !isfinite(config->corruption_probability) ||
        !isfinite(config->reordering_probability)) {
        return false;
    }

    if (config->loss_probability < 0.0 ||
        config->loss_probability > 1.0 ||
        config->duplication_probability < 0.0 ||
        config->duplication_probability > 1.0 ||
        config->corruption_probability < 0.0 ||
        config->corruption_probability > 1.0 ||
        config->reordering_probability < 0.0 ||
        config->reordering_probability > 1.0) {
        return false;
    }

    return true;
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

    /*
     * Xorshift32 requires a nonzero initial state.
     */
    emulator->random_state = config->random_seed;

    if (emulator->random_state == 0) {
        emulator->random_state =
            (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)emulator;

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

    if (pthread_create(
            &emulator->worker,
            NULL,
            channel_worker,
            emulator) != 0) {
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
        length > emulator->config.queue_limit_bytes -
                 emulator->queue_bytes) {
        pthread_mutex_unlock(&emulator->mutex);
        channel_free_datagram(datagram);
        return false;
    }

    datagram->delivery_time_ms =
        channel_schedule_delivery(emulator, length);

    channel_apply_reordering(emulator, datagram);

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

    /*
     * The worker has stopped, so the pending queue
     * can now be safely released.
     */
    channel_free_queue(emulator);

    pthread_cond_destroy(&emulator->condition);
    pthread_mutex_destroy(&emulator->mutex);

    free(emulator);
}