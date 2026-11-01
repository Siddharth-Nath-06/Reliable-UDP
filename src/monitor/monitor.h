#ifndef MONITOR_H
#define MONITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../common/types.h"

/*
 * Runtime Monitor
 *
 * Collects measurements for transfer experiments.
 * This module is observational: it never changes protocol behavior.
 *
 * Calls on one Monitor instance must be serialized by the caller.
 * Packet pointers passed to recording functions are borrowed and
 * are not retained.
 */

typedef struct Monitor Monitor;

/*
 * Snapshot of accumulated metrics.
 *
 * When no eligible RTT or RTO samples exist, the corresponding
 * minimum, maximum, and average values are zero.
 *
 * Goodput is calculated using bytes_delivered / duration_seconds.
 */
typedef struct {
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

    /* RTT statistics use only samples not associated with retransmits. */
    uint64_t rtt_samples;
    uint64_t rtt_samples_ignored;
    uint64_t min_rtt_ms;
    uint64_t max_rtt_ms;
    double average_rtt_ms;

    /* RTO statistics include all values passed to monitor_record_rto(). */
    uint64_t rto_samples;
    uint64_t min_rto_ms;
    uint64_t max_rto_ms;
    uint64_t last_rto_ms;
    double average_rto_ms;

    uint64_t duration_ms;
    double goodput_bytes_per_second;
} MonitorStats;

/*
 * Create an empty Monitor.
 * Returns NULL on allocation failure.
 */
Monitor *monitor_create(void);

/*
 * Start measuring transfer duration.
 * A Monitor can be started only once.
 */
bool monitor_start(Monitor *monitor);

/*
 * Finish measuring transfer duration.
 * Returns false if the Monitor was not started, is already finished,
 * or the monotonic clock could not be read.
 */
bool monitor_finish(Monitor *monitor);

/*
 * Open an optional event log.
 *
 * The file is overwritten if it already exists.
 * This operation does not affect accumulated statistics.
 */
bool monitor_open_event_log(
    Monitor *monitor,
    const char *path
);

/*
 * Close the event log.
 * Returns false if flushing or closing the log fails.
 */
bool monitor_close_event_log(Monitor *monitor);

/*
 * Report whether event logging has encountered an I/O error.
 * Returns true when logging is disabled and no logging error occurred.
 */
bool monitor_event_log_ok(const Monitor *monitor);

/*
 * Record a packet transmission or reception.
 * Packet fields are read immediately and not retained.
 *
 * Record sent packets when the Driver submits them to Transport.
 * Record received packets at the relevant receive stage, consistently
 * using the same stage when comparing experiments.
 */
void monitor_record_packet_sent(
    Monitor *monitor,
    const Packet *packet
);

void monitor_record_packet_received(
    Monitor *monitor,
    const Packet *packet
);

/* Record a retransmission attempt for a particular sequence number. */
void monitor_record_retransmission(
    Monitor *monitor,
    PacketNumber number
);

/* Record a timeout detected by ARQ. */
void monitor_record_timeout(
    Monitor *monitor,
    PacketNumber number
);

/* Record a duplicate or out-of-order packet identified by ARQ. */
void monitor_record_duplicate(
    Monitor *monitor,
    PacketNumber number
);

void monitor_record_out_of_order(
    Monitor *monitor,
    PacketNumber number
);

/*
 * Record an RTT observation.
 *
 * When retransmitted is true, the observation is counted as ignored
 * for RTT statistics, following Karn's algorithm. It is not included
 * in the min, max, or average RTT.
 */
void monitor_record_rtt(
    Monitor *monitor,
    uint64_t rtt_ms,
    bool retransmitted
);

/* Record an RTO value used or observed during the transfer. */
void monitor_record_rto(
    Monitor *monitor,
    uint64_t rto_ms
);

/*
 * Record bytes delivered as useful file data.
 * Call once for each successfully delivered file-data chunk.
 */
void monitor_record_bytes_delivered(
    Monitor *monitor,
    uint64_t bytes
);

/*
 * Copy the current metrics into caller-owned storage.
 *
 * If the transfer is still running, duration and goodput are computed
 * from the current monotonic time. Returns false if that clock read
 * fails.
 */
bool monitor_get_stats(
    const Monitor *monitor,
    MonitorStats *stats
);

/*
 * Write the current statistics as a one-row CSV summary.
 * The file is overwritten if it already exists.
 */
bool monitor_write_summary(
    const Monitor *monitor,
    const char *path
);

/* Close any open log and free the Monitor. */
void monitor_destroy(Monitor *monitor);

#endif