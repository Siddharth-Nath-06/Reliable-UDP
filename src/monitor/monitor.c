#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "monitor.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/constants.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

struct Monitor {
    bool started;
    bool finished;

    uint64_t start_time_ms;
    uint64_t finish_time_ms;

    FILE *event_log;
    bool log_failed;

    uint64_t packets_sent;
    uint64_t packets_received;

    uint64_t data_packets_sent;
    uint64_t data_packets_received;

    uint64_t control_packets_sent;
    uint64_t control_packets_received;

    uint64_t ack_packets_sent;
    uint64_t ack_packets_received;

    uint64_t nack_packets_sent;
    uint64_t nack_packets_received;

    uint64_t payload_bytes_sent;
    uint64_t payload_bytes_received;
    uint64_t bytes_delivered;

    uint64_t retransmissions;
    uint64_t timeouts;
    uint64_t duplicate_packets;
    uint64_t out_of_order_packets;

    uint64_t rtt_samples;
    uint64_t rtt_samples_ignored;
    uint64_t min_rtt_ms;
    uint64_t max_rtt_ms;
    long double rtt_sum_ms;

    uint64_t rto_samples;
    uint64_t min_rto_ms;
    uint64_t max_rto_ms;
    uint64_t last_rto_ms;
    long double rto_sum_ms;
};

/* ---------------------------------------------------------
 * Utility functions
 * --------------------------------------------------------- */

/*
 * Read a monotonic time in milliseconds.
 *
 * QueryPerformanceCounter is used on Windows.
 * CLOCK_MONOTONIC is used on POSIX systems.
 */
static bool monotonic_time_ms(uint64_t *result)
{
    if (result == NULL) {
        return false;
    }

#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0 ||
        counter.QuadPart < 0) {
        return false;
    }

    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;

    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remainder = ticks % ticks_per_second;

    if (seconds > UINT64_MAX / 1000U) {
        return false;
    }

    uint64_t whole_ms = seconds * 1000U;

    /*
     * Convert only the sub-second remainder. The long-double
     * intermediate avoids overflowing remainder * 1000.
     */
    uint64_t fractional_ms = (uint64_t)(
        ((long double)remainder * 1000.0L) /
        (long double)ticks_per_second
    );

    if (whole_ms > UINT64_MAX - fractional_ms) {
        return false;
    }

    *result = whole_ms + fractional_ms;
    return true;

#else
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec < 0) {
        return false;
    }

    uint64_t seconds = (uint64_t)value.tv_sec;

    if (seconds > UINT64_MAX / 1000U) {
        return false;
    }

    uint64_t whole_ms = seconds * 1000U;
    uint64_t fractional_ms = (uint64_t)value.tv_nsec / 1000000U;

    if (whole_ms > UINT64_MAX - fractional_ms) {
        return false;
    }

    *result = whole_ms + fractional_ms;
    return true;
#endif
}

/* Saturate counters instead of wrapping around on overflow. */
static uint64_t add_saturating(uint64_t current, uint64_t amount)
{
    if (UINT64_MAX - current < amount) {
        return UINT64_MAX;
    }

    return current + amount;
}

static uint64_t packet_elapsed_ms(const Monitor *monitor)
{
    if (monitor == NULL || !monitor->started) {
        return 0;
    }

    if (monitor->finished) {
        return monitor->finish_time_ms - monitor->start_time_ms;
    }

    uint64_t now_ms;

    if (!monotonic_time_ms(&now_ms) ||
        now_ms < monitor->start_time_ms) {
        return 0;
    }

    return now_ms - monitor->start_time_ms;
}

/*
 * Append one CSV event row.
 *
 * Event logging is best-effort. An I/O failure is remembered, but
 * must not change counters or control the transfer protocol.
 */
static void log_event(
    Monitor *monitor,
    const char *event_name,
    PacketNumber number,
    uint64_t value)
{
    if (monitor == NULL ||
        monitor->event_log == NULL ||
        monitor->log_failed) {
        return;
    }

    uint64_t elapsed_ms = packet_elapsed_ms(monitor);

    if (fprintf(
            monitor->event_log,
            "%" PRIu64 ",%s,%" PRIu16 ",%" PRIu64 "\n",
            elapsed_ms,
            event_name,
            number,
            value
        ) < 0) {
        monitor->log_failed = true;
    }
}

/*
 * Packet flag bits excluding protocol-identification bits.
 */
static uint8_t packet_type_flags(const Packet *packet)
{
    return (uint8_t)(
        packet->flags & (uint8_t)~FLAG_PROTOCOL_MASK
    );
}

