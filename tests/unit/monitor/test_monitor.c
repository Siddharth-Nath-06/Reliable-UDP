#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <errno.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/common/constants.h"
#include "../../../src/common/types.h"
#include "../../../src/monitor/monitor.h"

/* ---------------------------------------------------------
 * Helpers
 * --------------------------------------------------------- */

static void sleep_ms(unsigned long milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };

    while (nanosleep(&remaining, &remaining) == -1) {
        assert(errno == EINTR);
    }
#endif
}

static bool nearly_equal(double a, double b)
{
    double difference = a - b;

    if (difference < 0.0) {
        difference = -difference;
    }

    return difference <= 0.001 * (b >= 0.0 ? b + 1.0 : -b + 1.0);
}

static Packet make_test_packet(
    uint8_t flags,
    PacketNumber number,
    PayloadLength payload_length)
{
    Packet packet = {0};

    packet.flags = flags;
    packet.number = number;
    packet.payload_length = payload_length;

    /*
     * Monitor inspects metadata only; it does not own or retain
     * packet payloads. No allocation is needed for these tests.
     */
    packet.payload = NULL;

    return packet;
}

static char *read_entire_file(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long size = ftell(file);

    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    char *buffer = malloc((size_t)size + 1);

    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)size, file);

    if (bytes_read != (size_t)size || ferror(file)) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytes_read] = '\0';

    if (fclose(file) != 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

/* ---------------------------------------------------------
 * Creation and lifecycle
 * --------------------------------------------------------- */

static void test_creation_and_initial_stats(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    MonitorStats stats = {0};

    assert(monitor_get_stats(monitor, &stats));

    assert(stats.packets_sent == 0);
    assert(stats.packets_received == 0);
    assert(stats.data_packets_sent == 0);
    assert(stats.data_packets_received == 0);

    assert(stats.retransmissions == 0);
    assert(stats.timeouts == 0);
    assert(stats.duplicate_packets == 0);
    assert(stats.out_of_order_packets == 0);

    assert(stats.rtt_samples == 0);
    assert(stats.rtt_samples_ignored == 0);
    assert(stats.min_rtt_ms == 0);
    assert(stats.average_rtt_ms == 0.0);
    assert(stats.max_rtt_ms == 0);

    assert(stats.rto_samples == 0);
    assert(stats.min_rto_ms == 0);
    assert(stats.average_rto_ms == 0.0);
    assert(stats.max_rto_ms == 0);
    assert(stats.last_rto_ms == 0);

    assert(stats.duration_ms == 0);
    assert(stats.goodput_bytes_per_second == 0.0);

    /* Invalid lifecycle transitions must fail. */
    assert(!monitor_finish(monitor));

    assert(monitor_start(monitor));
    assert(!monitor_start(monitor));

    assert(monitor_finish(monitor));
    assert(!monitor_finish(monitor));

    /* A completed monitor can still provide its statistics. */
    assert(monitor_get_stats(monitor, &stats));

    monitor_destroy(monitor);

    /* NULL handling for public lifecycle/query functions. */
    assert(!monitor_start(NULL));
    assert(!monitor_finish(NULL));
    assert(!monitor_get_stats(NULL, &stats));
    assert(!monitor_get_stats(NULL, NULL));
    assert(!monitor_event_log_ok(NULL));

    monitor_destroy(NULL);

    puts("PASS: monitor creation and lifecycle");
}

/* ---------------------------------------------------------
 * Packet statistics
 * --------------------------------------------------------- */

static void test_packet_statistics(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    Packet data_sent = make_test_packet(
        FLAG_DATA | PROTOCOL_STOP_WAIT,
        0,
        100
    );

    Packet ack_sent = make_test_packet(
        FLAG_ACK | PROTOCOL_STOP_WAIT,
        0,
        0
    );

    Packet nack_sent = make_test_packet(
        FLAG_NACK | PROTOCOL_STOP_WAIT,
        1,
        0
    );

    Packet data_received = make_test_packet(
        FLAG_DATA | PROTOCOL_SELECTIVE_REPEAT,
        7,
        120
    );

    Packet ack_received = make_test_packet(
        FLAG_ACK | PROTOCOL_SELECTIVE_REPEAT,
        7,
        0
    );

    Packet nack_received = make_test_packet(
        FLAG_NACK | PROTOCOL_SELECTIVE_REPEAT,
        8,
        0
    );

    assert(monitor_start(monitor));

    monitor_record_packet_sent(monitor, &data_sent);
    monitor_record_packet_sent(monitor, &ack_sent);
    monitor_record_packet_sent(monitor, &nack_sent);

    monitor_record_packet_received(monitor, &data_received);
    monitor_record_packet_received(monitor, &ack_received);
    monitor_record_packet_received(monitor, &nack_received);

    MonitorStats stats = {0};

    assert(monitor_get_stats(monitor, &stats));

    assert(stats.packets_sent == 3);
    assert(stats.packets_received == 3);

    assert(stats.data_packets_sent == 1);
    assert(stats.data_packets_received == 1);

    assert(stats.control_packets_sent == 2);
    assert(stats.control_packets_received == 2);

    assert(stats.ack_packets_sent == 1);
    assert(stats.ack_packets_received == 1);

    assert(stats.nack_packets_sent == 1);
    assert(stats.nack_packets_received == 1);

    assert(stats.payload_bytes_sent == 100);
    assert(stats.payload_bytes_received == 120);

    /*
     * Monitor only observes the Packet; it does not retain the
     * pointer or allocate/free the packet's payload.
     */
    assert(data_sent.payload == NULL);
    assert(data_received.payload == NULL);

    assert(monitor_finish(monitor));
    monitor_destroy(monitor);

    puts("PASS: packet counters and payload-byte accounting");
}

/* ---------------------------------------------------------
 * Reliability statistics
 * --------------------------------------------------------- */

static void test_reliability_events(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    assert(monitor_start(monitor));

    monitor_record_retransmission(monitor, 4);
    monitor_record_retransmission(monitor, 4);
    monitor_record_timeout(monitor, 4);
    monitor_record_duplicate(monitor, 2);
    monitor_record_out_of_order(monitor, 7);
    monitor_record_out_of_order(monitor, 8);

    MonitorStats stats = {0};

    assert(monitor_get_stats(monitor, &stats));

    assert(stats.retransmissions == 2);
    assert(stats.timeouts == 1);
    assert(stats.duplicate_packets == 1);
    assert(stats.out_of_order_packets == 2);

    assert(monitor_finish(monitor));
    monitor_destroy(monitor);

    puts("PASS: retransmission, timeout, duplicate and ordering events");
}

/* ---------------------------------------------------------
 * RTT aggregation and Karn's algorithm
 * --------------------------------------------------------- */

static void test_rtt_statistics(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    assert(monitor_start(monitor));

    monitor_record_rtt(monitor, 10, false);
    monitor_record_rtt(monitor, 20, false);
    monitor_record_rtt(monitor, 30, false);

    /* Retransmitted observations must not enter RTT aggregates. */
    monitor_record_rtt(monitor, 900, true);

    MonitorStats stats = {0};

    assert(monitor_get_stats(monitor, &stats));

    assert(stats.rtt_samples == 3);
    assert(stats.rtt_samples_ignored == 1);

    assert(stats.min_rtt_ms == 10);
    assert(stats.max_rtt_ms == 30);
    assert(nearly_equal(stats.average_rtt_ms, 20.0));

    assert(monitor_finish(monitor));
    monitor_destroy(monitor);

    puts("PASS: RTT aggregates and retransmitted-sample exclusion");
}

/* ---------------------------------------------------------
 * RTO aggregation
 * --------------------------------------------------------- */

static void test_rto_statistics(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    assert(monitor_start(monitor));

    monitor_record_rto(monitor, 100);
    monitor_record_rto(monitor, 300);
    monitor_record_rto(monitor, 200);

    MonitorStats stats = {0};

    assert(monitor_get_stats(monitor, &stats));

    assert(stats.rto_samples == 3);
    assert(stats.min_rto_ms == 100);
    assert(stats.max_rto_ms == 300);
    assert(stats.last_rto_ms == 200);
    assert(nearly_equal(stats.average_rto_ms, 200.0));

    /* A zero RTO is still recorded as the value supplied. */
    monitor_record_rto(monitor, 0);

    assert(monitor_get_stats(monitor, &stats));
    assert(stats.rto_samples == 4);
    assert(stats.min_rto_ms == 0);
    assert(stats.max_rto_ms == 300);
    assert(stats.last_rto_ms == 0);
    assert(nearly_equal(stats.average_rto_ms, 150.0));

    assert(monitor_finish(monitor));
    monitor_destroy(monitor);

    puts("PASS: RTO aggregates");
}

/* ---------------------------------------------------------
 * Delivered bytes and goodput
 * --------------------------------------------------------- */

static void test_delivery_and_goodput(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    assert(monitor_start(monitor));

    monitor_record_bytes_delivered(monitor, 400);
    monitor_record_bytes_delivered(monitor, 600);

    sleep_ms(10);

    assert(monitor_finish(monitor));

    MonitorStats stats = {0};

    assert(monitor_get_stats(monitor, &stats));

    assert(stats.bytes_delivered == 1000);
    assert(stats.duration_ms > 0);

    double expected_goodput =
        (1000.0 * 1000.0) / (double)stats.duration_ms;

    assert(nearly_equal(
        stats.goodput_bytes_per_second,
        expected_goodput
    ));

    /*
     * Goodput uses delivered bytes, not transmitted bytes.
     * Without any delivered data, it should remain zero.
     */
    Monitor *empty = monitor_create();
    assert(empty != NULL);

    assert(monitor_start(empty));
    assert(monitor_finish(empty));

    assert(monitor_get_stats(empty, &stats));
    assert(stats.bytes_delivered == 0);
    assert(stats.goodput_bytes_per_second == 0.0);

    monitor_destroy(empty);
    monitor_destroy(monitor);

    puts("PASS: delivered bytes, duration and goodput");
}

/* ---------------------------------------------------------
 * CSV event logging
 * --------------------------------------------------------- */

static void test_csv_event_log(void)
{
    const char *path = "monitor_test_events.csv";
    (void)remove(path);

    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    assert(!monitor_open_event_log(monitor, NULL));
    assert(!monitor_open_event_log(monitor, ""));
    assert(monitor_open_event_log(monitor, path));

    /* Opening a second log while one is open must fail. */
    assert(!monitor_open_event_log(monitor, path));

    assert(monitor_event_log_ok(monitor));
    assert(monitor_start(monitor));

    Packet packet = make_test_packet(
        FLAG_DATA | PROTOCOL_GO_BACK_N,
        12,
        64
    );

    monitor_record_packet_sent(monitor, &packet);
    monitor_record_retransmission(monitor, 12);
    monitor_record_timeout(monitor, 12);
    monitor_record_rtt(monitor, 50, false);
    monitor_record_rtt(monitor, 90, true);
    monitor_record_rto(monitor, 250);
    monitor_record_bytes_delivered(monitor, 64);

    assert(monitor_finish(monitor));
    assert(monitor_close_event_log(monitor));
    assert(monitor_event_log_ok(monitor));

    char *contents = read_entire_file(path);
    assert(contents != NULL);

    assert(strstr(
        contents,
        "elapsed_ms,event,packet_number,value"
    ) != NULL);

    assert(strstr(contents, "transfer_started") != NULL);
    assert(strstr(contents, "transfer_finished") != NULL);
    assert(strstr(contents, "packet_sent") != NULL);
    assert(strstr(contents, "retransmission") != NULL);
    assert(strstr(contents, "timeout") != NULL);
    assert(strstr(contents, "rtt_sample") != NULL);
    assert(strstr(contents, "rtt_sample_ignored") != NULL);
    assert(strstr(contents, "rto_value") != NULL);
    assert(strstr(contents, "bytes_delivered") != NULL);

    free(contents);
    assert(remove(path) == 0);

    monitor_destroy(monitor);

    puts("PASS: CSV event log");
}

/* ---------------------------------------------------------
 * CSV summary
 * --------------------------------------------------------- */

static void test_csv_summary(void)
{
    const char *path = "monitor_test_summary.csv";
    (void)remove(path);

    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    assert(monitor_start(monitor));

    Packet data = make_test_packet(
        FLAG_DATA | PROTOCOL_STOP_WAIT,
        0,
        128
    );

    Packet ack = make_test_packet(
        FLAG_ACK | PROTOCOL_STOP_WAIT,
        0,
        0
    );

    monitor_record_packet_sent(monitor, &data);
    monitor_record_packet_received(monitor, &ack);

    monitor_record_retransmission(monitor, 0);
    monitor_record_timeout(monitor, 0);

    monitor_record_rtt(monitor, 20, false);
    monitor_record_rto(monitor, 100);

    monitor_record_bytes_delivered(monitor, 128);

    assert(monitor_finish(monitor));
    assert(monitor_write_summary(monitor, path));

    char *contents = read_entire_file(path);
    assert(contents != NULL);

    assert(strstr(contents, "packets_sent,packets_received") != NULL);
    assert(strstr(contents, "retransmissions,timeouts") != NULL);
    assert(strstr(contents, "average_rtt_ms") != NULL);
    assert(strstr(contents, "average_rto_ms") != NULL);
    assert(strstr(contents, "goodput_bytes_per_second") != NULL);

    /*
     * First row values should include:
     * packets sent = 1, packets received = 1,
     * data packets sent = 1, ACKs received = 1,
     * bytes sent = 128, delivered bytes = 128,
     * retransmissions = 1, timeouts = 1.
     */
    assert(strstr(contents, "1,1,1,0") != NULL);
    assert(strstr(contents, "128,0,128") != NULL);
    assert(strstr(contents, ",1,1,0,0,1,0,20,20.000,20") != NULL);

    free(contents);
    assert(remove(path) == 0);

    monitor_destroy(monitor);

    puts("PASS: CSV summary");
}

/* ---------------------------------------------------------
 * Error handling and NULL inputs
 * --------------------------------------------------------- */

static void test_null_event_inputs(void)
{
    Monitor *monitor = monitor_create();
    assert(monitor != NULL);

    Packet packet = make_test_packet(FLAG_DATA, 0, 10);

    /* These functions are no-ops for NULL Monitor pointers. */
    monitor_record_packet_sent(NULL, &packet);
    monitor_record_packet_received(NULL, &packet);

    monitor_record_retransmission(NULL, 0);
    monitor_record_timeout(NULL, 0);
    monitor_record_duplicate(NULL, 0);
    monitor_record_out_of_order(NULL, 0);

    monitor_record_rtt(NULL, 10, false);
    monitor_record_rto(NULL, 100);
    monitor_record_bytes_delivered(NULL, 10);

    assert(!monitor_close_event_log(NULL));
    assert(!monitor_open_event_log(NULL, "unused.csv"));
    assert(!monitor_write_summary(NULL, "unused.csv"));
    assert(!monitor_write_summary(monitor, NULL));
    assert(!monitor_write_summary(monitor, ""));

    assert(!monitor_get_stats(monitor, NULL));

    monitor_destroy(monitor);

    puts("PASS: NULL arguments and error handling");
}

/* ---------------------------------------------------------
 * Entry point
 * --------------------------------------------------------- */

int main(void)
{
    test_creation_and_initial_stats();
    test_packet_statistics();
    test_reliability_events();
    test_rtt_statistics();
    test_rto_statistics();
    test_delivery_and_goodput();
    test_csv_event_log();
    test_csv_summary();
    test_null_event_inputs();

    puts("\nAll Monitor tests passed.");
    return 0;
}