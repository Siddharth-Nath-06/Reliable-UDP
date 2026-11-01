#ifndef ARQ_H
#define ARQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../common/types.h"

#define ARQ_MAX_WINDOW_SIZE 32

typedef struct ArqContext ArqContext;

/*
 * Create an ARQ context for the selected protocol.
 *
 * Stop-and-Wait requires window_size == 1.
 * Returns NULL for invalid parameters or allocation failure.
 */
ArqContext *arq_create(
    ArqProtocol protocol,
    uint16_t window_size,
    uint64_t initial_rto_ms,
    uint64_t min_rto_ms,
    uint64_t max_rto_ms
);

void arq_destroy(ArqContext *arq);

/*
 * Check whether another DATA chunk fits in the sender window.
 *
 * The Driver should check this before reading another file chunk,
 * so it does not consume a chunk when the window is full.
 */
bool arq_can_send(const ArqContext *arq);

/*
 * Assign a sequence number to a raw FileChunk, create its logical
 * DATA packet using Packet Creator, retain a copy for retransmission,
 * and return a separate caller-owned Packet.
 *
 * The chunk is borrowed: ARQ does not modify or free it.
 *
 * outgoing must be zero-initialized or previously released.
 * If this function fails, outgoing remains unchanged.
 */
bool arq_send(
    ArqContext *arq,
    const FileChunk *chunk,
    Packet *outgoing
);

/*
 * Process a verified ACK or NACK packet.
 *
 * Stop-and-Wait and Go-Back-N use cumulative ACKs.
 * Selective Repeat acknowledges packets individually.
 *
 * A NACK schedules the identified packet for retransmission;
 * Go-Back-N also schedules subsequent outstanding packets.
 */
bool arq_receive(
    ArqContext *arq,
    const Packet *packet
);

/*
 * Retrieve one caller-owned packet copy whose retransmission
 * has been scheduled.
 *
 * Repeat until this returns false to drain all pending packets.
 * outgoing must be zero-initialized or previously released.
 */
bool arq_get_retransmission(
    ArqContext *arq,
    Packet *outgoing
);

/*
 * Detect expired timers and schedule the relevant retransmissions.
 *
 * Returns true when one or more retransmissions are pending,
 * including retransmissions previously scheduled by a NACK.
 *
 * Does not perform network I/O. The caller should drain pending
 * packets using arq_get_retransmission().
 */
bool arq_should_retransmit(ArqContext *arq);

PacketNumber arq_next_sequence_number(const ArqContext *arq);

size_t arq_outstanding_count(const ArqContext *arq);

/*
 * Process a verified incoming DATA packet and generate its ACK.
 *
 * The incoming packet's protocol bits must match this ARQ context.
 * ack must be zero-initialized or previously released.
 *
 * Accepted packets are retained and exposed through arq_get_delivered()
 * in the order required by the selected protocol.
 */
bool arq_receive_data(
    ArqContext *arq,
    const Packet *incoming,
    Packet *ack
);

/*
 * Transfer the next in-order accepted Packet to the caller.
 *
 * The caller owns the returned payload and should release it after
 * dewrapping/reconstruction. delivered must be zero-initialized
 * or previously released.
 */
bool arq_get_delivered(
    ArqContext *arq,
    Packet *delivered
);

/*
 * Release a caller-owned Packet payload and zero the whole Packet.
 */
void arq_packet_release(Packet *packet);

PacketNumber arq_expected_receive_number(const ArqContext *arq);

/*
 * Record an RTT sample.
 * The RTO module applies Karn's algorithm to retransmitted samples.
 */
void arq_record_rtt(
    ArqContext *arq,
    uint64_t rtt_ms,
    bool retransmitted
);

uint64_t arq_get_rto(const ArqContext *arq);

#endif