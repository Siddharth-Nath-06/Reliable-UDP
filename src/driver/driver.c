#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "driver.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "../arq/arq.h"
#include "../common/constants.h"
#include "../file/file_manager.h"
#include "../file/sha256.h"
#include "../packet/packet.h"
#include "../reconstruct/reconstructor.h"

/*
 * Transfer metadata:
 *
 *   8 bytes  - file size, unsigned, big-endian
 *   32 bytes - SHA-256 digest
 */
#define DRIVER_FILE_SIZE_FIELD_SIZE 8U
#define DRIVER_DIGEST_SIZE SHA256_DIGEST_SIZE
#define DRIVER_METADATA_SIZE \
    (DRIVER_FILE_SIZE_FIELD_SIZE + DRIVER_DIGEST_SIZE)

#define DRIVER_DATAGRAM_CAPACITY \
    (PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE)

#define DRIVER_PROTOCOL_FLAGS_MASK FLAG_PROTOCOL_MASK

typedef struct {
    uint64_t sent_at_ms;
    bool used;
    bool retransmitted;
} DriverRttEntry;

typedef struct {
    const DriverConfig *config;
    DriverResult *result;

    Transport *transport;
    ArqContext *arq;
    Monitor *monitor;

    FileManager file_manager;
    bool file_manager_open;

    Reconstructor reconstructor;
    bool reconstructor_open;

    uint64_t start_time_ms;

    uint64_t expected_file_size;
    SHA256Digest expected_digest;

    uint64_t bytes_written;
    uint16_t proposed_window_size;
    uint16_t agreed_window_size;

    bool source_eof;
    bool monitor_started;
} DriverRuntime;


/* ------------------------------------------------------------------------- */
/* Time and progress                                                         */
/* ------------------------------------------------------------------------- */

static uint64_t driver_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return ((uint64_t)ts.tv_sec * 1000U) +
           ((uint64_t)ts.tv_nsec / 1000000U);
#endif
}

static bool driver_deadline_expired(const DriverRuntime *runtime)
{
    uint64_t limit = runtime->config->max_transfer_duration_ms;

    if (limit == 0) {
        return false;
    }

    return driver_now_ms() - runtime->start_time_ms >= limit;
}

static void driver_report_progress(
    DriverRuntime *runtime,
    DriverPhase phase,
    uint64_t bytes_processed,
    uint64_t total_bytes)
{
    if (runtime->config->progress_callback == NULL) {
        return;
    }

    DriverProgress progress = {
        .phase = phase,
        .bytes_processed = bytes_processed,
        .total_bytes = total_bytes
    };

    runtime->config->progress_callback(
        runtime->config->progress_context,
        &progress
    );
}


/* ------------------------------------------------------------------------- */
/* Configuration helpers                                                     */
/* ------------------------------------------------------------------------- */

static uint8_t driver_protocol_flag(ArqProtocol protocol)
{
    switch (protocol) {
        case ARQ_STOP_WAIT:
            return PROTOCOL_STOP_WAIT;

        case ARQ_GO_BACK_N:
            return PROTOCOL_GO_BACK_N;

        case ARQ_SELECTIVE_REPEAT:
            return PROTOCOL_SELECTIVE_REPEAT;

        default:
            return PROTOCOL_RESERVED;
    }
}

static bool driver_valid_protocol(ArqProtocol protocol)
{
    return protocol == ARQ_STOP_WAIT ||
           protocol == ARQ_GO_BACK_N ||
           protocol == ARQ_SELECTIVE_REPEAT;
}

static bool driver_valid_channel(
    const ChannelEmulatorConfig *channel)
{
    if (channel == NULL ||
        channel->bandwidth_bps == 0 ||
        channel->queue_limit_bytes == 0) {
        return false;
    }

    if (!(channel->loss_probability >= 0.0 &&
          channel->loss_probability <= 1.0)) {
        return false;
    }

    if (!(channel->duplication_probability >= 0.0 &&
          channel->duplication_probability <= 1.0)) {
        return false;
    }

    if (!(channel->corruption_probability >= 0.0 &&
          channel->corruption_probability <= 1.0)) {
        return false;
    }

    if (!(channel->reordering_probability >= 0.0 &&
          channel->reordering_probability <= 1.0)) {
        return false;
    }

    return true;
}

static bool driver_valid_config(const DriverConfig *config)
{
    if (config == NULL) {
        return false;
    }

    if (config->mode != DRIVER_MODE_DEMO &&
        config->mode != DRIVER_MODE_EXPERIMENT) {
        return false;
    }

    if (config->role != TRANSPORT_SENDER &&
        config->role != TRANSPORT_RECEIVER) {
        return false;
    }

    if (config->mode == DRIVER_MODE_EXPERIMENT &&
        !config->enable_monitoring) {
        return false;
    }

    if (!config->enable_monitoring &&
        (config->event_log_path != NULL ||
         config->summary_path != NULL)) {
        return false;
    }

    if (config->sender_address == NULL ||
        config->receiver_address == NULL ||
        config->sender_address[0] == '\0' ||
        config->receiver_address[0] == '\0' ||
        config->sender_port == 0 ||
        config->receiver_port == 0) {
        return false;
    }

    if (!driver_valid_protocol(config->protocol)) {
        return false;
    }

    if (config->window_size == 0 ||
        config->window_size > ARQ_MAX_WINDOW_SIZE) {
        return false;
    }

    if (config->protocol == ARQ_STOP_WAIT &&
        config->window_size != 1) {
        return false;
    }

    if (config->initial_rto_ms == 0 ||
        config->min_rto_ms == 0 ||
        config->max_rto_ms == 0 ||
        config->min_rto_ms > config->initial_rto_ms ||
        config->initial_rto_ms > config->max_rto_ms) {
        return false;
    }

    if (config->receive_timeout_ms == 0 ||
        config->control_timeout_ms == 0) {
        return false;
    }

    if (config->role == TRANSPORT_SENDER) {
        if (config->source_path == NULL ||
            config->source_path[0] == '\0' ||
            config->chunk_size == 0 ||
            config->chunk_size > MAX_PAYLOAD_SIZE ||
            !driver_valid_channel(&config->channel)) {
            return false;
        }
    } else {
        if (config->destination_path == NULL ||
            config->destination_path[0] == '\0') {
            return false;
        }
    }

    return true;
}


