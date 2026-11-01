#ifndef DRIVER_H
#define DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../common/types.h"
#include "../transport/transport.h"
#include "../channel/channel_emulator.h"
#include "../monitor/monitor.h"

typedef enum {
    DRIVER_MODE_DEMO,
    DRIVER_MODE_EXPERIMENT
} DriverRunMode;

typedef enum {
    DRIVER_STATUS_OK,
    DRIVER_STATUS_INVALID_CONFIG,
    DRIVER_STATUS_INITIALIZATION_FAILED,
    DRIVER_STATUS_MONITOR_ERROR,
    DRIVER_STATUS_HANDSHAKE_TIMEOUT,
    DRIVER_STATUS_HANDSHAKE_REJECTED,
    DRIVER_STATUS_TRANSFER_TIMEOUT,
    DRIVER_STATUS_TRANSPORT_ERROR,
    DRIVER_STATUS_FILE_ERROR,
    DRIVER_STATUS_PROTOCOL_ERROR,
    DRIVER_STATUS_INTEGRITY_FAILED,
    DRIVER_STATUS_REMOTE_REJECTED,
    DRIVER_STATUS_INTERNAL_ERROR
} DriverStatus;

typedef enum {
    DRIVER_PHASE_INITIALIZING,
    DRIVER_PHASE_WAITING_FOR_PEER,
    DRIVER_PHASE_NEGOTIATING,
    DRIVER_PHASE_TRANSFERRING,
    DRIVER_PHASE_VERIFYING,
    DRIVER_PHASE_FINISHING,
    DRIVER_PHASE_COMPLETED,
    DRIVER_PHASE_FAILED
} DriverPhase;

typedef struct {
    DriverPhase phase;
    uint64_t bytes_processed;
    uint64_t total_bytes;
} DriverProgress;

typedef void (*DriverProgressCallback)(
    void *context,
    const DriverProgress *progress
);

typedef struct {
    DriverRunMode mode;
    bool enable_monitoring;

    TransportRole role;

    const char *sender_address;
    uint16_t sender_port;

    const char *receiver_address;
    uint16_t receiver_port;

    ArqProtocol protocol;
    uint16_t window_size;
    size_t chunk_size;

    uint64_t initial_rto_ms;
    uint64_t min_rto_ms;
    uint64_t max_rto_ms;

    uint32_t receive_timeout_ms;
    uint32_t control_timeout_ms;
    uint32_t max_control_retries;
    uint64_t max_transfer_duration_ms;

    const char *source_path;
    const char *destination_path;

    ChannelEmulatorConfig channel;

    const char *event_log_path;
    const char *summary_path;

    DriverProgressCallback progress_callback;
    void *progress_context;
} DriverConfig;

typedef struct {
    DriverRunMode mode;
    TransportRole role;

    bool file_size_known;
    uint64_t file_size_bytes;

    uint64_t bytes_read_from_source;
    uint64_t bytes_written_to_destination;

    bool integrity_verified;

    bool monitor_stats_available;
    bool monitor_output_failed;
    MonitorStats metrics;
} DriverResult;

DriverStatus driver_run(
    const DriverConfig *config,
    DriverResult *result
);

const char *driver_status_string(DriverStatus status);

#endif