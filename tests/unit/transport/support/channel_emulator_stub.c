/*
 * Test double for the Channel Emulator.
 *
 * This stub does not simulate bandwidth, delay, loss, corruption,
 * duplication, or reordering. It validates the Transport integration
 * by asynchronously passing submitted datagrams to the registered
 * callback, preserving their direction and bytes.
 */
#include "channel/channel_emulator.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct StubDatagram {
    ChannelDirection direction;
    uint8_t *data;
    size_t length;
    struct StubDatagram *next;
} StubDatagram;

struct ChannelEmulator {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    bool worker_started;
    bool stopping;

    ChannelEmulatorOutputCallback output_callback;
    void *callback_context;

    StubDatagram *head;
    StubDatagram *tail;
};

static void free_datagram(StubDatagram *datagram)
{
    if (datagram != NULL) {
        free(datagram->data);
        free(datagram);
    }
}

static void free_pending_datagrams(ChannelEmulator *emulator)
{
    StubDatagram *current = emulator->head;

    while (current != NULL) {
        StubDatagram *next = current->next;
        free_datagram(current);
        current = next;
    }

    emulator->head = NULL;
    emulator->tail = NULL;
}

static void *stub_worker(void *argument)
{
    ChannelEmulator *emulator = argument;

    for (;;) {
        StubDatagram *datagram;

        pthread_mutex_lock(&emulator->mutex);

        while (emulator->head == NULL && !emulator->stopping) {
            pthread_cond_wait(&emulator->condition, &emulator->mutex);
        }

        /* Discard pending items on shutdown, matching the contract. */
        if (emulator->stopping) {
            pthread_mutex_unlock(&emulator->mutex);
            break;
        }

        datagram = emulator->head;
        emulator->head = datagram->next;
        if (emulator->head == NULL) {
            emulator->tail = NULL;
        }

        pthread_mutex_unlock(&emulator->mutex);

        /* The callback runs on this worker thread. */
        emulator->output_callback(
            emulator->callback_context,
            datagram->direction,
            datagram->data,
            datagram->length
        );

        free_datagram(datagram);
    }

    return NULL;
}

ChannelEmulator *channel_emulator_create(
    const ChannelEmulatorConfig *config,
    ChannelEmulatorOutputCallback output_callback,
    void *callback_context)
{
    ChannelEmulator *emulator;

    if (config == NULL || output_callback == NULL ||
        config->bandwidth_bps == 0 || config->queue_limit_bytes == 0 ||
        config->loss_probability < 0.0 || config->loss_probability > 1.0 ||
        config->duplication_probability < 0.0 ||
        config->duplication_probability > 1.0 ||
        config->corruption_probability < 0.0 ||
        config->corruption_probability > 1.0 ||
        config->reordering_probability < 0.0 ||
        config->reordering_probability > 1.0) {
        return NULL;
    }

    emulator = calloc(1, sizeof(*emulator));
    if (emulator == NULL) {
        return NULL;
    }

    emulator->output_callback = output_callback;
    emulator->callback_context = callback_context;

    if (pthread_mutex_init(&emulator->mutex, NULL) != 0) {
        free(emulator);
        return NULL;
    }

    if (pthread_cond_init(&emulator->condition, NULL) != 0) {
        pthread_mutex_destroy(&emulator->mutex);
        free(emulator);
        return NULL;
    }

    if (pthread_create(&emulator->worker, NULL, stub_worker, emulator) != 0) {
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
    StubDatagram *datagram;

    if (emulator == NULL || data == NULL || length == 0 ||
        (direction != CHANNEL_DIRECTION_OUTBOUND &&
         direction != CHANNEL_DIRECTION_INBOUND)) {
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

    if (emulator->stopping) {
        pthread_mutex_unlock(&emulator->mutex);
        free_datagram(datagram);
        return false;
    }

    if (emulator->tail == NULL) {
        emulator->head = datagram;
        emulator->tail = datagram;
    } else {
        emulator->tail->next = datagram;
        emulator->tail = datagram;
    }

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

    free_pending_datagrams(emulator);
    pthread_cond_destroy(&emulator->condition);
    pthread_mutex_destroy(&emulator->mutex);
    free(emulator);
}