/* ------------------------------------------------------------------------- */
/* Packet helpers                                                             */
/* ------------------------------------------------------------------------- */

static uint8_t driver_packet_type(const Packet *packet)
{
    return packet->flags & (uint8_t)~DRIVER_PROTOCOL_FLAGS_MASK;
}

static bool driver_protocol_matches(
    const DriverRuntime *runtime,
    const Packet *packet)
{
    return (packet->flags & DRIVER_PROTOCOL_FLAGS_MASK) ==
           driver_protocol_flag(runtime->config->protocol);
}

/*
 * Send a packet through Packet Handler and Transport.
 *
 * Transport acceptance means the packet was accepted locally,
 * not that it was delivered remotely.
 */
static bool driver_send_packet(
    DriverRuntime *runtime,
    Packet *packet)
{
    uint8_t buffer[DRIVER_DATAGRAM_CAPACITY];
    size_t serialized_size = 0;

    if (!packet_serialize(
            packet,
            buffer,
            sizeof(buffer),
            &serialized_size)) {
        return false;
    }

    if (!transport_send(
            runtime->transport,
            buffer,
            serialized_size)) {
        return false;
    }

    if (runtime->monitor != NULL) {
        monitor_record_packet_sent(runtime->monitor, packet);
    }

    return true;
}

static bool driver_send_control(
    DriverRuntime *runtime,
    uint8_t flags,
    PacketNumber number)
{
    Packet packet = {0};

    flags |= driver_protocol_flag(runtime->config->protocol);

    if (!packet_create_control(&packet, flags, number)) {
        return false;
    }

    bool sent = driver_send_packet(runtime, &packet);
    packet_release_payload(&packet);

    return sent;
}

static bool driver_send_control_payload(
    DriverRuntime *runtime,
    uint8_t flags,
    PacketNumber number,
    const uint8_t *payload,
    size_t payload_length)
{
    Packet packet = {0};

    flags |= driver_protocol_flag(runtime->config->protocol);

    if (!packet_create_control_with_payload(
            &packet,
            flags,
            number,
            payload,
            payload_length)) {
        return false;
    }

    bool sent = driver_send_packet(runtime, &packet);
    packet_release_payload(&packet);

    return sent;
}

/*
 * Return:
 *   1: valid packet received
 *   0: timeout or malformed/corrupt packet
 *  -1: transport failure
 *
 * Ownership of a valid packet's payload passes to the caller.
 */
static int driver_receive_packet(
    DriverRuntime *runtime,
    Packet *packet,
    uint32_t timeout_ms)
{
    uint8_t buffer[DRIVER_DATAGRAM_CAPACITY];
    size_t received_length = 0;

    TransportReceiveStatus status = transport_receive(
        runtime->transport,
        buffer,
        sizeof(buffer),
        &received_length,
        timeout_ms
    );

    if (status == TRANSPORT_RECEIVE_TIMEOUT) {
        return 0;
    }

    if (status == TRANSPORT_RECEIVE_ERROR) {
        return -1;
    }

    ArqPacketInput input = {0};

    if (!packet_receive(buffer, received_length, &input)) {
        return -1;
    }

    if (!input.valid) {
        packet_release_payload(&input.packet);
        return 0;
    }

    *packet = input.packet;

    if (runtime->monitor != NULL) {
        monitor_record_packet_received(runtime->monitor, packet);
    }

    return 1;
}


/* ------------------------------------------------------------------------- */
/* Metadata encoding                                                          */
/* ------------------------------------------------------------------------- */

static void driver_encode_u64(uint8_t *buffer, uint64_t value)
{
    for (size_t i = 0; i < DRIVER_FILE_SIZE_FIELD_SIZE; ++i) {
        unsigned shift =
            (unsigned)((DRIVER_FILE_SIZE_FIELD_SIZE - 1U - i) * 8U);

        buffer[i] = (uint8_t)(value >> shift);
    }
}

static uint64_t driver_decode_u64(const uint8_t *buffer)
{
    uint64_t value = 0;

    for (size_t i = 0; i < DRIVER_FILE_SIZE_FIELD_SIZE; ++i) {
        value = (value << 8) | buffer[i];
    }

    return value;
}

static void driver_build_metadata(
    uint8_t metadata[DRIVER_METADATA_SIZE],
    uint64_t file_size,
    const SHA256Digest *digest)
{
    driver_encode_u64(metadata, file_size);

    memcpy(
        metadata + DRIVER_FILE_SIZE_FIELD_SIZE,
        digest->data,
        DRIVER_DIGEST_SIZE
    );
}

static bool driver_parse_metadata(
    const Packet *packet,
    uint64_t *file_size,
    SHA256Digest *digest)
{
    if (packet == NULL ||
        file_size == NULL ||
        digest == NULL ||
        packet->payload == NULL ||
        packet->payload_length != DRIVER_METADATA_SIZE) {
        return false;
    }

    *file_size = driver_decode_u64(packet->payload);

    memcpy(
        digest->data,
        packet->payload + DRIVER_FILE_SIZE_FIELD_SIZE,
        DRIVER_DIGEST_SIZE
    );

    return true;
}


