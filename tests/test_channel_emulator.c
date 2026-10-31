#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "../src/channel/channel_emulator.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#define TEST_HISTORY_CAPACITY 64
#define TEST_MAX_DATAGRAM_SIZE 256

typedef struct {
    ChannelDirection direction;
    uint8_t data[TEST_MAX_DATAGRAM_SIZE];
    size_t length;
} CallbackRecord;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t callback_count;
    size_t record_count;
    bool overflow;
    CallbackRecord records[TEST_HISTORY_CAPACITY];
} TestContext;

static void test_output_callback(
    void *context,
    ChannelDirection direction,
    const uint8_t *data,
    size_t length)
{
    TestContext *test = context;

    pthread_mutex_lock(&test->mutex);

    test->callback_count++;
    if (test->record_count < TEST_HISTORY_CAPACITY &&
        length <= TEST_MAX_DATAGRAM_SIZE) {
        CallbackRecord *record = &test->records[test->record_count++];
        record->direction = direction;
        record->length = length;
        memcpy(record->data, data, length);
    } else {
        test->overflow = true;
    }

    pthread_cond_broadcast(&test->condition);
    pthread_mutex_unlock(&test->mutex);
}

static void test_reset_context(TestContext *test)
{
    pthread_mutex_lock(&test->mutex);
    test->callback_count = 0;
    test->record_count = 0;
    test->overflow = false;
    memset(test->records, 0, sizeof(test->records));
    pthread_mutex_unlock(&test->mutex);
}

static size_t test_callback_count(TestContext *test)
{
    size_t count;
    pthread_mutex_lock(&test->mutex);
    count = test->callback_count;
    pthread_mutex_unlock(&test->mutex);
    return count;
}

static bool test_wait_for_callbacks(
    TestContext *test,
    size_t expected_count,
    uint32_t timeout_ms)
{
    struct timespec deadline;
    int result = 0;

    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return false;
    }

    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&test->mutex);
    while (test->callback_count < expected_count && result == 0) {
        result = pthread_cond_timedwait(
            &test->condition, &test->mutex, &deadline);
    }

    bool reached = test->callback_count >= expected_count;
    pthread_mutex_unlock(&test->mutex);
    return reached;
}

static void test_sleep_ms(uint32_t milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec remaining;
    remaining.tv_sec = (time_t)(milliseconds / 1000U);
    remaining.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        /* Continue sleeping for the remaining interval. */
    }
#endif
}

static bool test_record_matches(
    TestContext *test,
    size_t index,
    ChannelDirection direction,
    const uint8_t *expected_data,
    size_t expected_length)
{
    bool matches = false;

    pthread_mutex_lock(&test->mutex);
    if (index < test->record_count) {
        const CallbackRecord *record = &test->records[index];
        matches = record->direction == direction &&
                  record->length == expected_length &&
                  memcmp(record->data, expected_data, expected_length) == 0;
    }
    pthread_mutex_unlock(&test->mutex);
    return matches;
}

static bool test_record_has_single_bit_difference(
    TestContext *test,
    size_t index,
    const uint8_t *original,
    size_t original_length)
{
    bool valid = false;
    size_t differing_bits = 0;

    pthread_mutex_lock(&test->mutex);
    if (index < test->record_count &&
        test->records[index].length == original_length) {
        const CallbackRecord *record = &test->records[index];
        for (size_t i = 0; i < original_length; i++) {
            uint8_t difference = (uint8_t)(record->data[i] ^ original[i]);
            while (difference != 0) {
                differing_bits++;
                difference &= (uint8_t)(difference - 1U);
            }
        }
        valid = differing_bits == 1;
    }
    pthread_mutex_unlock(&test->mutex);
    return valid;
}

static ChannelEmulatorConfig test_default_config(void)
{
    ChannelEmulatorConfig config = {0};
    config.bandwidth_bps = 1000000U;
    config.delay_ms = 0U;
    config.jitter_ms = 0U;
    config.loss_probability = 0.0;
    config.duplication_probability = 0.0;
    config.corruption_probability = 0.0;
    config.reordering_probability = 0.0;
    config.reordering_delay_ms = 0U;
    config.queue_limit_bytes = 1024U * 1024U;
    config.random_seed = 12345U;
    return config;
}

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", (message)); \
        goto cleanup; \
    } \
} while (0)