static bool is_control_packet(uint8_t type)
{
    return (type & (FLAG_ACK | FLAG_NACK | FLAG_SYN |
                    FLAG_FIN | FLAG_WINDOW)) != 0;
}

/* ---------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------- */

Monitor *monitor_create(void)
{
    return calloc(1, sizeof(Monitor));
}

bool monitor_start(Monitor *monitor)
{
    if (monitor == NULL || monitor->started) {
        return false;
    }

    uint64_t now_ms;

    if (!monotonic_time_ms(&now_ms)) {
        return false;
    }

    monitor->start_time_ms = now_ms;
    monitor->finish_time_ms = now_ms;
    monitor->started = true;

    log_event(monitor, "transfer_started", 0, 0);

    return true;
}

bool monitor_finish(Monitor *monitor)
{
    if (monitor == NULL ||
        !monitor->started ||
        monitor->finished) {
        return false;
    }

    uint64_t now_ms;

    if (!monotonic_time_ms(&now_ms) ||
        now_ms < monitor->start_time_ms) {
        return false;
    }

    monitor->finish_time_ms = now_ms;
    monitor->finished = true;

    log_event(
        monitor,
        "transfer_finished",
        0,
        monitor->finish_time_ms - monitor->start_time_ms
    );

    if (monitor->event_log != NULL &&
        fflush(monitor->event_log) != 0) {
        monitor->log_failed = true;
    }

    return true;
}

bool monitor_open_event_log(
    Monitor *monitor,
    const char *path)
{
    if (monitor == NULL ||
        path == NULL ||
        path[0] == '\0' ||
        monitor->event_log != NULL) {
        return false;
    }

    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return false;
    }

    if (fprintf(
            file,
            "elapsed_ms,event,packet_number,value\n"
        ) < 0) {
        fclose(file);
        return false;
    }

    monitor->event_log = file;
    monitor->log_failed = false;

    return true;
}

bool monitor_close_event_log(Monitor *monitor)
{
    if (monitor == NULL) {
        return false;
    }

    if (monitor->event_log == NULL) {
        return !monitor->log_failed;
    }

    FILE *file = monitor->event_log;
    monitor->event_log = NULL;

    if (fflush(file) != 0) {
        monitor->log_failed = true;
    }

    if (fclose(file) != 0) {
        monitor->log_failed = true;
    }

    return !monitor->log_failed;
}

bool monitor_event_log_ok(const Monitor *monitor)
{
    if (monitor == NULL) {
        return false;
    }

    if (monitor->log_failed) {
        return false;
    }

    return monitor->event_log == NULL ||
           ferror(monitor->event_log) == 0;
}

/* ---------------------------------------------------------
 * Packet activity
 * --------------------------------------------------------- */

void monitor_record_packet_sent(
    Monitor *monitor,
    const Packet *packet)
{
    if (monitor == NULL || packet == NULL) {
        return;
    }

    monitor->packets_sent =
        add_saturating(monitor->packets_sent, 1);

    uint8_t type = packet_type_flags(packet);

    if ((type & FLAG_DATA) != 0) {
        monitor->data_packets_sent =
            add_saturating(monitor->data_packets_sent, 1);

        monitor->payload_bytes_sent = add_saturating(
            monitor->payload_bytes_sent,
            packet->payload_length
        );
    }

    if (is_control_packet(type)) {
        monitor->control_packets_sent =
            add_saturating(monitor->control_packets_sent, 1);
    }

    if ((type & FLAG_ACK) != 0) {
        monitor->ack_packets_sent =
            add_saturating(monitor->ack_packets_sent, 1);
    }

    if ((type & FLAG_NACK) != 0) {
        monitor->nack_packets_sent =
            add_saturating(monitor->nack_packets_sent, 1);
    }

    log_event(
        monitor,
        "packet_sent",
        packet->number,
        packet->payload_length
    );
}

void monitor_record_packet_received(
    Monitor *monitor,
    const Packet *packet)
{
    if (monitor == NULL || packet == NULL) {
        return;
    }

    monitor->packets_received =
        add_saturating(monitor->packets_received, 1);

    uint8_t type = packet_type_flags(packet);

    if ((type & FLAG_DATA) != 0) {
        monitor->data_packets_received =
            add_saturating(monitor->data_packets_received, 1);

        monitor->payload_bytes_received = add_saturating(
            monitor->payload_bytes_received,
            packet->payload_length
        );
    }

    if (is_control_packet(type)) {
        monitor->control_packets_received =
            add_saturating(monitor->control_packets_received, 1);
    }

    if ((type & FLAG_ACK) != 0) {
        monitor->ack_packets_received =
            add_saturating(monitor->ack_packets_received, 1);
    }

    if ((type & FLAG_NACK) != 0) {
        monitor->nack_packets_received =
            add_saturating(monitor->nack_packets_received, 1);
    }

    log_event(
        monitor,
        "packet_received",
        packet->number,
        packet->payload_length
    );
}

