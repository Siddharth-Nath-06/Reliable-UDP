
#ifndef ARQ_H
#define ARQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../common/types.h"

#define ARQ_MAX_WINDOW_SIZE 32

typedef struct ArqContext ArqContext;

ArqContext *arq_create(
    ArqProtocol protocol,
    uint16_t window_size,
    uint64_t timeout_ms
);

void arq_destroy(ArqContext *arq);

bool arq_send(ArqContext *arq, const Packet *packet, Packet *outgoing);

bool arq_receive(ArqContext *arq, const Packet *packet);

bool arq_get_retransmission(ArqContext *arq, Packet *outgoing);

bool arq_should_retransmit(ArqContext *arq);

PacketNumber arq_next_sequence_number(const ArqContext *arq);

size_t arq_outstanding_count(const ArqContext *arq);

bool arq_receive_data(
    ArqContext *arq,
    const Packet *incoming,
    Packet *ack
);

bool arq_get_delivered(ArqContext *arq, Packet *delivered);

void arq_packet_release(Packet *packet);

PacketNumber arq_expected_receive_number(const ArqContext *arq);

#endif