static bool test_delivery_direction_and_input_copy(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t outbound[] = "Outbound test";
    const uint8_t inbound[] = "Inbound test";
    uint8_t mutable_input[sizeof(outbound)];

    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for delivery/copy test");

    memcpy(mutable_input, outbound, sizeof(outbound));
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    mutable_input, sizeof(mutable_input)),
            "outbound submission accepted");
    memset(mutable_input, 'X', sizeof(mutable_input));
    REQUIRE(test_wait_for_callbacks(test, 1, 2000),
            "outbound callback arrives");
    REQUIRE(test_record_matches(test, 0, CHANNEL_DIRECTION_OUTBOUND,
                                outbound, sizeof(outbound)),
            "outbound data is copied and direction is preserved");

    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_INBOUND,
                                    inbound, sizeof(inbound)),
            "inbound submission accepted");
    REQUIRE(test_wait_for_callbacks(test, 2, 2000),
            "inbound callback arrives");
    REQUIRE(test_record_matches(test, 1, CHANNEL_DIRECTION_INBOUND,
                                inbound, sizeof(inbound)),
            "inbound direction and contents are preserved");

    passed = true;
    printf("PASS: direction routing and input-data copying\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_duplication(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t data[] = "Duplicate me";

    config.duplication_probability = 1.0;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for duplication test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    data, sizeof(data)),
            "submit packet for duplication test");
    REQUIRE(test_wait_for_callbacks(test, 2, 2000),
            "duplicated packet produces two callbacks");
    REQUIRE(test_callback_count(test) == 2,
            "one duplicated datagram produces exactly two callbacks");
    REQUIRE(test_record_matches(test, 0, CHANNEL_DIRECTION_OUTBOUND,
                                data, sizeof(data)),
            "first duplicate contents");
    REQUIRE(test_record_matches(test, 1, CHANNEL_DIRECTION_OUTBOUND,
                                data, sizeof(data)),
            "second duplicate contents");

    passed = true;
    printf("PASS: packet duplication\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_loss(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t data[] = "Drop me";

    config.loss_probability = 1.0;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for loss test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    data, sizeof(data)),
            "loss-test packet accepted into queue");
    (void)test_wait_for_callbacks(test, 1, 150);
    REQUIRE(test_callback_count(test) == 0,
            "100-percent loss produces no output callback");

    passed = true;
    printf("PASS: packet loss\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_single_bit_corruption(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t data[] = "Corrupt exactly one bit";

    config.corruption_probability = 1.0;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for corruption test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    data, sizeof(data)),
            "corruption-test packet accepted");
    REQUIRE(test_wait_for_callbacks(test, 1, 2000),
            "corrupted packet is delivered");
    REQUIRE(test_callback_count(test) == 1,
            "corruption produces exactly one callback");
    REQUIRE(test_record_has_single_bit_difference(
                test, 0, data, sizeof(data)),
            "corruption flips exactly one bit and preserves packet length");

    passed = true;
    printf("PASS: single-bit corruption\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_configured_delay(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t data[] = "Delayed packet";

    config.delay_ms = 350U;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for delay test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    data, sizeof(data)),
            "delay-test packet accepted");
    test_sleep_ms(100U);
    REQUIRE(test_callback_count(test) == 0,
            "packet is not emitted substantially before configured delay");
    REQUIRE(test_wait_for_callbacks(test, 1, 2000),
            "packet eventually arrives after configured delay");
    REQUIRE(test_record_matches(test, 0, CHANNEL_DIRECTION_OUTBOUND,
                                data, sizeof(data)),
            "delayed packet contents remain unchanged");

    passed = true;
    printf("PASS: configured channel delay\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_symmetric_jitter(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t data[] = "Jitter test";

    /* Seed 1 selects a negative jitter offset for the first scheduling draw. */
    config.delay_ms = 1000U;
    config.jitter_ms = 500U;
    config.random_seed = 1U;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for jitter test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    data, sizeof(data)),
            "jitter-test packet accepted");

    /* Symmetric jitter with this seed schedules below 900 ms. A one-sided
       nonnegative jitter implementation would not deliver this packet yet. */
    REQUIRE(test_wait_for_callbacks(test, 1, 900U),
            "negative jitter can reduce the configured base delay");
    REQUIRE(test_record_matches(test, 0, CHANNEL_DIRECTION_OUTBOUND,
                                data, sizeof(data)),
            "jitter does not modify packet contents");

    passed = true;
    printf("PASS: symmetric jitter can decrease base delay\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_high_bandwidth_precision(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    uint8_t payload[1000];
    const size_t packet_count = 256;

    memset(payload, 0xA7, sizeof(payload));
    config.bandwidth_bps = 1000000000U; /* 1 Gbit/s: 8 us per 1000-byte datagram. */
    config.queue_limit_bytes = 1024U * 1024U;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for high-bandwidth precision test");

    for (size_t i = 0; i < packet_count; i++) {
        REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                        payload, sizeof(payload)),
                "high-bandwidth packet accepted");
    }

    /* The simulated serialization total is about 2.05 ms. This generous
       deadline distinguishes microsecond scheduling from a 1 ms minimum
       imposed separately on every packet, while allowing OS scheduling slack. */
    REQUIRE(test_wait_for_callbacks(test, packet_count, 200U),
            "high-rate packets are not artificially throttled to 1 ms each");
    REQUIRE(test_callback_count(test) == packet_count,
            "all high-bandwidth packets are delivered exactly once");

    passed = true;
    printf("PASS: high-bandwidth sub-millisecond scheduling precision\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_reordering(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t first[] = "First packet";
    const uint8_t second[] = "Second packet";

    /* Seed 32 causes the first reorder draw to succeed and the second to fail. */
    config.random_seed = 32U;
    config.reordering_probability = 0.5;
    config.reordering_delay_ms = 300U;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for reordering test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    first, sizeof(first)),
            "submit first packet");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    second, sizeof(second)),
            "submit second packet");
    REQUIRE(test_wait_for_callbacks(test, 2, 2000),
            "both reordered packets are delivered");
    REQUIRE(test_callback_count(test) == 2,
            "exactly two callbacks occur in reordering test");
    REQUIRE(test_record_matches(test, 0, CHANNEL_DIRECTION_OUTBOUND,
                                second, sizeof(second)),
            "second submitted packet is delivered first");
    REQUIRE(test_record_matches(test, 1, CHANNEL_DIRECTION_OUTBOUND,
                                first, sizeof(first)),
            "first submitted packet is delivered second");

    passed = true;
    printf("PASS: deterministic packet reordering\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_shared_bandwidth(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    uint8_t outbound[100];
    uint8_t inbound[100];

    memset(outbound, 0xA5, sizeof(outbound));
    memset(inbound, 0x5A, sizeof(inbound));
    config.bandwidth_bps = 8000U; /* 100 bytes takes 100 ms. */

    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for bandwidth test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    outbound, sizeof(outbound)),
            "submit outbound bandwidth packet");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_INBOUND,
                                    inbound, sizeof(inbound)),
            "submit inbound bandwidth packet");

    test_sleep_ms(150U);
    REQUIRE(test_callback_count(test) < 2,
            "opposite directions share one bandwidth timeline");
    REQUIRE(test_wait_for_callbacks(test, 2, 2000),
            "both packets eventually pass through the shared channel");
    REQUIRE(test_callback_count(test) == 2,
            "bandwidth test produces exactly two callbacks");
    REQUIRE(test_record_matches(test, 0, CHANNEL_DIRECTION_OUTBOUND,
                                outbound, sizeof(outbound)),
            "outbound packet occupies the first bandwidth slot");
    REQUIRE(test_record_matches(test, 1, CHANNEL_DIRECTION_INBOUND,
                                inbound, sizeof(inbound)),
            "inbound packet occupies the next bandwidth slot");

    passed = true;
    printf("PASS: shared bandwidth across both directions\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_queue_capacity_and_shutdown(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t first[] = "First packet";
    const uint8_t second[8] = "Second";

    config.delay_ms = 10000U;
    config.queue_limit_bytes = 16U;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for queue test");
    REQUIRE(channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                    first, sizeof(first)),
            "first packet fits queue capacity");
    REQUIRE(!channel_emulator_submit(emulator, CHANNEL_DIRECTION_INBOUND,
                                     second, sizeof(second)),
            "queue limit applies across both directions");

    /* Destroy should wake the worker rather than wait for the 10-second delay. */
    channel_emulator_destroy(emulator);
    emulator = NULL;
    test_sleep_ms(50U);
    REQUIRE(test_callback_count(test) == 0,
            "shutdown discards pending work and makes no late callbacks");

    passed = true;
    printf("PASS: shared queue limit and shutdown with pending work\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_invalid_inputs_and_config(TestContext *test)
{
    bool passed = false;
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    const uint8_t byte = 0x42;

    REQUIRE(channel_emulator_create(NULL, test_output_callback, test) == NULL,
            "null config rejected");

    ChannelEmulatorConfig invalid = config;
    invalid.bandwidth_bps = 0;
    REQUIRE(channel_emulator_create(&invalid, test_output_callback, test) == NULL,
            "zero bandwidth rejected");

    invalid = config;
    invalid.queue_limit_bytes = 0;
    REQUIRE(channel_emulator_create(&invalid, test_output_callback, test) == NULL,
            "zero queue capacity rejected");

    invalid = config;
    invalid.loss_probability = -0.1;
    REQUIRE(channel_emulator_create(&invalid, test_output_callback, test) == NULL,
            "negative probability rejected");

    invalid = config;
    invalid.duplication_probability = 1.1;
    REQUIRE(channel_emulator_create(&invalid, test_output_callback, test) == NULL,
            "probability above one rejected");

    invalid = config;
    invalid.corruption_probability = NAN;
    REQUIRE(channel_emulator_create(&invalid, test_output_callback, test) == NULL,
            "NaN probability rejected");

    REQUIRE(channel_emulator_create(&config, NULL, test) == NULL,
            "null output callback rejected");

    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    REQUIRE(emulator != NULL, "create emulator for argument tests");
    REQUIRE(!channel_emulator_submit(NULL, CHANNEL_DIRECTION_OUTBOUND,
                                     &byte, sizeof(byte)),
            "null emulator rejected");
    REQUIRE(!channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                     NULL, sizeof(byte)),
            "null input data rejected");
    REQUIRE(!channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                     &byte, 0),
            "empty datagram rejected");
    REQUIRE(!channel_emulator_submit(emulator, (ChannelDirection)99,
                                     &byte, sizeof(byte)),
            "invalid direction rejected");

    passed = true;
    printf("PASS: invalid configuration and submit arguments\n");
cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool run_random_trial(
    TestContext *test,
    uint8_t *delivered_ids,
    size_t *delivered_count)
{
    ChannelEmulator *emulator = NULL;
    ChannelEmulatorConfig config = test_default_config();
    bool passed = false;

    config.delay_ms = 80U;
    config.loss_probability = 0.5;
    config.random_seed = 987654U;
    test_reset_context(test);
    emulator = channel_emulator_create(&config, test_output_callback, test);
    if (emulator == NULL) {
        goto cleanup;
    }

    for (uint8_t id = 0; id < 24; id++) {
        if (!channel_emulator_submit(emulator, CHANNEL_DIRECTION_OUTBOUND,
                                     &id, sizeof(id))) {
            goto cleanup;
        }
    }

    /* All scheduled packets are due by this point; no random decisions
       are made in the worker, so the survivor sequence should repeat. */
    test_sleep_ms(300U);

    pthread_mutex_lock(&test->mutex);
    if (test->overflow || test->record_count > TEST_HISTORY_CAPACITY) {
        pthread_mutex_unlock(&test->mutex);
        goto cleanup;
    }

    *delivered_count = test->record_count;
    for (size_t i = 0; i < test->record_count; i++) {
        if (test->records[i].length != 1 ||
            test->records[i].direction != CHANNEL_DIRECTION_OUTBOUND) {
            pthread_mutex_unlock(&test->mutex);
            goto cleanup;
        }
        delivered_ids[i] = test->records[i].data[0];
    }
    pthread_mutex_unlock(&test->mutex);
    passed = true;

cleanup:
    channel_emulator_destroy(emulator);
    return passed;
}