/* ------------------------------------------------------------------------- */
/* ARQ and Transport initialization                                           */
/* ------------------------------------------------------------------------- */

static bool driver_create_arq(
    DriverRuntime *runtime,
    uint16_t window_size)
{
    runtime->arq = arq_create(
        runtime->config->protocol,
        window_size,
        runtime->config->initial_rto_ms,
        runtime->config->min_rto_ms,
        runtime->config->max_rto_ms
    );

    if (runtime->arq == NULL) {
        return false;
    }

    runtime->agreed_window_size = window_size;

    if (runtime->monitor != NULL &&
        runtime->config->role == TRANSPORT_SENDER) {
        monitor_record_rto(
            runtime->monitor,
            arq_get_rto(runtime->arq)
        );
    }

    return true;
}

static bool driver_create_transport(DriverRuntime *runtime)
{
    TransportConfig transport_config = {
        .role = runtime->config->role,
        .sender_address = runtime->config->sender_address,
        .sender_port = runtime->config->sender_port,
        .receiver_address = runtime->config->receiver_address,
        .receiver_port = runtime->config->receiver_port,
        .channel_config =
            runtime->config->role == TRANSPORT_SENDER
                ? &runtime->config->channel
                : NULL
    };

    runtime->transport = transport_init(&transport_config);
    return runtime->transport != NULL;
}


/* ------------------------------------------------------------------------- */
/* Sender handshake                                                           */
/* ------------------------------------------------------------------------- */

static DriverStatus driver_sender_handshake(DriverRuntime *runtime)
{
    uint8_t metadata[DRIVER_METADATA_SIZE];

    uint64_t file_size = file_manager_get_size(
        &runtime->file_manager
    );

    SHA256Digest digest = {0};

    if (file_manager_get_sha256(
            &runtime->file_manager,
            &digest) != 0) {
        return DRIVER_STATUS_FILE_ERROR;
    }

    runtime->expected_file_size = file_size;
    runtime->expected_digest = digest;

    runtime->result->file_size_known = true;
    runtime->result->file_size_bytes = file_size;

    driver_build_metadata(metadata, file_size, &digest);

    driver_report_progress(
        runtime,
        DRIVER_PHASE_NEGOTIATING,
        0,
        file_size
    );

    uint8_t syn_flags =
        (uint8_t)(FLAG_SYN | FLAG_WINDOW |
                  driver_protocol_flag(runtime->config->protocol));

    for (uint32_t attempt = 0;
         attempt <= runtime->config->max_control_retries;
         ++attempt) {

        if (driver_deadline_expired(runtime)) {
            return DRIVER_STATUS_HANDSHAKE_TIMEOUT;
        }

        if (!driver_send_control_payload(
                runtime,
                syn_flags,
                runtime->config->window_size,
                metadata,
                sizeof(metadata))) {
            return DRIVER_STATUS_TRANSPORT_ERROR;
        }

        uint64_t wait_start = driver_now_ms();

        while (driver_now_ms() - wait_start <
               runtime->config->control_timeout_ms) {

            if (driver_deadline_expired(runtime)) {
                return DRIVER_STATUS_HANDSHAKE_TIMEOUT;
            }

            uint64_t elapsed = driver_now_ms() - wait_start;
            uint64_t remaining =
                runtime->config->control_timeout_ms - elapsed;

            uint32_t wait_ms = runtime->config->receive_timeout_ms;

            if ((uint64_t)wait_ms > remaining) {
                wait_ms = (uint32_t)remaining;
            }

            if (wait_ms == 0) {
                break;
            }

            Packet incoming = {0};

            int received = driver_receive_packet(
                runtime,
                &incoming,
                wait_ms
            );

            if (received < 0) {
                return DRIVER_STATUS_TRANSPORT_ERROR;
            }

            if (received == 0) {
                continue;
            }

            uint8_t type = driver_packet_type(&incoming);

            /* A peer using a different protocol still signals rejection
             * with SYN|NACK|WINDOW. Classify that before protocol matching. */
            if (type == (FLAG_SYN | FLAG_NACK | FLAG_WINDOW)) {
                packet_release_payload(&incoming);
                return DRIVER_STATUS_HANDSHAKE_REJECTED;
            }

            if (!driver_protocol_matches(runtime, &incoming)) {
                packet_release_payload(&incoming);
                return DRIVER_STATUS_PROTOCOL_ERROR;
            }

            if (type == (FLAG_SYN | FLAG_ACK | FLAG_WINDOW)) {
                uint16_t accepted_window = incoming.number;

                packet_release_payload(&incoming);

                if (accepted_window == 0 ||
                    accepted_window > runtime->config->window_size ||
                    accepted_window > ARQ_MAX_WINDOW_SIZE) {
                    return DRIVER_STATUS_PROTOCOL_ERROR;
                }

                if (runtime->config->protocol == ARQ_STOP_WAIT &&
                    accepted_window != 1) {
                    return DRIVER_STATUS_PROTOCOL_ERROR;
                }

                if (!driver_create_arq(runtime, accepted_window)) {
                    return DRIVER_STATUS_INITIALIZATION_FAILED;
                }

                return DRIVER_STATUS_OK;
            }

            packet_release_payload(&incoming);
        }
    }

    return DRIVER_STATUS_HANDSHAKE_TIMEOUT;
}


/* ------------------------------------------------------------------------- */
/* Receiver handshake                                                         */
/* ------------------------------------------------------------------------- */

