#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "driver.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define TEST_SOURCE_PATH "tests/unit/driver/driver_source_fixture.bin"
#define TEST_DESTINATION_PATH "tests/unit/driver/driver_destination_fixture.bin"
#define TEST_MISSING_PATH "tests/unit/driver/does_not_exist.bin"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL: %s:%d: %s\n",                           \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static unsigned failures;

typedef struct {
    DriverPhase last_phase;
    unsigned callback_count;
} ProgressTracker;

typedef struct {
    DriverConfig config;
    DriverResult result;
    DriverStatus status;
} DriverThreadArgs;

static void sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    (void)nanosleep(&delay, NULL);
#endif
}

static void track_progress(void *context, const DriverProgress *progress)
{
    ProgressTracker *tracker = context;

    if (tracker == NULL || progress == NULL) {
        return;
    }

    tracker->last_phase = progress->phase;
    tracker->callback_count++;
}

static DriverConfig make_config(TransportRole role, ArqProtocol protocol)
{
    DriverConfig config = {0};

    config.mode = DRIVER_MODE_DEMO;
    config.enable_monitoring = false;
    config.role = role;

    config.sender_address = "127.0.0.1";
    config.sender_port = 48121;
    config.receiver_address = "127.0.0.1";
    config.receiver_port = 48122;

    config.protocol = protocol;
    config.window_size = protocol == ARQ_STOP_WAIT ? 1U : 4U;
    config.chunk_size = 512;

    config.initial_rto_ms = 100;
    config.min_rto_ms = 20;
    config.max_rto_ms = 1000;

    config.receive_timeout_ms = 10;
    config.control_timeout_ms = 150;
    config.max_control_retries = 2;
    config.max_transfer_duration_ms = 5000;

    config.source_path = TEST_MISSING_PATH;
    config.destination_path = TEST_DESTINATION_PATH;

    config.channel.bandwidth_bps = 100000000U;
    config.channel.delay_ms = 0;
    config.channel.jitter_ms = 0;
    config.channel.loss_probability = 0.0;
    config.channel.duplication_probability = 0.0;
    config.channel.corruption_probability = 0.0;
    config.channel.reordering_probability = 0.0;
    config.channel.reordering_delay_ms = 0;
    config.channel.queue_limit_bytes = 1024U * 1024U;
    config.channel.random_seed = 12345U;

    return config;
}

static void test_status_strings(void)
{
    CHECK(strcmp(driver_status_string(DRIVER_STATUS_OK), "Success") == 0);
    CHECK(driver_status_string(DRIVER_STATUS_INVALID_CONFIG) != NULL);
    CHECK(driver_status_string(DRIVER_STATUS_INTERNAL_ERROR) != NULL);
    CHECK(driver_status_string((DriverStatus)999) != NULL);
    CHECK(driver_status_string((DriverStatus)999)[0] != '\0');
}

static void test_null_arguments(void)
{
    DriverResult result;

    CHECK(driver_run(NULL, NULL) == DRIVER_STATUS_INVALID_CONFIG);
    CHECK(driver_run(NULL, &result) == DRIVER_STATUS_INVALID_CONFIG);
    DriverConfig config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    CHECK(driver_run(&config, NULL) == DRIVER_STATUS_INVALID_CONFIG);
}

static void test_invalid_configurations(void)
{
    DriverResult result;
    DriverConfig config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);

    config.source_path = NULL;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.window_size = 0;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_STOP_WAIT);
    config.window_size = 2;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.protocol = (ArqProtocol)99;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.min_rto_ms = 101;
    config.initial_rto_ms = 100;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.channel.loss_probability = 1.1;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.channel.corruption_probability = NAN;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.event_log_path = "events.csv";
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);
    config.mode = DRIVER_MODE_EXPERIMENT;
    config.enable_monitoring = false;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_RECEIVER, ARQ_GO_BACK_N);
    config.destination_path = NULL;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);

    config = make_config(TRANSPORT_RECEIVER, ARQ_GO_BACK_N);
    config.receive_timeout_ms = 0;
    CHECK(driver_run(&config, &result) == DRIVER_STATUS_INVALID_CONFIG);
}

static void test_missing_source_file_and_progress(void)
{
    DriverResult result;
    ProgressTracker tracker = {0};
    DriverConfig config = make_config(TRANSPORT_SENDER, ARQ_GO_BACK_N);

    (void)remove(TEST_MISSING_PATH);
    config.progress_callback = track_progress;
    config.progress_context = &tracker;

    CHECK(driver_run(&config, &result) == DRIVER_STATUS_FILE_ERROR);
    CHECK(tracker.callback_count >= 2U);
    CHECK(tracker.last_phase == DRIVER_PHASE_FAILED);
    CHECK(!result.file_size_known);
}

static bool write_fixture(const char *path, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)((i * 37U + 11U) & 0xFFU);
        if (fwrite(&value, 1, 1, file) != 1) {
            fclose(file);
            return false;
        }
    }

    return fclose(file) == 0;
}

static bool files_equal(const char *first_path, const char *second_path)
{
    FILE *first = fopen(first_path, "rb");
    FILE *second = fopen(second_path, "rb");

    if (first == NULL || second == NULL) {
        if (first != NULL) {
            fclose(first);
        }
        if (second != NULL) {
            fclose(second);
        }
        return false;
    }

    bool equal = true;
    unsigned char first_buffer[1024];
    unsigned char second_buffer[1024];

    for (;;) {
        size_t first_count = fread(first_buffer, 1, sizeof(first_buffer), first);
        size_t second_count = fread(second_buffer, 1, sizeof(second_buffer), second);

        if (first_count != second_count ||
            memcmp(first_buffer, second_buffer, first_count) != 0) {
            equal = false;
            break;
        }

        if (first_count < sizeof(first_buffer)) {
            if (ferror(first) || ferror(second)) {
                equal = false;
            }
            break;
        }
    }

    fclose(first);
    fclose(second);
    return equal;
}

