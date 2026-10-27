#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Transport Transport;

/*
 * Forward declaration of the channel configuration type.
 * Transport's public interface does not depend on the
 * emulator's implementation.
 */
struct ChannelEmulatorConfig;

/*
 * Role of this Transport instance in the current transfer.
 */
typedef enum {
    TRANSPORT_SENDER,
    TRANSPORT_RECEIVER
} TransportRole;

/*
 * Configuration for a Transport instance.
 *
 * The sender Transport owns the Channel Emulator.
 * The receiver Transport communicates directly with the
 * sender's UDP endpoint.
 */
typedef struct {
    TransportRole role;

    const char *sender_address;
    uint16_t sender_port;

    const char *receiver_address;
    uint16_t receiver_port;

    /*
     * Required for TRANSPORT_SENDER.
     * Unused for TRANSPORT_RECEIVER.
     *
     * transport_init() passes this configuration to the
     * Channel Emulator during initialization.
     */
    const struct ChannelEmulatorConfig *channel_config;

} TransportConfig;

/*
 * Result of attempting to receive a datagram.
 */
typedef enum {
    TRANSPORT_RECEIVE_OK,
    TRANSPORT_RECEIVE_TIMEOUT,
    TRANSPORT_RECEIVE_ERROR
} TransportReceiveStatus;

/*
 * Initialize Transport.
 *
 * Binds the UDP socket to this instance's local endpoint.
 * For a sender, also initializes the Channel Emulator.
 *
 * Returns NULL if initialization fails.
 */
Transport *transport_init(const TransportConfig *config);

/*
 * Send a datagram through the transport pipeline.
 *
 * Sender:
 *   Submits the datagram to the Channel Emulator for
 *   asynchronous processing and eventual UDP transmission.
 *
 * Receiver:
 *   Sends the datagram directly to the sender's UDP endpoint.
 *
 * Returns true if the datagram is accepted by the local
 * sending operation. This does not guarantee network delivery.
 */
bool transport_send(
    Transport *transport,
    const uint8_t *data,
    size_t length
);

/*
 * Receive a datagram.
 *
 * Sender:
 *   Returns an inbound datagram after it has passed through
 *   the Channel Emulator.
 *
 * Receiver:
 *   Returns a datagram received from the sender's UDP endpoint.
 *
 * Waits up to timeout_ms for a datagram.
 *
 * On TRANSPORT_RECEIVE_OK, received_length contains the
 * number of bytes received.
 *
 * On timeout or error, received_length is set to zero.
 */
TransportReceiveStatus transport_receive(
    Transport *transport,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *received_length,
    uint32_t timeout_ms
);

/*
 * Close the UDP socket, stop the Channel Emulator if owned,
 * and release all Transport resources.
 */
void transport_close(Transport *transport);

#endif