static DriverStatus driver_receiver_handshake(DriverRuntime *runtime)
{
    driver_report_progress(
        runtime,
        DRIVER_PHASE_WAITING_FOR_PEER,
        0,
        0
    );

    for (;;) {
        if (driver_deadline_expired(runtime)) {
            return DRIVER_STATUS_HANDSHAKE_TIMEOUT;
        }

        Packet incoming = {0};

        int received = driver_receive_packet(
            runtime,
            &incoming,
            runtime->config->receive_timeout_ms
        );

        if (received < 0) {
            return DRIVER_STATUS_TRANSPORT_ERROR;
        }

        if (received == 0) {
            continue;
        }

        uint8_t type = driver_packet_type(&incoming);

        if (type != (FLAG_SYN | FLAG_WINDOW)) {
            packet_release_payload(&incoming);
            continue;
        }

        if (!driver_protocol_matches(runtime, &incoming)) {
            PacketNumber requested_window = incoming.number;
            packet_release_payload(&incoming);

            (void)requested_window;

            if (!driver_send_control(
                    runtime,
                    FLAG_SYN | FLAG_NACK | FLAG_WINDOW,
                    0)) {
                return DRIVER_STATUS_TRANSPORT_ERROR;
            }

            return DRIVER_STATUS_HANDSHAKE_REJECTED;
        }

        uint16_t proposed_window = incoming.number;

        if (proposed_window == 0 ||
            proposed_window > ARQ_MAX_WINDOW_SIZE ||
            !driver_parse_metadata(
                &incoming,
                &runtime->expected_file_size,
                &runtime->expected_digest)) {

            packet_release_payload(&incoming);

            if (!driver_send_control(
                    runtime,
                    FLAG_SYN | FLAG_NACK | FLAG_WINDOW,
                    0)) {
                return DRIVER_STATUS_TRANSPORT_ERROR;
            }

            return DRIVER_STATUS_PROTOCOL_ERROR;
        }

        packet_release_payload(&incoming);

        runtime->proposed_window_size = proposed_window;
        uint16_t accepted_window = proposed_window;

        if (accepted_window > runtime->config->window_size) {
            accepted_window = runtime->config->window_size;
        }

        if (runtime->config->protocol == ARQ_STOP_WAIT) {
            if (proposed_window != 1) {
                (void)driver_send_control(
                    runtime,
                    FLAG_SYN | FLAG_NACK | FLAG_WINDOW,
                    0
                );

                return DRIVER_STATUS_HANDSHAKE_REJECTED;
            }

            accepted_window = 1;
        }

        if (!driver_create_arq(runtime, accepted_window)) {
            return DRIVER_STATUS_INITIALIZATION_FAILED;
        }

        if (reconstructor_open(
                &runtime->reconstructor,
                runtime->config->destination_path) != 0) {
            (void)driver_send_control(
                runtime,
                FLAG_SYN | FLAG_NACK | FLAG_WINDOW,
                0
            );

            return DRIVER_STATUS_FILE_ERROR;
        }

        runtime->reconstructor_open = true;
        runtime->result->file_size_known = true;
        runtime->result->file_size_bytes =
            runtime->expected_file_size;

        if (!driver_send_control(
                runtime,
                FLAG_SYN | FLAG_ACK | FLAG_WINDOW,
                accepted_window)) {
            return DRIVER_STATUS_TRANSPORT_ERROR;
        }

        return DRIVER_STATUS_OK;
    }
}


/* ------------------------------------------------------------------------- */
/* Sender data transfer                                                       */
/* ------------------------------------------------------------------------- */

static DriverStatus driver_send_retransmissions(
    DriverRuntime *runtime,
    DriverRttEntry *rtt_table)
{
    bool retransmissions_pending =
        arq_should_retransmit(runtime->arq);

    PacketNumber timed_out_number;

    while (arq_get_timeout_event(runtime->arq, &timed_out_number)) {
        if (runtime->monitor != NULL) {
            monitor_record_timeout(
                runtime->monitor,
                timed_out_number
            );
        }
    }

    if (!retransmissions_pending) {
        return DRIVER_STATUS_OK;
    }

    for (;;) {
        Packet packet = {0};

        if (!arq_get_retransmission(runtime->arq, &packet)) {
            break;
        }

        DriverRttEntry *entry = &rtt_table[packet.number];

        if (entry->used) {
            entry->retransmitted = true;
        }

        PacketNumber retransmitted_number = packet.number;
        bool sent = driver_send_packet(runtime, &packet);
        packet_release_payload(&packet);

        if (!sent) {
            return DRIVER_STATUS_TRANSPORT_ERROR;
        }

        if (runtime->monitor != NULL) {
            monitor_record_retransmission(
                runtime->monitor,
                retransmitted_number
            );
        }
    }

    return DRIVER_STATUS_OK;
}

static void driver_process_ack_rtt(
    DriverRuntime *runtime,
    const Packet *ack,
    DriverRttEntry *rtt_table,
    PacketNumber previous_base,
    size_t previous_outstanding)
{
    if (previous_outstanding == 0) {
        return;
    }

    PacketNumber ack_number = ack->number;
    uint64_t now = driver_now_ms();

    DriverRttEntry *sample = &rtt_table[ack_number];

    if (sample->used) {
        uint64_t elapsed = now - sample->sent_at_ms;

        if (elapsed == 0) {
            elapsed = 1;
        }

        arq_record_rtt(
            runtime->arq,
            elapsed,
            sample->retransmitted
        );

        if (runtime->monitor != NULL) {
            monitor_record_rtt(
                runtime->monitor,
                elapsed,
                sample->retransmitted
            );

            if (runtime->config->role == TRANSPORT_SENDER &&
                !sample->retransmitted) {
                monitor_record_rto(
                    runtime->monitor,
                    arq_get_rto(runtime->arq)
                );
            }
        }
    }

    if (runtime->config->protocol == ARQ_SELECTIVE_REPEAT) {
        sample->used = false;
        return;
    }

    uint16_t distance =
        (uint16_t)(ack_number - previous_base);

    if ((size_t)distance >= previous_outstanding) {
        return;
    }

    size_t acknowledged_count = (size_t)distance + 1U;

    for (size_t i = 0; i < acknowledged_count; ++i) {
        PacketNumber number =
            (PacketNumber)(previous_base + i);

        rtt_table[number].used = false;
    }
}

