
#include "arq.h"

#include <stdlib.h>
#include <string.h>

#include "../common/constants.h"
#include "../timer/timer.h"

#include "../rto/rto.h"

#define ARQ_DELIVERY_QUEUE_SIZE 128

typedef struct {
    bool used;
    bool retransmit_pending;
    Packet packet;
    TimerId timer_id;
} ArqSlot;

typedef struct {
    bool used;
    Packet packet;
} ArqReceiveSlot;

struct ArqContext {
    ArqProtocol protocol;
    uint16_t window_size;
    PacketNumber next_sequence_number;
    PacketNumber base_sequence_number;
    size_t outstanding;
    ArqSlot slots[ARQ_MAX_WINDOW_SIZE];
    PacketNumber expected_receive_number;
    ArqReceiveSlot receive_slots[ARQ_MAX_WINDOW_SIZE];
    Packet delivery_queue[ARQ_DELIVERY_QUEUE_SIZE];
    size_t delivery_head;
    size_t delivery_count;
    RtoEstimator *rto;
};

static uint8_t protocol_flags(ArqProtocol protocol)
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

static ArqSlot *find_slot(ArqContext *arq, PacketNumber number)
{
    ArqSlot *slot = &arq->slots[number % ARQ_MAX_WINDOW_SIZE];

    if (slot->used && slot->packet.number == number) {
        return slot;
    }

    return NULL;
}

static void release_slot(ArqSlot *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }

    timer_cancel(slot->timer_id);
    timer_destroy(slot->timer_id);
    free(slot->packet.payload);
    memset(slot, 0, sizeof(*slot));
}

static void discard_receive_slot(ArqReceiveSlot *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }

    free(slot->packet.payload);
    memset(slot, 0, sizeof(*slot));
}

static bool has_pending_retransmission(const ArqContext *arq)
{
    for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
        if (arq->slots[i].used &&
            arq->slots[i].retransmit_pending) {
            return true;
        }
    }

    return false;
}

static void advance_base(ArqContext *arq)
{
    while (arq->base_sequence_number != arq->next_sequence_number &&
           find_slot(arq, arq->base_sequence_number) == NULL) {
        arq->base_sequence_number++;
    }
}

static bool queue_received_packet(ArqContext *arq, Packet *packet)
{
    if (arq->delivery_count >= ARQ_DELIVERY_QUEUE_SIZE) {
        return false;
    }

    size_t index =
        (arq->delivery_head + arq->delivery_count) %
        ARQ_DELIVERY_QUEUE_SIZE;

    arq->delivery_queue[index] = *packet;
    arq->delivery_count++;

    packet->payload = NULL;
    packet->payload_length = 0;

    return true;
}

static bool copy_received_packet(
    const Packet *source,
    Packet *destination)
{
    *destination = *source;
    destination->payload = NULL;

    if (source->payload_length == 0) {
        return true;
    }

    if (source->payload == NULL) {
        return false;
    }

    destination->payload = malloc(source->payload_length);

    if (destination->payload == NULL) {
        return false;
    }

    memcpy(
        destination->payload,
        source->payload,
        source->payload_length
    );

    return true;
}

static bool deliver_in_order(ArqContext *arq)
{
    for (;;) {
        ArqReceiveSlot *slot =
            &arq->receive_slots[
                arq->expected_receive_number % ARQ_MAX_WINDOW_SIZE
            ];

        if (!slot->used ||
            slot->packet.number != arq->expected_receive_number) {
            return true;
        }

        if (arq->delivery_count >= ARQ_DELIVERY_QUEUE_SIZE) {
            return false;
        }

        Packet packet = slot->packet;
        memset(slot, 0, sizeof(*slot));

        if (!queue_received_packet(arq, &packet)) {
            slot->used = true;
            slot->packet = packet;
            return false;
        }

        arq->expected_receive_number++;
    }
}

ArqContext *arq_create(
    ArqProtocol protocol,
    uint16_t window_size,
    uint64_t initial_rto_ms,
    uint64_t min_rto_ms,
    uint64_t max_rto_ms)
{
    if (protocol != ARQ_STOP_WAIT &&
        protocol != ARQ_GO_BACK_N &&
        protocol != ARQ_SELECTIVE_REPEAT) {
        return NULL;
    }

    if (window_size == 0 ||
        window_size > ARQ_MAX_WINDOW_SIZE) {
        return NULL;
    }

    if (protocol == ARQ_STOP_WAIT && window_size != 1) {
        return NULL;
    }

    ArqContext *arq = calloc(1, sizeof(*arq));

    if (arq == NULL) {
        return NULL;
    }

    arq->rto = rto_create(
        initial_rto_ms,
        min_rto_ms,
        max_rto_ms
    );

    if (arq->rto == NULL) {
        free(arq);
        return NULL;
    }

    arq->protocol = protocol;
    arq->window_size = window_size;

    return arq;
}

