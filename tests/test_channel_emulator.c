
#include "../src/channel/channel_emulator.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;

    int callback_count;
    ChannelDirection last_direction;
    uint8_t last_data[128];
    size_t last_length;
} TestContext;

static void test_output_callback(
    void *context,
    ChannelDirection direction,
    const uint8_t *data,
    size_t length)
{
    TestContext *test = context;

    pthread_mutex_lock(&test->mutex);

    if (length <= sizeof(test->last_data)) {
        memcpy(test->last_data, data, length);
        test->last_length = length;
        test->last_direction = direction;
        test->callback_count++;
    }

    pthread_cond_signal(&test->condition);
    pthread_mutex_unlock(&test->mutex);
}

static bool wait_for_callback(TestContext *test, int expected_count)
{
    struct timeval now;
    struct timespec deadline;
    int result = 0;

    gettimeofday(&now, NULL);

    deadline.tv_sec = now.tv_sec + 5;
    deadline.tv_nsec = (long)now.tv_usec * 1000L;

    pthread_mutex_lock(&test->mutex);

    while (test->callback_count < expected_count && result == 0) {
        result = pthread_cond_timedwait(
            &test->condition,
            &test->mutex,
            &deadline
        );
    }

    pthread_mutex_unlock(&test->mutex);

    return result == 0;
}

static bool check_callback(
    TestContext *test,
    int expected_count,
    ChannelDirection expected_direction,
    const uint8_t *expected_data,
    size_t expected_length)
{
    bool passed;

    pthread_mutex_lock(&test->mutex);

    passed =
        test->callback_count == expected_count &&
        test->last_direction == expected_direction &&
        test->last_length == expected_length &&
        memcmp(test->last_data, expected_data, expected_length) == 0;

    pthread_mutex_unlock(&test->mutex);

    return passed;
}