static DriverStatus driver_sender_transfer(DriverRuntime *runtime)
{
    DriverRttEntry *rtt_table = calloc(
        UINT16_MAX + 1U,
        sizeof(*rtt_table)
    );

    if (rtt_table == NULL) {
        return DRIVER_STATUS_INITIALIZATION_FAILED;
    }

    driver_report_progress(
        runtime,
        DRIVER_PHASE_TRANSFERRING,
        0,
        runtime->expected_file_size
    );

    DriverStatus result = DRIVER_STATUS_OK;

    while (!runtime->source_eof ||
           arq_outstanding_count(runtime->arq) > 0) {

        if (driver_deadline_expired(runtime)) {
            result = DRIVER_STATUS_TRANSFER_TIMEOUT;
            break;
        }

        /*
         * Read and submit chunks while the negotiated ARQ window
         * has room. ARQ assigns the actual sequence number.
         */
        while (!runtime->source_eof &&
               arq_can_send(runtime->arq)) {

            FileChunk chunk = {0};

            int read_status = file_manager_read_chunk(
                &runtime->file_manager,
                &chunk
            );

            if (read_status < 0) {
                result = DRIVER_STATUS_FILE_ERROR;
                goto transfer_cleanup;
            }

            if (read_status == 0) {
                runtime->source_eof = true;
                break;
            }

            Packet outgoing = {0};

            size_t chunk_length = chunk.length;

            bool arq_ok = arq_send(
                runtime->arq,
                &chunk,
                &outgoing
            );

            free(chunk.data);

            if (!arq_ok) {
                result = DRIVER_STATUS_INTERNAL_ERROR;
                goto transfer_cleanup;
            }

            DriverRttEntry *entry = &rtt_table[outgoing.number];

            entry->used = true;
            entry->retransmitted = false;
            entry->sent_at_ms = driver_now_ms();

            bool sent = driver_send_packet(runtime, &outgoing);
            arq_packet_release(&outgoing);

            if (!sent) {
                result = DRIVER_STATUS_TRANSPORT_ERROR;
                goto transfer_cleanup;
            }

            runtime->result->bytes_read_from_source += chunk_length;

            driver_report_progress(
                runtime,
                DRIVER_PHASE_TRANSFERRING,
                runtime->result->bytes_read_from_source,
                runtime->expected_file_size
            );
        }

        Packet incoming = {0};

        int received = driver_receive_packet(
            runtime,
            &incoming,
            runtime->config->receive_timeout_ms
        );

        if (received < 0) {
            result = DRIVER_STATUS_TRANSPORT_ERROR;
            break;
        }

        if (received > 0) {
            uint8_t type = driver_packet_type(&incoming);

            if (!driver_protocol_matches(runtime, &incoming)) {
                packet_release_payload(&incoming);
                result = DRIVER_STATUS_PROTOCOL_ERROR;
                break;
            }

            if (type == FLAG_ACK) {
                PacketNumber previous_base =
                    (PacketNumber)(
                        arq_next_sequence_number(runtime->arq) -
                        arq_outstanding_count(runtime->arq)
                    );

                size_t previous_outstanding =
                    arq_outstanding_count(runtime->arq);

                bool accepted = arq_receive(
                    runtime->arq,
                    &incoming
                );

                if (accepted) {
                    driver_process_ack_rtt(
                        runtime,
                        &incoming,
                        rtt_table,
                        previous_base,
                        previous_outstanding
                    );
                }

                packet_release_payload(&incoming);
            } else if (type == FLAG_NACK) {
                /* ARQ owns the recovery policy for NACKs. */
                (void)arq_receive(runtime->arq, &incoming);
                packet_release_payload(&incoming);
            } else {
                packet_release_payload(&incoming);
            }
        }

        result = driver_send_retransmissions(runtime, rtt_table);

        if (result != DRIVER_STATUS_OK) {
            break;
        }
    }

transfer_cleanup:
    free(rtt_table);
    return result;
}


/* ------------------------------------------------------------------------- */
/* Receiver data transfer                                                     */
/* ------------------------------------------------------------------------- */

static DriverStatus driver_receiver_process_data(
    DriverRuntime *runtime,
    const Packet *incoming)
{
    Packet ack = {0};

    PacketNumber expected_before =
        arq_expected_receive_number(runtime->arq);

    bool accepted = arq_receive_data(
        runtime->arq,
        incoming,
        &ack
    );

    if (!accepted) {
        packet_release_payload(&ack);
        return DRIVER_STATUS_PROTOCOL_ERROR;
    }

    if (incoming->number != expected_before &&
        runtime->monitor != NULL) {
        uint16_t behind =
            (uint16_t)(expected_before - incoming->number);

        if (behind > 0 && behind < 32768U) {
            monitor_record_duplicate(
                runtime->monitor,
                incoming->number
            );
        } else {
            monitor_record_out_of_order(
                runtime->monitor,
                incoming->number
            );
        }
    }

    if (!driver_send_packet(runtime, &ack)) {
        packet_release_payload(&ack);
        return DRIVER_STATUS_TRANSPORT_ERROR;
    }

    packet_release_payload(&ack);

    for (;;) {
        Packet delivered = {0};

        if (!arq_get_delivered(runtime->arq, &delivered)) {
            break;
        }

        FileChunk chunk = {0};

        if (!packet_dewrap(&delivered, &chunk)) {
            arq_packet_release(&delivered);
            return DRIVER_STATUS_PROTOCOL_ERROR;
        }

        if (reconstructor_write_chunk(
                &runtime->reconstructor,
                &chunk) != 0) {
            free(chunk.data);
            arq_packet_release(&delivered);
            return DRIVER_STATUS_FILE_ERROR;
        }

        runtime->bytes_written += chunk.length;
        runtime->result->bytes_written_to_destination =
            runtime->bytes_written;

        if (runtime->monitor != NULL) {
            monitor_record_bytes_delivered(
                runtime->monitor,
                chunk.length
            );
        }

        free(chunk.data);
        arq_packet_release(&delivered);

        driver_report_progress(
            runtime,
            DRIVER_PHASE_TRANSFERRING,
            runtime->bytes_written,
            runtime->expected_file_size
        );
    }

    return DRIVER_STATUS_OK;
}