void arq_packet_release(Packet *packet)
{
    if (packet == NULL) {
        return;
    }

    free(packet->payload);
    packet->payload = NULL;
    packet->payload_length = 0;
}

void arq_destroy(ArqContext *arq)
{
    if (arq == NULL) {
        return;
    }

    for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
        release_slot(&arq->slots[i]);
        discard_receive_slot(&arq->receive_slots[i]);
    }

    for (size_t i = 0; i < ARQ_DELIVERY_QUEUE_SIZE; i++) {
        arq_packet_release(&arq->delivery_queue[i]);
    }

    rto_destroy(arq->rto);
    free(arq);
}


bool arq_send(
    ArqContext *arq,
    const Packet *packet,
    Packet *outgoing)
{
    if (arq == NULL || packet == NULL || outgoing == NULL) {
        return false;
    }

    if ((packet->flags & FLAG_DATA) == 0 ||
        packet->payload_length > MAX_PAYLOAD_SIZE ||
        (packet->payload_length > 0 && packet->payload == NULL)) {
        return false;
    }

    uint16_t sequence_span =
        (uint16_t)(arq->next_sequence_number -
                   arq->base_sequence_number);

    if (sequence_span >= arq->window_size) {
        return false;
    }

    PacketNumber number = arq->next_sequence_number;
    ArqSlot *slot = &arq->slots[number % ARQ_MAX_WINDOW_SIZE];

    if (slot->used) {
        return false;
    }

    TimerId timer_id = timer_create();

    if (timer_id == 0) {
        return false;
    }

    Packet stored = *packet;
    stored.number = number;
    stored.flags =
        (uint8_t)((stored.flags & ~FLAG_PROTOCOL_MASK) |
                  protocol_flags(arq->protocol));
    stored.payload = NULL;

    if (packet->payload_length > 0) {
        stored.payload = malloc(packet->payload_length);

        if (stored.payload == NULL) {
            timer_destroy(timer_id);
            return false;
        }

        memcpy(
            stored.payload,
            packet->payload,
            packet->payload_length
        );
    }

    slot->used = true;
    slot->retransmit_pending = false;
    slot->packet = stored;
    slot->timer_id = timer_id;

    bool start_timer =
        arq->protocol == ARQ_SELECTIVE_REPEAT ||
        arq->outstanding == 0;

    if (start_timer && !timer_start(timer_id, rto_get(arq->rto))) {
        release_slot(slot);
        return false;
    }

    if (!copy_received_packet(&slot->packet, outgoing)) {
        release_slot(slot);
        return false;
    }

    arq->next_sequence_number++;
    arq->outstanding++;

    return true;
}


bool arq_receive(ArqContext *arq, const Packet *packet)
{
    if (arq == NULL || packet == NULL ||
        (packet->flags & FLAG_ACK) == 0) {
        return false;
    }

    PacketNumber acknowledged = packet->number;

    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        ArqSlot *slot = find_slot(arq, acknowledged);

        if (slot == NULL) {
            return false;
        }

        release_slot(slot);
        arq->outstanding--;
        advance_base(arq);

        return true;
    }

    uint16_t distance =
        (uint16_t)(acknowledged - arq->base_sequence_number);

    if (arq->outstanding == 0 || distance >= arq->outstanding) {
        return false;
    }

    size_t acknowledged_count = (size_t)distance + 1;

    for (size_t i = 0; i < acknowledged_count; i++) {
        PacketNumber number =
            (PacketNumber)(arq->base_sequence_number + i);

        ArqSlot *slot = find_slot(arq, number);

        if (slot != NULL) {
            release_slot(slot);
            arq->outstanding--;
        }
    }

    arq->base_sequence_number =
        (PacketNumber)(acknowledged + 1);

    if (arq->outstanding > 0) {
        ArqSlot *oldest = find_slot(
            arq,
            arq->base_sequence_number
        );

        if (oldest != NULL) {
            timer_restart(oldest->timer_id, rto_get(arq->rto));
        }
    } else {
        arq->base_sequence_number = arq->next_sequence_number;
    }

    return true;
}

bool arq_should_retransmit(ArqContext *arq)
{
    if (arq == NULL || arq->outstanding == 0 ||
        has_pending_retransmission(arq)) {
        return false;
    }

    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        bool expired = false;

        for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
            ArqSlot *slot = &arq->slots[i];

            if (slot->used && timer_expired(slot->timer_id)) {
                slot->retransmit_pending = true;
                timer_restart(slot->timer_id, rto_get(arq->rto));
                expired = true;
            }
        }

        return expired;
    }

    ArqSlot *oldest = find_slot(
        arq,
        arq->base_sequence_number
    );

    if (oldest == NULL || !timer_expired(oldest->timer_id)) {
        return false;
    }

    if (arq->protocol == ARQ_GO_BACK_N) {
        for (uint16_t i = 0; i < arq->outstanding; i++) {
            PacketNumber number =
                (PacketNumber)(arq->base_sequence_number + i);

            ArqSlot *slot = find_slot(arq, number);

            if (slot != NULL) {
                slot->retransmit_pending = true;
            }
        }
    } else {
        oldest->retransmit_pending = true;
    }

    timer_restart(oldest->timer_id, rto_get(arq->rto));

    return true;
}