static bool test_seeded_random_reproducibility(TestContext *test)
{
    uint8_t first_trial[TEST_HISTORY_CAPACITY];
    uint8_t second_trial[TEST_HISTORY_CAPACITY];
    size_t first_count = 0;
    size_t second_count = 0;

    if (!run_random_trial(test, first_trial, &first_count)) {
        printf("FAIL: first seeded random trial completes\n");
        return false;
    }
    if (!run_random_trial(test, second_trial, &second_count)) {
        printf("FAIL: second seeded random trial completes\n");
        return false;
    }
    if (first_count != second_count) {
        printf("FAIL: same seed produces same number of survivors\n");
        return false;
    }
    if (memcmp(first_trial, second_trial, first_count) != 0) {
        printf("FAIL: same seed produces same survivor order\n");
        return false;
    }

    printf("PASS: fixed-seed decisions are reproducible\n");
    return true;
}

int main(void)
{
    TestContext test = {0};
    unsigned passed = 0;
    const unsigned total = 12;

    if (pthread_mutex_init(&test.mutex, NULL) != 0) {
        printf("FAIL: initialize test mutex\n");
        return 1;
    }
    if (pthread_cond_init(&test.condition, NULL) != 0) {
        pthread_mutex_destroy(&test.mutex);
        printf("FAIL: initialize test condition\n");
        return 1;
    }

    passed += test_delivery_direction_and_input_copy(&test);
    passed += test_duplication(&test);
    passed += test_loss(&test);
    passed += test_single_bit_corruption(&test);
    passed += test_configured_delay(&test);
    passed += test_symmetric_jitter(&test);
    passed += test_reordering(&test);
    passed += test_high_bandwidth_precision(&test);
    passed += test_shared_bandwidth(&test);
    passed += test_queue_capacity_and_shutdown(&test);
    passed += test_invalid_inputs_and_config(&test);
    passed += test_seeded_random_reproducibility(&test);

    pthread_cond_destroy(&test.condition);
    pthread_mutex_destroy(&test.mutex);

    if (passed != total) {
        printf("\n%u/%u channel emulator tests passed.\n",
               passed, total);
        return 1;
    }

    printf("\nAll %u channel emulator tests passed.\n", total);
    return 0;
}