static bool driver_duplicate_syn_matches(
    const DriverRuntime *runtime,
    const Packet *packet)
{
    uint64_t file_size = 0;
    SHA256Digest digest = {0};

    if (packet == NULL ||
        driver_packet_type(packet) != (FLAG_SYN | FLAG_WINDOW) ||
        !driver_protocol_matches(runtime, packet) ||
        packet->number != runtime->proposed_window_size ||
        !driver_parse_metadata(packet, &file_size, &digest)) {
        return false;
    }

    return file_size == runtime->expected_file_size &&
           memcmp(
               digest.data,
               runtime->expected_digest.data,
               DRIVER_DIGEST_SIZE) == 0;
}

static DriverStatus driver_receiver_finish(
    DriverRuntime *runtime,
    PacketNumber fin_number)
{
    PacketNumber expected_final_number;

    if (runtime->expected_file_size == 0 && runtime->bytes_written == 0) {
        expected_final_number = UINT16_MAX;
    } else {
        expected_final_number = (PacketNumber)(
            arq_expected_receive_number(runtime->arq) - 1U
        );
    }

    if (fin_number != expected_final_number) {
        /* Tell the peer this FIN cannot complete the active transfer. */
        (void)driver_send_control(
            runtime,
            FLAG_FIN | FLAG_NACK,
            fin_number
        );
        return DRIVER_STATUS_PROTOCOL_ERROR;
    }

    driver_report_progress(
        runtime,
        DRIVER_PHASE_VERIFYING,
        runtime->bytes_written,
        runtime->expected_file_size
    );

    SHA256Digest actual_digest = {0};

    int finish_status = reconstructor_finish(
        &runtime->reconstructor,
        &runtime->expected_digest,
        &actual_digest
    );

    runtime->reconstructor_open = false;

    bool size_matches =
        runtime->bytes_written == runtime->expected_file_size;

    bool integrity_ok = finish_status == 0 && size_matches;

    runtime->result->integrity_verified = integrity_ok;

    uint8_t response_flags = integrity_ok
        ? (uint8_t)(FLAG_FIN | FLAG_ACK)
        : (uint8_t)(FLAG_FIN | FLAG_NACK);

    driver_report_progress(
        runtime,
        DRIVER_PHASE_FINISHING,
        runtime->bytes_written,
        runtime->expected_file_size
    );

    if (!driver_send_control(
            runtime,
            response_flags,
            fin_number)) {
        return DRIVER_STATUS_TRANSPORT_ERROR;
    }

    /*
     * Linger after the final response. If it was lost, the sender
     * will retransmit FIN and must receive the same response again.
     */
    uint64_t linger_ms =
        (uint64_t)runtime->config->control_timeout_ms *
        ((uint64_t)runtime->config->max_control_retries + 2U);

    uint64_t linger_start = driver_now_ms();
    bool overall_deadline_hit = false;

    while (driver_now_ms() - linger_start < linger_ms) {
        if (driver_deadline_expired(runtime)) {
            overall_deadline_hit = true;
            break;
        }

        Packet duplicate = {0};

        uint64_t now = driver_now_ms();
        uint64_t linger_elapsed = now - linger_start;
        uint64_t remaining_linger = linger_ms - linger_elapsed;
        uint32_t wait_ms = runtime->config->receive_timeout_ms;

        if ((uint64_t)wait_ms > remaining_linger) {
            wait_ms = (uint32_t)remaining_linger;
        }

        if (runtime->config->max_transfer_duration_ms != 0) {
            uint64_t transfer_elapsed = now - runtime->start_time_ms;
            uint64_t remaining_transfer =
                transfer_elapsed < runtime->config->max_transfer_duration_ms
                    ? runtime->config->max_transfer_duration_ms - transfer_elapsed
                    : 0;

            if ((uint64_t)wait_ms > remaining_transfer) {
                wait_ms = (uint32_t)remaining_transfer;
            }
        }

        if (wait_ms == 0) {
            overall_deadline_hit = driver_deadline_expired(runtime);
            break;
        }

        int received = driver_receive_packet(
            runtime,
            &duplicate,
            wait_ms
        );

        if (received < 0) {
            return DRIVER_STATUS_TRANSPORT_ERROR;
        }

        if (received == 0) {
            continue;
        }

        uint8_t type = driver_packet_type(&duplicate);

        if (driver_protocol_matches(runtime, &duplicate) &&
            type == FLAG_FIN &&
            duplicate.number == fin_number) {

            packet_release_payload(&duplicate);

            if (!driver_send_control(
                    runtime,
                    response_flags,
                    fin_number)) {
                return DRIVER_STATUS_TRANSPORT_ERROR;
            }

            continue;
        }

        packet_release_payload(&duplicate);
    }

    if (overall_deadline_hit || driver_deadline_expired(runtime)) {
        return DRIVER_STATUS_TRANSFER_TIMEOUT;
    }

    if (finish_status < 0) {
        return DRIVER_STATUS_FILE_ERROR;
    }

    if (!integrity_ok) {
        return DRIVER_STATUS_INTEGRITY_FAILED;
    }

    return DRIVER_STATUS_OK;
}