bool arq_get_retransmission(
    ArqContext *arq,
    Packet *outgoing)
{
    if (arq == NULL || outgoing == NULL) {
        return false;
    }

    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
            ArqSlot *slot = &arq->slots[i];

            if (slot->used && slot->retransmit_pending) {
                if (!copy_received_packet(&slot->packet, outgoing)) {
                    return false;
                }

                slot->retransmit_pending = false;
                return true;
            }
        }

        return false;
    }

    for (uint16_t i = 0; i < arq->outstanding; i++) {
        PacketNumber number =
            (PacketNumber)(arq->base_sequence_number + i);

        ArqSlot *slot = find_slot(arq, number);

        if (slot != NULL && slot->retransmit_pending) {
            if (!copy_received_packet(&slot->packet, outgoing)) {
                return false;
            }

            slot->retransmit_pending = false;
            return true;
        }
    }

    return false;
}


PacketNumber arq_next_sequence_number(const ArqContext *arq)
{
    if (arq == NULL) {
        return 0;
    }

    return arq->next_sequence_number;
}

size_t arq_outstanding_count(const ArqContext *arq)
{
    if (arq == NULL) {
        return 0;
    }

    return arq->outstanding;
}

bool arq_receive_data(
    ArqContext *arq,
    const Packet *incoming,
    Packet *ack)
{
    if (arq == NULL || incoming == NULL || ack == NULL) {
        return false;
    }

    if ((incoming->flags & FLAG_DATA) == 0 ||
        incoming->payload_length > MAX_PAYLOAD_SIZE ||
        (incoming->payload_length > 0 && incoming->payload == NULL)) {
        return false;
    }

    memset(ack, 0, sizeof(*ack));
    ack->flags =
        (uint8_t)(FLAG_ACK | protocol_flags(arq->protocol));

    PacketNumber number = incoming->number;
    PacketNumber expected = arq->expected_receive_number;

    if (arq->protocol == ARQ_STOP_WAIT ||
        arq->protocol == ARQ_GO_BACK_N) {
        if (number == expected) {
            if (arq->delivery_count >= ARQ_DELIVERY_QUEUE_SIZE) {
                return false;
            }

            Packet copy = {0};

            if (!copy_received_packet(incoming, &copy)) {
                return false;
            }

            if (!queue_received_packet(arq, &copy)) {
                arq_packet_release(&copy);
                return false;
            }

            arq->expected_receive_number++;
        }

        ack->number = (PacketNumber)(arq->expected_receive_number - 1);
        return true;
    }

    uint16_t distance = (uint16_t)(number - expected);

    if (distance < arq->window_size) {
        ArqReceiveSlot *slot =
            &arq->receive_slots[number % ARQ_MAX_WINDOW_SIZE];

        if (!slot->used) {
            Packet copy = {0};

            if (!copy_received_packet(incoming, &copy)) {
                return false;
            }

            slot->packet = copy;
            slot->used = true;
        } else if (slot->packet.number != number) {
            return false;
        }

        ack->number = number;
        deliver_in_order(arq);

        return true;
    }

    uint16_t behind = (uint16_t)(expected - number);

    if (behind <= arq->window_size) {
        ack->number = number;
        return true;
    }

    return false;
}

bool arq_get_delivered(ArqContext *arq, Packet *delivered)
{
    if (arq == NULL || delivered == NULL ||
        arq->delivery_count == 0) {
        return false;
    }

    *delivered = arq->delivery_queue[arq->delivery_head];

    memset(
        &arq->delivery_queue[arq->delivery_head],
        0,
        sizeof(Packet)
    );

    arq->delivery_head =
        (arq->delivery_head + 1) % ARQ_DELIVERY_QUEUE_SIZE;

    arq->delivery_count--;

    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        deliver_in_order(arq);
    }

    return true;
}

PacketNumber arq_expected_receive_number(const ArqContext *arq)
{
    if (arq == NULL) {
        return 0;
    }

    return arq->expected_receive_number;
}

void arq_record_rtt(
    ArqContext *arq,
    uint64_t rtt_ms,
    bool retransmitted)
{
    if (arq == NULL) {
        return;
    }

    rto_update(
        arq->rto,
        rtt_ms,
        retransmitted
    );
}

uint64_t arq_get_rto(
    const ArqContext *arq)
{
    if (arq == NULL) {
        return 0;
    }

    return rto_get(arq->rto);
}