/* ---------------------------------------------------------
 * Reliability events
 * --------------------------------------------------------- */

void monitor_record_retransmission(
    Monitor *monitor,
    PacketNumber number)
{
    if (monitor == NULL) {
        return;
    }

    monitor->retransmissions =
        add_saturating(monitor->retransmissions, 1);

    log_event(monitor, "retransmission", number, 1);
}

void monitor_record_timeout(
    Monitor *monitor,
    PacketNumber number)
{
    if (monitor == NULL) {
        return;
    }

    monitor->timeouts =
        add_saturating(monitor->timeouts, 1);

    log_event(monitor, "timeout", number, 1);
}

void monitor_record_duplicate(
    Monitor *monitor,
    PacketNumber number)
{
    if (monitor == NULL) {
        return;
    }

    monitor->duplicate_packets =
        add_saturating(monitor->duplicate_packets, 1);

    log_event(monitor, "duplicate_packet", number, 1);
}

void monitor_record_out_of_order(
    Monitor *monitor,
    PacketNumber number)
{
    if (monitor == NULL) {
        return;
    }

    monitor->out_of_order_packets =
        add_saturating(monitor->out_of_order_packets, 1);

    log_event(monitor, "out_of_order_packet", number, 1);
}

/* ---------------------------------------------------------
 * RTT and RTO observations
 * --------------------------------------------------------- */

void monitor_record_rtt(
    Monitor *monitor,
    uint64_t rtt_ms,
    bool retransmitted)
{
    if (monitor == NULL) {
        return;
    }

    if (retransmitted) {
        monitor->rtt_samples_ignored = add_saturating(
            monitor->rtt_samples_ignored,
            1
        );

        log_event(
            monitor,
            "rtt_sample_ignored",
            0,
            rtt_ms
        );

        return;
    }

    if (monitor->rtt_samples == 0) {
        monitor->min_rtt_ms = rtt_ms;
        monitor->max_rtt_ms = rtt_ms;
    } else {
        if (rtt_ms < monitor->min_rtt_ms) {
            monitor->min_rtt_ms = rtt_ms;
        }

        if (rtt_ms > monitor->max_rtt_ms) {
            monitor->max_rtt_ms = rtt_ms;
        }
    }

    monitor->rtt_samples =
        add_saturating(monitor->rtt_samples, 1);

    monitor->rtt_sum_ms += (long double)rtt_ms;

    log_event(monitor, "rtt_sample", 0, rtt_ms);
}

void monitor_record_rto(
    Monitor *monitor,
    uint64_t rto_ms)
{
    if (monitor == NULL) {
        return;
    }

    if (monitor->rto_samples == 0) {
        monitor->min_rto_ms = rto_ms;
        monitor->max_rto_ms = rto_ms;
    } else {
        if (rto_ms < monitor->min_rto_ms) {
            monitor->min_rto_ms = rto_ms;
        }

        if (rto_ms > monitor->max_rto_ms) {
            monitor->max_rto_ms = rto_ms;
        }
    }

    monitor->rto_samples =
        add_saturating(monitor->rto_samples, 1);

    monitor->last_rto_ms = rto_ms;
    monitor->rto_sum_ms += (long double)rto_ms;

    log_event(monitor, "rto_value", 0, rto_ms);
}

void monitor_record_bytes_delivered(
    Monitor *monitor,
    uint64_t bytes)
{
    if (monitor == NULL) {
        return;
    }

    monitor->bytes_delivered = add_saturating(
        monitor->bytes_delivered,
        bytes
    );

    log_event(monitor, "bytes_delivered", 0, bytes);
}

/* ---------------------------------------------------------
 * Statistics snapshot
 * --------------------------------------------------------- */