static DriverStatus driver_receiver_transfer(DriverRuntime *runtime)
{
    driver_report_progress(
        runtime,
        DRIVER_PHASE_TRANSFERRING,
        0,
        runtime->expected_file_size
    );

    for (;;) {
        if (driver_deadline_expired(runtime)) {
            return DRIVER_STATUS_TRANSFER_TIMEOUT;
        }

        Packet incoming = {0};

        int received = driver_receive_packet(
            runtime,
            &incoming,
            runtime->config->receive_timeout_ms
        );

        if (received < 0) {
            return DRIVER_STATUS_TRANSPORT_ERROR;
        }

        if (received == 0) {
            continue;
        }

        if (!driver_protocol_matches(runtime, &incoming)) {
            packet_release_payload(&incoming);
            return DRIVER_STATUS_PROTOCOL_ERROR;
        }

        uint8_t type = driver_packet_type(&incoming);

        if (type == FLAG_DATA) {
            DriverStatus status = driver_receiver_process_data(
                runtime,
                &incoming
            );

            packet_release_payload(&incoming);

            if (status != DRIVER_STATUS_OK) {
                return status;
            }

            continue;
        }

        if (type == (FLAG_SYN | FLAG_WINDOW)) {
            /* Only an exact retransmission of the active handshake may
             * receive the cached response. Never renegotiate mid-transfer. */
            bool matches = driver_duplicate_syn_matches(runtime, &incoming);
            packet_release_payload(&incoming);

            if (!matches) {
                return DRIVER_STATUS_PROTOCOL_ERROR;
            }

            if (!driver_send_control(
                    runtime,
                    FLAG_SYN | FLAG_ACK | FLAG_WINDOW,
                    runtime->agreed_window_size)) {
                return DRIVER_STATUS_TRANSPORT_ERROR;
            }

            continue;
        }

        if (type == FLAG_FIN) {
            PacketNumber fin_number = incoming.number;
            packet_release_payload(&incoming);

            return driver_receiver_finish(runtime, fin_number);
        }

        packet_release_payload(&incoming);
    }
}


/* ------------------------------------------------------------------------- */
/* Cleanup and public entry point                                             */
/* ------------------------------------------------------------------------- */

static void driver_cleanup(DriverRuntime *runtime)
{
    if (runtime->file_manager_open) {
        file_manager_close(&runtime->file_manager);
        runtime->file_manager_open = false;
    }

    if (runtime->reconstructor_open) {
        reconstructor_abort(&runtime->reconstructor);
        runtime->reconstructor_open = false;
    }

    if (runtime->transport != NULL) {
        transport_close(runtime->transport);
        runtime->transport = NULL;
    }

    if (runtime->arq != NULL) {
        arq_destroy(runtime->arq);
        runtime->arq = NULL;
    }

    if (runtime->monitor != NULL) {
        if (runtime->monitor_started &&
            !monitor_finish(runtime->monitor)) {
            runtime->result->monitor_output_failed = true;
        }

        if (runtime->config->event_log_path != NULL &&
            !monitor_close_event_log(runtime->monitor)) {
            runtime->result->monitor_output_failed = true;
        }

        if (runtime->config->summary_path != NULL &&
            !monitor_write_summary(
                runtime->monitor,
                runtime->config->summary_path)) {
            runtime->result->monitor_output_failed = true;
        }

        if (monitor_get_stats(
                runtime->monitor,
                &runtime->result->metrics)) {
            runtime->result->monitor_stats_available = true;
        }

        monitor_destroy(runtime->monitor);
        runtime->monitor = NULL;
    }
}