int main(void)
{
    ChannelEmulatorConfig config = {
        1000000U,
        50U,
        0U,
        0.0,
        0.0,
        0.0,
        0.0,
        0U,
        1024U * 1024U,
        12345U
    };

    TestContext test = {0};
    ChannelEmulator *emulator;

    const uint8_t outbound_data[] = "Outbound test";
    const uint8_t inbound_data[] = "Inbound test";

    uint8_t outbound_copy[sizeof(outbound_data)];

    if (pthread_mutex_init(&test.mutex, NULL) != 0) {
        printf("FAIL: Could not initialize test mutex\n");
        return 1;
    }

    if (pthread_cond_init(&test.condition, NULL) != 0) {
        pthread_mutex_destroy(&test.mutex);
        printf("FAIL: Could not initialize test condition\n");
        return 1;
    }

    emulator = channel_emulator_create(
        &config,
        test_output_callback,
        &test
    );

    if (emulator == NULL) {
        printf("FAIL: Could not create channel emulator\n");
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    /* Test outbound delivery and input-data copying. */
    memcpy(outbound_copy, outbound_data, sizeof(outbound_data));

    if (!channel_emulator_submit(
            emulator,
            CHANNEL_DIRECTION_OUTBOUND,
            outbound_copy,
            sizeof(outbound_copy))) {
        printf("FAIL: Outbound submission rejected\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    /* Modify the caller's buffer after submission. */
    memset(outbound_copy, 'X', sizeof(outbound_copy));

    if (!wait_for_callback(&test, 1) ||
        !check_callback(
            &test,
            1,
            CHANNEL_DIRECTION_OUTBOUND,
            outbound_data,
            sizeof(outbound_data))) {
        printf("FAIL: Outbound delivery or data copying\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    printf("PASS: Outbound delivery and data copying\n");

    /* Test inbound delivery. */
    if (!channel_emulator_submit(
            emulator,
            CHANNEL_DIRECTION_INBOUND,
            inbound_data,
            sizeof(inbound_data))) {
        printf("FAIL: Inbound submission rejected\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    if (!wait_for_callback(&test, 2) ||
        !check_callback(
            &test,
            2,
            CHANNEL_DIRECTION_INBOUND,
            inbound_data,
            sizeof(inbound_data))) {
        printf("FAIL: Inbound delivery\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    printf("PASS: Inbound delivery\n");


/* Test packet duplication. */
channel_emulator_destroy(emulator);

config.duplication_probability = 1.0;

test.callback_count = 0;
test.last_length = 0;

emulator = channel_emulator_create(
    &config,
    test_output_callback,
    &test
);

if (emulator == NULL) {
    printf("FAIL: Could not create emulator for duplication test\n");
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

if (!channel_emulator_submit(
        emulator,
        CHANNEL_DIRECTION_OUTBOUND,
        outbound_data,
        sizeof(outbound_data))) {
    printf("FAIL: Duplication test submission rejected\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

if (!wait_for_callback(&test, 2)) {
    printf("FAIL: Expected two callbacks for duplicated datagram\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

pthread_mutex_lock(&test.mutex);

if (test.callback_count != 2 ||
    test.last_direction != CHANNEL_DIRECTION_OUTBOUND ||
    test.last_length != sizeof(outbound_data) ||
    memcmp(test.last_data, outbound_data, sizeof(outbound_data)) != 0) {
    pthread_mutex_unlock(&test.mutex);
    printf("FAIL: Packet duplication behavior\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

pthread_mutex_unlock(&test.mutex);

printf("PASS: Packet duplication\n");

channel_emulator_destroy(emulator);

/* Test packet loss. */
config.duplication_probability = 0.0;
config.loss_probability = 1.0;

test.callback_count = 0;
test.last_length = 0;

emulator = channel_emulator_create(
    &config,
    test_output_callback,
    &test
);

if (emulator == NULL) {
    printf("FAIL: Could not create emulator for loss test\n");
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

if (!channel_emulator_submit(
        emulator,
        CHANNEL_DIRECTION_OUTBOUND,
        outbound_data,
        sizeof(outbound_data))) {
    printf("FAIL: Loss test submission rejected\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

/* Allow time for the worker to process and drop the datagram. */
#ifdef _WIN32
Sleep(300);
#else
{
    struct timespec pause_time;

    pause_time.tv_sec = 0;
    pause_time.tv_nsec = 300000000L;

    nanosleep(&pause_time, NULL);
}
#endif

pthread_mutex_lock(&test.mutex);

if (test.callback_count != 0) {
    pthread_mutex_unlock(&test.mutex);
    printf("FAIL: Packet was delivered despite 100%% loss\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

pthread_mutex_unlock(&test.mutex);

printf("PASS: Packet loss\n");

channel_emulator_destroy(emulator);
/* Test packet corruption. */
config.loss_probability = 0.0;
config.corruption_probability = 1.0;

test.callback_count = 0;
test.last_length = 0;

emulator = channel_emulator_create(
    &config,
    test_output_callback,
    &test
);

if (emulator == NULL) {
    printf("FAIL: Could not create emulator for corruption test\n");
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

if (!channel_emulator_submit(
        emulator,
        CHANNEL_DIRECTION_OUTBOUND,
        outbound_data,
        sizeof(outbound_data))) {
    printf("FAIL: Corruption test submission rejected\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

if (!wait_for_callback(&test, 1)) {
    printf("FAIL: Corrupted datagram was not delivered\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

pthread_mutex_lock(&test.mutex);

if (test.callback_count != 1 ||
    test.last_direction != CHANNEL_DIRECTION_OUTBOUND ||
    test.last_length != sizeof(outbound_data) ||
    memcmp(test.last_data, outbound_data, sizeof(outbound_data)) == 0) {
    pthread_mutex_unlock(&test.mutex);
    printf("FAIL: Packet corruption behavior\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

pthread_mutex_unlock(&test.mutex);

printf("PASS: Packet corruption\n");

channel_emulator_destroy(emulator);


/* Test configured channel delay. */
config.corruption_probability = 0.0;
config.delay_ms = 500U;
config.jitter_ms = 0U;

test.callback_count = 0;
test.last_length = 0;

emulator = channel_emulator_create(
    &config,
    test_output_callback,
    &test
);

if (emulator == NULL) {
    printf("FAIL: Could not create emulator for delay test\n");
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

if (!channel_emulator_submit(
        emulator,
        CHANNEL_DIRECTION_OUTBOUND,
        outbound_data,
        sizeof(outbound_data))) {
    printf("FAIL: Delay test submission rejected\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

/* Check that delivery has not happened after 200 ms. */
#ifdef _WIN32
Sleep(200);
#else
{
    struct timespec pause_time = {0, 200000000L};
    nanosleep(&pause_time, NULL);
}
#endif

pthread_mutex_lock(&test.mutex);

if (test.callback_count != 0) {
    pthread_mutex_unlock(&test.mutex);
    printf("FAIL: Datagram delivered before configured delay\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

pthread_mutex_unlock(&test.mutex);

/* Now wait for the expected delivery. */
if (!wait_for_callback(&test, 1)) {
    printf("FAIL: Datagram not delivered after configured delay\n");
    channel_emulator_destroy(emulator);
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

printf("PASS: Configured channel delay\n");

channel_emulator_destroy(emulator);

/* Test packet reordering. */
config.delay_ms = 0U;
config.reordering_probability = 0.01;
config.reordering_delay_ms = 500U;
config.random_seed = 1U;

test.callback_count = 0;
test.last_length = 0;

emulator = channel_emulator_create(
    &config,
    test_output_callback,
    &test
);

if (emulator == NULL) {
    printf("FAIL: Could not create emulator for reordering test\n");
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

{
    const uint8_t first_packet[] = "First packet";
    const uint8_t second_packet[] = "Second packet";

    if (!channel_emulator_submit(
            emulator,
            CHANNEL_DIRECTION_OUTBOUND,
            first_packet,
            sizeof(first_packet)) ||
        !channel_emulator_submit(
            emulator,
            CHANNEL_DIRECTION_OUTBOUND,
            second_packet,
            sizeof(second_packet))) {
        printf("FAIL: Reordering test submission rejected\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    if (!wait_for_callback(&test, 1)) {
        printf("FAIL: No packet delivered during reordering test\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    pthread_mutex_lock(&test.mutex);

    if (test.last_length != sizeof(second_packet) ||
        memcmp(test.last_data, second_packet, sizeof(second_packet)) != 0) {
        pthread_mutex_unlock(&test.mutex);
        printf("FAIL: Packets were not reordered\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    pthread_mutex_unlock(&test.mutex);
}

printf("PASS: Packet reordering\n");

channel_emulator_destroy(emulator);

/* Test queue capacity and rejection. */
config.reordering_probability = 0.0;
config.reordering_delay_ms = 0U;
config.delay_ms = 10000U;
config.queue_limit_bytes = 16U;

test.callback_count = 0;
test.last_length = 0;

emulator = channel_emulator_create(
    &config,
    test_output_callback,
    &test
);

if (emulator == NULL) {
    printf("FAIL: Could not create emulator for queue test\n");
    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);
    return 1;
}

{
    const uint8_t packet[12] = "First packet";
    const uint8_t second_packet[8] = "Second";

    if (!channel_emulator_submit(
            emulator,
            CHANNEL_DIRECTION_OUTBOUND,
            packet,
            sizeof(packet))) {
        printf("FAIL: First queue submission rejected\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }

    if (channel_emulator_submit(
            emulator,
            CHANNEL_DIRECTION_OUTBOUND,
            second_packet,
            sizeof(second_packet))) {
        printf("FAIL: Queue accepted data beyond its limit\n");
        channel_emulator_destroy(emulator);
        pthread_cond_destroy(&test.condition);
        pthread_mutex_destroy(&test.mutex);
        return 1;
    }
}

printf("PASS: Queue capacity and rejection\n");

channel_emulator_destroy(emulator);

/* Test invalid configuration rejection. */
{
    ChannelEmulatorConfig invalid_config = config;

    invalid_config.bandwidth_bps = 0;

    if (channel_emulator_create(
            &invalid_config,
            test_output_callback,
            &test) != NULL) {
        printf("FAIL: Zero bandwidth was accepted\n");
        return 1;
    }

    invalid_config = config;
    invalid_config.queue_limit_bytes = 0;

    if (channel_emulator_create(
            &invalid_config,
            test_output_callback,
            &test) != NULL) {
        printf("FAIL: Zero queue limit was accepted\n");
        return 1;
    }

    invalid_config = config;
    invalid_config.loss_probability = -0.1;

    if (channel_emulator_create(
            &invalid_config,
            test_output_callback,
            &test) != NULL) {
        printf("FAIL: Negative loss probability was accepted\n");
        return 1;
    }

    invalid_config = config;
    invalid_config.duplication_probability = 1.1;

    if (channel_emulator_create(
            &invalid_config,
            test_output_callback,
            &test) != NULL) {
        printf("FAIL: Duplication probability above 1 was accepted\n");
        return 1;
    }

    invalid_config = config;
    invalid_config.corruption_probability = NAN;

    if (channel_emulator_create(
            &invalid_config,
            test_output_callback,
            &test) != NULL) {
        printf("FAIL: NaN corruption probability was accepted\n");
        return 1;
    }
}

printf("PASS: Invalid configuration rejection\n");

pthread_cond_destroy(&test.condition);
pthread_mutex_destroy(&test.mutex);

printf("\nAll channel emulator tests passed!\n");

return 0;

}