static void *run_driver_thread(void *opaque)
{
    DriverThreadArgs *args = opaque;
    args->status = driver_run(&args->config, &args->result);
    return NULL;
}

static void test_end_to_end_transfer(ArqProtocol protocol,
                                     size_t file_size,
                                     unsigned port_offset)
{
    if (!write_fixture(TEST_SOURCE_PATH, file_size)) {
        CHECK(false);
        return;
    }
    (void)remove(TEST_DESTINATION_PATH);

    DriverThreadArgs receiver = {0};
    receiver.config = make_config(TRANSPORT_RECEIVER, protocol);
    receiver.config.sender_port = (uint16_t)(48121U + port_offset);
    receiver.config.receiver_port = (uint16_t)(48122U + port_offset);
    receiver.config.max_transfer_duration_ms = 5000;

    DriverConfig sender = make_config(TRANSPORT_SENDER, protocol);
    sender.sender_port = receiver.config.sender_port;
    sender.receiver_port = receiver.config.receiver_port;
    sender.source_path = TEST_SOURCE_PATH;
    sender.max_transfer_duration_ms = 5000;

    pthread_t receiver_thread;
    int thread_status = pthread_create(
        &receiver_thread,
        NULL,
        run_driver_thread,
        &receiver
    );

    CHECK(thread_status == 0);
    if (thread_status != 0) {
        return;
    }

    /* Let the receiver bind its UDP endpoint before the sender starts. */
    sleep_ms(100);

    DriverResult sender_result = {0};
    DriverStatus sender_status = driver_run(&sender, &sender_result);
    fprintf(stderr, "  sender returned: %s\n",
            driver_status_string(sender_status));
    fflush(stderr);

    int join_status = pthread_join(receiver_thread, NULL);
    fprintf(stderr, "  receiver returned: %s\n",
            driver_status_string(receiver.status));
    fflush(stderr);
    CHECK(join_status == 0);

    CHECK(sender_status == DRIVER_STATUS_OK);
    CHECK(receiver.status == DRIVER_STATUS_OK);

    CHECK(sender_result.file_size_known);
    CHECK(sender_result.file_size_bytes == (uint64_t)file_size);
    CHECK(sender_result.bytes_read_from_source == (uint64_t)file_size);
    CHECK(sender_result.integrity_verified);

    CHECK(receiver.result.file_size_known);
    CHECK(receiver.result.file_size_bytes == (uint64_t)file_size);
    CHECK(receiver.result.bytes_written_to_destination == (uint64_t)file_size);
    CHECK(receiver.result.integrity_verified);

    CHECK(files_equal(TEST_SOURCE_PATH, TEST_DESTINATION_PATH));

    (void)remove(TEST_SOURCE_PATH);
    (void)remove(TEST_DESTINATION_PATH);
}

int main(void)
{
    puts("[ RUN      ] status strings");
    fflush(stdout);
    test_status_strings();
    puts("[       OK ] status strings");
    fflush(stdout);

    puts("[ RUN      ] null arguments");
    fflush(stdout);
    test_null_arguments();
    puts("[       OK ] null arguments");
    fflush(stdout);

    puts("[ RUN      ] invalid configurations");
    fflush(stdout);
    test_invalid_configurations();
    puts("[       OK ] invalid configurations");
    fflush(stdout);

    puts("[ RUN      ] missing source and progress callback");
    fflush(stdout);
    test_missing_source_file_and_progress();
    puts("[       OK ] missing source and progress callback");
    fflush(stdout);

    /* Exercise every ARQ mode over loopback with no simulated impairments. */
    puts("[ RUN      ] Stop-and-Wait loopback transfer (3500 bytes)");
    fflush(stdout);
    test_end_to_end_transfer(ARQ_STOP_WAIT, 3500U, 0U);
    puts("[       DONE ] Stop-and-Wait loopback transfer");
    fflush(stdout);

    puts("[ RUN      ] Go-Back-N loopback transfer (3500 bytes)");
    fflush(stdout);
    test_end_to_end_transfer(ARQ_GO_BACK_N, 3500U, 10U);
    puts("[       DONE ] Go-Back-N loopback transfer");
    fflush(stdout);

    puts("[ RUN      ] Selective Repeat loopback transfer (3500 bytes)");
    fflush(stdout);
    test_end_to_end_transfer(ARQ_SELECTIVE_REPEAT, 3500U, 20U);
    puts("[       DONE ] Selective Repeat loopback transfer");
    fflush(stdout);

    /* Empty-file transfer exercises FIN without any DATA packets. */
    puts("[ RUN      ] empty-file Stop-and-Wait transfer");
    fflush(stdout);
    test_end_to_end_transfer(ARQ_STOP_WAIT, 0U, 30U);
    puts("[       DONE ] empty-file Stop-and-Wait transfer");
    fflush(stdout);

    if (failures != 0U) {
        fprintf(stderr, "Driver tests failed: %u failure(s).\n", failures);
        return EXIT_FAILURE;
    }

    puts("All Driver tests passed.");
    return EXIT_SUCCESS;
}