DriverStatus driver_run(
    const DriverConfig *config,
    DriverResult *result)
{
    if (result == NULL) {
        return DRIVER_STATUS_INVALID_CONFIG;
    }

    memset(result, 0, sizeof(*result));

    if (!driver_valid_config(config)) {
        return DRIVER_STATUS_INVALID_CONFIG;
    }

    result->mode = config->mode;
    result->role = config->role;

    DriverRuntime runtime = {0};

    runtime.config = config;
    runtime.result = result;
    runtime.start_time_ms = driver_now_ms();

    driver_report_progress(
        &runtime,
        DRIVER_PHASE_INITIALIZING,
        0,
        0
    );

    /*
     * Start monitoring before the transfer, so initialization,
     * handshake, control packets, and data packets can be recorded.
     */
    if (config->enable_monitoring) {
        runtime.monitor = monitor_create();

        if (runtime.monitor == NULL ||
            !monitor_start(runtime.monitor)) {
            if (runtime.monitor != NULL) {
                monitor_destroy(runtime.monitor);
                runtime.monitor = NULL;
            }

            return DRIVER_STATUS_MONITOR_ERROR;
        }

        runtime.monitor_started = true;

        if (config->event_log_path != NULL &&
            !monitor_open_event_log(
                runtime.monitor,
                config->event_log_path)) {
            result->monitor_output_failed = true;
        }
    }

    DriverStatus status = DRIVER_STATUS_OK;

    if (config->role == TRANSPORT_SENDER) {
        if (file_manager_open(
                &runtime.file_manager,
                config->source_path,
                config->chunk_size) != 0) {
            status = DRIVER_STATUS_FILE_ERROR;
            goto cleanup;
        }

        runtime.file_manager_open = true;

        runtime.expected_file_size =
            file_manager_get_size(&runtime.file_manager);

        result->file_size_known = true;
        result->file_size_bytes = runtime.expected_file_size;
    }

    if (!driver_create_transport(&runtime)) {
        status = DRIVER_STATUS_INITIALIZATION_FAILED;
        goto cleanup;
    }

    if (config->role == TRANSPORT_SENDER) {
        status = driver_sender_handshake(&runtime);

        if (status != DRIVER_STATUS_OK) {
            goto cleanup;
        }

        status = driver_sender_transfer(&runtime);

        if (status != DRIVER_STATUS_OK) {
            goto cleanup;
        }

        driver_report_progress(
            &runtime,
            DRIVER_PHASE_FINISHING,
            result->bytes_read_from_source,
            runtime.expected_file_size
        );

        PacketNumber final_number =
            (PacketNumber)(
                arq_next_sequence_number(runtime.arq) - 1U
            );

        bool completion_confirmed = false;

        for (uint32_t attempt = 0;
             attempt <= config->max_control_retries;
             ++attempt) {

            if (driver_deadline_expired(&runtime)) {
                status = DRIVER_STATUS_TRANSFER_TIMEOUT;
                goto cleanup;
            }

            if (!driver_send_control(
                    &runtime,
                    FLAG_FIN,
                    final_number)) {
                status = DRIVER_STATUS_TRANSPORT_ERROR;
                goto cleanup;
            }

            uint64_t wait_start = driver_now_ms();

            while (driver_now_ms() - wait_start <
                   config->control_timeout_ms) {

                if (driver_deadline_expired(&runtime)) {
                    status = DRIVER_STATUS_TRANSFER_TIMEOUT;
                    goto cleanup;
                }

                uint64_t elapsed = driver_now_ms() - wait_start;
                uint64_t remaining =
                    config->control_timeout_ms - elapsed;

                uint32_t wait_ms = config->receive_timeout_ms;

                if ((uint64_t)wait_ms > remaining) {
                    wait_ms = (uint32_t)remaining;
                }

                if (wait_ms == 0) {
                    break;
                }

                Packet response = {0};

                int received = driver_receive_packet(
                    &runtime,
                    &response,
                    wait_ms
                );

                if (received < 0) {
                    status = DRIVER_STATUS_TRANSPORT_ERROR;
                    goto cleanup;
                }

                if (received == 0) {
                    continue;
                }

                uint8_t type = driver_packet_type(&response);

                bool valid_response =
                    driver_protocol_matches(&runtime, &response) &&
                    response.number == final_number;

                packet_release_payload(&response);

                if (!valid_response) {
                    continue;
                }

                if (type == (FLAG_FIN | FLAG_ACK)) {
                    completion_confirmed = true;
                    break;
                }

                if (type == (FLAG_FIN | FLAG_NACK)) {
                    status = DRIVER_STATUS_REMOTE_REJECTED;
                    goto cleanup;
                }
            }

            if (completion_confirmed) {
                break;
            }
        }

        if (!completion_confirmed) {
            status = DRIVER_STATUS_TRANSFER_TIMEOUT;
            goto cleanup;
        }

        result->integrity_verified = true;
        status = DRIVER_STATUS_OK;
    } else {
        status = driver_receiver_handshake(&runtime);

        if (status != DRIVER_STATUS_OK) {
            goto cleanup;
        }

        status = driver_receiver_transfer(&runtime);
    }

cleanup:
    if (status == DRIVER_STATUS_OK) {
        driver_report_progress(
            &runtime,
            DRIVER_PHASE_COMPLETED,
            config->role == TRANSPORT_SENDER
                ? result->bytes_read_from_source
                : result->bytes_written_to_destination,
            result->file_size_known
                ? result->file_size_bytes
                : 0
        );
    } else {
        driver_report_progress(
            &runtime,
            DRIVER_PHASE_FAILED,
            config->role == TRANSPORT_SENDER
                ? result->bytes_read_from_source
                : result->bytes_written_to_destination,
            result->file_size_known
                ? result->file_size_bytes
                : 0
        );
    }

    driver_cleanup(&runtime);
    return status;
}


/* ------------------------------------------------------------------------- */
/* Status descriptions                                                        */
/* ------------------------------------------------------------------------- */

const char *driver_status_string(DriverStatus status)
{
    switch (status) {
        case DRIVER_STATUS_OK:
            return "Success";

        case DRIVER_STATUS_INVALID_CONFIG:
            return "Invalid configuration";

        case DRIVER_STATUS_INITIALIZATION_FAILED:
            return "Module initialization failed";

        case DRIVER_STATUS_MONITOR_ERROR:
            return "Monitoring initialization failed";

        case DRIVER_STATUS_HANDSHAKE_TIMEOUT:
            return "Handshake timed out";

        case DRIVER_STATUS_HANDSHAKE_REJECTED:
            return "Handshake rejected by peer";

        case DRIVER_STATUS_TRANSFER_TIMEOUT:
            return "Transfer timed out";

        case DRIVER_STATUS_TRANSPORT_ERROR:
            return "Transport error";

        case DRIVER_STATUS_FILE_ERROR:
            return "File operation failed";

        case DRIVER_STATUS_PROTOCOL_ERROR:
            return "Protocol error";

        case DRIVER_STATUS_INTEGRITY_FAILED:
            return "File integrity verification failed";

        case DRIVER_STATUS_REMOTE_REJECTED:
            return "Remote peer rejected completion";

        case DRIVER_STATUS_INTERNAL_ERROR:
            return "Internal error";

        default:
            return "Unknown driver status";
    }
}