bool monitor_get_stats(
    const Monitor *monitor,
    MonitorStats *stats)
{
    if (monitor == NULL || stats == NULL) {
        return false;
    }

    memset(stats, 0, sizeof(*stats));

    stats->packets_sent = monitor->packets_sent;
    stats->packets_received = monitor->packets_received;

    stats->data_packets_sent = monitor->data_packets_sent;
    stats->data_packets_received = monitor->data_packets_received;

    stats->control_packets_sent = monitor->control_packets_sent;
    stats->control_packets_received = monitor->control_packets_received;

    stats->ack_packets_sent = monitor->ack_packets_sent;
    stats->ack_packets_received = monitor->ack_packets_received;

    stats->nack_packets_sent = monitor->nack_packets_sent;
    stats->nack_packets_received = monitor->nack_packets_received;

    stats->payload_bytes_sent = monitor->payload_bytes_sent;
    stats->payload_bytes_received = monitor->payload_bytes_received;
    stats->bytes_delivered = monitor->bytes_delivered;

    stats->retransmissions = monitor->retransmissions;
    stats->timeouts = monitor->timeouts;
    stats->duplicate_packets = monitor->duplicate_packets;
    stats->out_of_order_packets = monitor->out_of_order_packets;

    stats->rtt_samples = monitor->rtt_samples;
    stats->rtt_samples_ignored = monitor->rtt_samples_ignored;
    stats->min_rtt_ms = monitor->min_rtt_ms;
    stats->max_rtt_ms = monitor->max_rtt_ms;

    if (monitor->rtt_samples > 0) {
        stats->average_rtt_ms = (double)(
            monitor->rtt_sum_ms /
            (long double)monitor->rtt_samples
        );
    }

    stats->rto_samples = monitor->rto_samples;
    stats->min_rto_ms = monitor->min_rto_ms;
    stats->max_rto_ms = monitor->max_rto_ms;
    stats->last_rto_ms = monitor->last_rto_ms;

    if (monitor->rto_samples > 0) {
        stats->average_rto_ms = (double)(
            monitor->rto_sum_ms /
            (long double)monitor->rto_samples
        );
    }

    if (monitor->started) {
        if (monitor->finished) {
            stats->duration_ms =
                monitor->finish_time_ms - monitor->start_time_ms;
        } else {
            uint64_t now_ms;

            if (!monotonic_time_ms(&now_ms) ||
                now_ms < monitor->start_time_ms) {
                return false;
            }

            stats->duration_ms = now_ms - monitor->start_time_ms;
        }
    }

    if (stats->duration_ms > 0) {
        stats->goodput_bytes_per_second =
            ((double)stats->bytes_delivered * 1000.0) /
            (double)stats->duration_ms;
    }

    return true;
}

/* ---------------------------------------------------------
 * CSV summary
 * --------------------------------------------------------- */

bool monitor_write_summary(
    const Monitor *monitor,
    const char *path)
{
    if (monitor == NULL || path == NULL || path[0] == '\0') {
        return false;
    }

    MonitorStats stats;

    if (!monitor_get_stats(monitor, &stats)) {
        return false;
    }

    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return false;
    }

    bool success = true;

    if (fprintf(
            file,
            "packets_sent,packets_received,"
            "data_packets_sent,data_packets_received,"
            "control_packets_sent,control_packets_received,"
            "ack_packets_sent,ack_packets_received,"
            "nack_packets_sent,nack_packets_received,"
            "payload_bytes_sent,payload_bytes_received,bytes_delivered,"
            "retransmissions,timeouts,duplicate_packets,out_of_order_packets,"
            "rtt_samples,rtt_samples_ignored,min_rtt_ms,average_rtt_ms,max_rtt_ms,"
            "rto_samples,min_rto_ms,average_rto_ms,max_rto_ms,last_rto_ms,"
            "duration_ms,goodput_bytes_per_second\n"
        ) < 0) {
        success = false;
    }

    if (success &&
        fprintf(
            file,
            "%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%.3f,%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%.3f\n",
            stats.packets_sent,
            stats.packets_received,
            stats.data_packets_sent,
            stats.data_packets_received,
            stats.control_packets_sent,
            stats.control_packets_received,
            stats.ack_packets_sent,
            stats.ack_packets_received,
            stats.nack_packets_sent,
            stats.nack_packets_received,
            stats.payload_bytes_sent,
            stats.payload_bytes_received,
            stats.bytes_delivered,
            stats.retransmissions,
            stats.timeouts,
            stats.duplicate_packets,
            stats.out_of_order_packets,
            stats.rtt_samples,
            stats.rtt_samples_ignored,
            stats.min_rtt_ms,
            stats.average_rtt_ms,
            stats.max_rtt_ms,
            stats.rto_samples,
            stats.min_rto_ms,
            stats.average_rto_ms,
            stats.max_rto_ms,
            stats.last_rto_ms,
            stats.duration_ms,
            stats.goodput_bytes_per_second
        ) < 0) {
        success = false;
    }

    if (fclose(file) != 0) {
        success = false;
    }

    return success;
}

/* ---------------------------------------------------------
 * Cleanup
 * --------------------------------------------------------- */

void monitor_destroy(Monitor *monitor)
{
    if (monitor == NULL) {
        return;
    }

    if (monitor->event_log != NULL) {
        (void)monitor_close_event_log(monitor);
    }

    free(monitor);
}