#include "arq.h"

#include <stdlib.h>
#include <string.h>

#include "../common/constants.h"
#include "../packet/packet.h"
#include "../timer/timer.h"
#include "../rto/rto.h"

#define ARQ_DELIVERY_QUEUE_SIZE 128

typedef struct {
    bool used;
    bool retransmit_pending;
    Packet packet;
    TimerId timer_id;
} ArqSendSlot;

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

    ArqSendSlot send_slots[ARQ_MAX_WINDOW_SIZE];

    PacketNumber expected_receive_number;
    ArqReceiveSlot receive_slots[ARQ_MAX_WINDOW_SIZE];

    Packet delivery_queue[ARQ_DELIVERY_QUEUE_SIZE];
    size_t delivery_head;
    size_t delivery_count;

    RtoEstimator *rto;
};

/* ---------- Protocol and packet validation ---------- */

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

static uint8_t packet_type_flags(const Packet *packet)
{
    return (uint8_t)(
        packet->flags & (uint8_t)~FLAG_PROTOCOL_MASK
    );
}

static bool packet_protocol_matches(
    const ArqContext *arq,
    const Packet *packet)
{
    return
        (packet->flags & FLAG_PROTOCOL_MASK) ==
        protocol_flags(arq->protocol);
}

static bool valid_data_packet(
    const ArqContext *arq,
    const Packet *packet)
{
    return
        packet != NULL &&
        packet_type_flags(packet) == FLAG_DATA &&
        packet_protocol_matches(arq, packet) &&
        packet->payload_length <= MAX_PAYLOAD_SIZE &&
        (packet->payload_length == 0 || packet->payload != NULL);
}

static bool valid_control_packet(
    const ArqContext *arq,
    const Packet *packet)
{
    if (packet == NULL ||
        !packet_protocol_matches(arq, packet)) {
        return false;
    }

    uint8_t type = packet_type_flags(packet);

    return
        (type == FLAG_ACK || type == FLAG_NACK) &&
        packet->payload_length == 0 &&
        packet->payload == NULL;
}

/*
 * Deep-copy a Packet.
 *
 * Destination is assigned only after allocation succeeds.
 * Destination must be empty; this helper does not release any
 * previous destination payload.
 */
static bool copy_packet(
    const Packet *source,
    Packet *destination)
{
    if (source == NULL || destination == NULL ||
        source->payload_length > MAX_PAYLOAD_SIZE ||
        (source->payload_length > 0 &&
         source->payload == NULL)) {
        return false;
    }

    Packet copy = *source;
    copy.payload = NULL;

    if (source->payload_length > 0) {
        copy.payload = malloc(source->payload_length);

        if (copy.payload == NULL) {
            return false;
        }

        memcpy(
            copy.payload,
            source->payload,
            source->payload_length
        );
    }

    *destination = copy;
    return true;
}

void arq_packet_release(Packet *packet)
{
    if (packet == NULL) {
        return;
    }

    packet_release_payload(packet);
    memset(packet, 0, sizeof(*packet));
}

/* ---------- Sender and receiver slots ---------- */

static ArqSendSlot *find_send_slot(
    ArqContext *arq,
    PacketNumber number)
{
    ArqSendSlot *slot =
        &arq->send_slots[number % ARQ_MAX_WINDOW_SIZE];

    if (slot->used && slot->packet.number == number) {
        return slot;
    }

    return NULL;
}

static const ArqSendSlot *find_send_slot_const(
    const ArqContext *arq,
    PacketNumber number)
{
    const ArqSendSlot *slot =
        &arq->send_slots[number % ARQ_MAX_WINDOW_SIZE];

    if (slot->used && slot->packet.number == number) {
        return slot;
    }

    return NULL;
}

static void release_send_slot(ArqSendSlot *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }

    if (slot->timer_id != 0) {
        /*
         * Cancellation can fail if the timer is already stopped.
         * Destroy is still attempted to release its slot.
         */
        (void)timer_cancel(slot->timer_id);
        (void)timer_destroy(slot->timer_id);
    }

    arq_packet_release(&slot->packet);
    memset(slot, 0, sizeof(*slot));
}

static void release_receive_slot(ArqReceiveSlot *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }

    arq_packet_release(&slot->packet);
    memset(slot, 0, sizeof(*slot));
}

/* ---------- Delivery queue ---------- */

/*
 * Move packet ownership to the delivery queue.
 * On success, packet is reset to zero.
 */
static bool enqueue_delivered_packet(
    ArqContext *arq,
    Packet *packet)
{
    if (arq->delivery_count >= ARQ_DELIVERY_QUEUE_SIZE) {
        return false;
    }

    size_t tail =
        (arq->delivery_head + arq->delivery_count) %
        ARQ_DELIVERY_QUEUE_SIZE;

    arq->delivery_queue[tail] = *packet;
    arq->delivery_count++;

    memset(packet, 0, sizeof(*packet));

    return true;
}

/*
 * Move consecutive Selective Repeat receive slots into the
 * delivery queue. Any packets that cannot be moved because the
 * queue is full remain safely retained in their receive slots.
 */
static bool deliver_in_order(ArqContext *arq)
{
    for (;;) {
        ArqReceiveSlot *slot =
            &arq->receive_slots[
                arq->expected_receive_number %
                ARQ_MAX_WINDOW_SIZE
            ];

        if (!slot->used ||
            slot->packet.number != arq->expected_receive_number) {
            return true;
        }

        if (arq->delivery_count >= ARQ_DELIVERY_QUEUE_SIZE) {
            return false;
        }

        if (!enqueue_delivered_packet(arq, &slot->packet)) {
            return false;
        }

        memset(slot, 0, sizeof(*slot));
        arq->expected_receive_number++;
    }
}

/* ---------- Sender-window helpers ---------- */

static void advance_send_base(ArqContext *arq)
{
    while (arq->base_sequence_number !=
               arq->next_sequence_number &&
           find_send_slot(
               arq,
               arq->base_sequence_number
           ) == NULL) {
        arq->base_sequence_number++;
    }
}

static bool has_pending_retransmission(const ArqContext *arq)
{
    for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
        if (arq->send_slots[i].used &&
            arq->send_slots[i].retransmit_pending) {
            return true;
        }
    }

    return false;
}

/*
 * Schedule a NACK-triggered retransmission.
 *
 * Stop-and-Wait and Selective Repeat retransmit the identified packet.
 * Go-Back-N retransmits the identified packet and all later outstanding
 * packets. The Go-Back-N base timer governs the outstanding window.
 */
static bool mark_one_for_retransmission(
    ArqContext *arq,
    PacketNumber number)
{
    ArqSendSlot *slot = find_send_slot(arq, number);

    if (slot == NULL) {
        return false;
    }

    if (arq->protocol == ARQ_SELECTIVE_REPEAT ||
        arq->protocol == ARQ_STOP_WAIT) {
        if (!timer_restart(
                slot->timer_id,
                rto_get(arq->rto))) {
            return false;
        }

        slot->retransmit_pending = true;
        return true;
    }

    uint16_t distance =
        (uint16_t)(number - arq->base_sequence_number);

    if ((size_t)distance >= arq->outstanding) {
        return false;
    }

    ArqSendSlot *base =
        find_send_slot(arq, arq->base_sequence_number);

    if (base == NULL ||
        !timer_restart(
            base->timer_id,
            rto_get(arq->rto))) {
        return false;
    }

    for (uint16_t i = distance; i < arq->outstanding; i++) {
        PacketNumber current =
            (PacketNumber)(arq->base_sequence_number + i);

        ArqSendSlot *current_slot =
            find_send_slot(arq, current);

        if (current_slot != NULL) {
            current_slot->retransmit_pending = true;
        }
    }

    return true;
}

/* ---------- Lifecycle ---------- */

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

void arq_destroy(ArqContext *arq)
{
    if (arq == NULL) {
        return;
    }

    for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
        release_send_slot(&arq->send_slots[i]);
        release_receive_slot(&arq->receive_slots[i]);
    }

    for (size_t i = 0; i < ARQ_DELIVERY_QUEUE_SIZE; i++) {
        arq_packet_release(&arq->delivery_queue[i]);
    }

    rto_destroy(arq->rto);
    free(arq);
}

/* ---------- Sender operations ---------- */

bool arq_can_send(const ArqContext *arq)
{
    if (arq == NULL ||
        arq->outstanding >= arq->window_size) {
        return false;
    }

    uint16_t sequence_span = (uint16_t)(
        arq->next_sequence_number -
        arq->base_sequence_number
    );

    if (sequence_span >= arq->window_size) {
        return false;
    }

    return find_send_slot_const(
        arq,
        arq->next_sequence_number
    ) == NULL;
}

bool arq_send(
    ArqContext *arq,
    const FileChunk *chunk,
    Packet *outgoing)
{
    if (arq == NULL || chunk == NULL || outgoing == NULL ||
        chunk->length > MAX_PAYLOAD_SIZE ||
        (chunk->length > 0 && chunk->data == NULL) ||
        !arq_can_send(arq)) {
        return false;
    }

    PacketNumber number = arq->next_sequence_number;

    uint8_t flags = (uint8_t)(
        FLAG_DATA | protocol_flags(arq->protocol)
    );

    Packet stored = {0};
    Packet transmission = {0};

    /*
     * Construct the logical DATA packet with Packet Creator.
     * This copies the chunk, so the caller can release its FileChunk
     * once arq_send() has completed successfully.
     */
    if (!packet_create_data(
            &stored,
            flags,
            number,
            chunk)) {
        arq_packet_release(&stored);
        return false;
    }

    /*
     * ARQ keeps its own copy and gives the Driver a separate copy.
     * The Driver can serialize and release its copy independently.
     */
    if (!copy_packet(&stored, &transmission)) {
        arq_packet_release(&stored);
        return false;
    }

    TimerId timer_id = timer_create();

    if (timer_id == 0) {
        arq_packet_release(&stored);
        arq_packet_release(&transmission);
        return false;
    }

    bool should_start_timer =
        arq->protocol == ARQ_SELECTIVE_REPEAT ||
        arq->outstanding == 0;

    if (should_start_timer &&
        !timer_start(timer_id, rto_get(arq->rto))) {
        (void)timer_destroy(timer_id);
        arq_packet_release(&stored);
        arq_packet_release(&transmission);
        return false;
    }

    ArqSendSlot *slot =
        &arq->send_slots[number % ARQ_MAX_WINDOW_SIZE];

    slot->used = true;
    slot->retransmit_pending = false;
    slot->packet = stored;
    slot->timer_id = timer_id;

    arq->next_sequence_number++;
    arq->outstanding++;

    *outgoing = transmission;

    return true;
}

/*
 * Process an ACK or NACK received by the sender.
 *
 * For cumulative ACK protocols, the ACK number identifies the
 * highest sequence number acknowledged.
 */
bool arq_receive(
    ArqContext *arq,
    const Packet *packet)
{
    if (arq == NULL ||
        !valid_control_packet(arq, packet)) {
        return false;
    }

    uint8_t type = packet_type_flags(packet);
    PacketNumber number = packet->number;

    if (type == FLAG_NACK) {
        return mark_one_for_retransmission(arq, number);
    }

    /* Selective Repeat: individual ACKs. */
    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        ArqSendSlot *slot = find_send_slot(arq, number);

        if (slot == NULL) {
            return false;
        }

        release_send_slot(slot);
        arq->outstanding--;

        advance_send_base(arq);

        return true;
    }

    /* Stop-and-Wait and Go-Back-N: cumulative ACKs. */
    if (arq->outstanding == 0) {
        return false;
    }

    uint16_t distance =
        (uint16_t)(number - arq->base_sequence_number);

    if ((size_t)distance >= arq->outstanding) {
        return false;
    }

    size_t ack_count = (size_t)distance + 1;

    for (size_t i = 0; i < ack_count; i++) {
        PacketNumber current =
            (PacketNumber)(arq->base_sequence_number + i);

        ArqSendSlot *slot = find_send_slot(arq, current);

        if (slot != NULL) {
            release_send_slot(slot);
            arq->outstanding--;
        }
    }

    arq->base_sequence_number =
        (PacketNumber)(number + 1);

    if (arq->outstanding == 0) {
        arq->base_sequence_number =
            arq->next_sequence_number;

        return true;
    }

    /*
     * For Go-Back-N, the next base's timer may have been created
     * but not started yet. timer_restart supports both states.
     */
    ArqSendSlot *base =
        find_send_slot(arq, arq->base_sequence_number);

    if (base != NULL) {
        (void)timer_restart(
            base->timer_id,
            rto_get(arq->rto)
        );
    }

    return true;
}

/* ---------- Timeout and retransmission operations ---------- */

bool arq_should_retransmit(ArqContext *arq)
{
    if (arq == NULL || arq->outstanding == 0) {
        return false;
    }

    /*
     * Selective Repeat maintains an independent timer per packet.
     * Schedule every expired packet, then report whether any
     * retransmissions are pending.
     */
    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        for (size_t i = 0; i < ARQ_MAX_WINDOW_SIZE; i++) {
            ArqSendSlot *slot = &arq->send_slots[i];

            if (!slot->used ||
                !timer_expired(slot->timer_id)) {
                continue;
            }

            if (timer_restart(
                    slot->timer_id,
                    rto_get(arq->rto))) {
                slot->retransmit_pending = true;
            }
        }

        return has_pending_retransmission(arq);
    }

    /*
     * A NACK may already have scheduled retransmissions.
     * Report them instead of suppressing the retransmission batch.
     */
    if (has_pending_retransmission(arq)) {
        return true;
    }

    /*
     * Stop-and-Wait and Go-Back-N use the base packet's timer.
     */
    ArqSendSlot *base =
        find_send_slot(arq, arq->base_sequence_number);

    if (base == NULL ||
        !timer_expired(base->timer_id)) {
        return false;
    }

    if (!timer_restart(
            base->timer_id,
            rto_get(arq->rto))) {
        return false;
    }

    if (arq->protocol == ARQ_GO_BACK_N) {
        for (uint16_t i = 0; i < arq->outstanding; i++) {
            PacketNumber number =
                (PacketNumber)(arq->base_sequence_number + i);

            ArqSendSlot *slot = find_send_slot(arq, number);

            if (slot != NULL) {
                slot->retransmit_pending = true;
            }
        }
    } else {
        base->retransmit_pending = true;
    }

    return has_pending_retransmission(arq);
}

bool arq_get_retransmission(
    ArqContext *arq,
    Packet *outgoing)
{
    if (arq == NULL || outgoing == NULL) {
        return false;
    }

    /*
     * Selective Repeat scans the current sequence window in order,
     * allowing independent retransmission of pending packets.
     */
    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        for (uint16_t i = 0; i < arq->window_size; i++) {
            PacketNumber number =
                (PacketNumber)(arq->base_sequence_number + i);

            ArqSendSlot *slot = find_send_slot(arq, number);

            if (slot == NULL ||
                !slot->retransmit_pending) {
                continue;
            }

            Packet copy = {0};

            if (!copy_packet(&slot->packet, &copy)) {
                return false;
            }

            *outgoing = copy;
            slot->retransmit_pending = false;

            return true;
        }

        return false;
    }

    /* Stop-and-Wait and Go-Back-N return pending packets in order. */
    for (uint16_t i = 0; i < arq->outstanding; i++) {
        PacketNumber number =
            (PacketNumber)(arq->base_sequence_number + i);

        ArqSendSlot *slot = find_send_slot(arq, number);

        if (slot == NULL ||
            !slot->retransmit_pending) {
            continue;
        }

        Packet copy = {0};

        if (!copy_packet(&slot->packet, &copy)) {
            return false;
        }

        *outgoing = copy;
        slot->retransmit_pending = false;

        return true;
    }

    return false;
}

/* ---------- State accessors and RTO ---------- */

PacketNumber arq_next_sequence_number(const ArqContext *arq)
{
    return arq == NULL ? 0 : arq->next_sequence_number;
}

size_t arq_outstanding_count(const ArqContext *arq)
{
    return arq == NULL ? 0 : arq->outstanding;
}

PacketNumber arq_expected_receive_number(const ArqContext *arq)
{
    return arq == NULL ? 0 : arq->expected_receive_number;
}

void arq_record_rtt(
    ArqContext *arq,
    uint64_t rtt_ms,
    bool retransmitted)
{
    if (arq != NULL) {
        rto_update(
            arq->rto,
            rtt_ms,
            retransmitted
        );
    }
}

uint64_t arq_get_rto(const ArqContext *arq)
{
    return arq == NULL ? 0 : rto_get(arq->rto);
}

/* ---------- Receiver operations ---------- */

bool arq_receive_data(
    ArqContext *arq,
    const Packet *incoming,
    Packet *ack)
{
    if (arq == NULL || incoming == NULL || ack == NULL ||
        !valid_data_packet(arq, incoming)) {
        return false;
    }

    Packet generated_ack = {0};

    PacketNumber number = incoming->number;
    PacketNumber expected = arq->expected_receive_number;

    /*
     * Stop-and-Wait and Go-Back-N deliver only in-order packets.
     * Out-of-order and duplicate packets trigger the last cumulative
     * ACK, allowing the sender to recover from lost ACKs.
     */
    if (arq->protocol == ARQ_STOP_WAIT ||
        arq->protocol == ARQ_GO_BACK_N) {
        if (number == expected) {
            if (arq->delivery_count >= ARQ_DELIVERY_QUEUE_SIZE) {
                return false;
            }

            Packet copy = {0};

            if (!copy_packet(incoming, &copy)) {
                return false;
            }

            if (!enqueue_delivered_packet(arq, &copy)) {
                arq_packet_release(&copy);
                return false;
            }

            arq->expected_receive_number++;
        }

        PacketNumber ack_number =
            (PacketNumber)(arq->expected_receive_number - 1);

        if (!packet_create_control(
                &generated_ack,
                (uint8_t)(
                    FLAG_ACK | protocol_flags(arq->protocol)
                ),
                ack_number)) {
            return false;
        }

        *ack = generated_ack;
        return true;
    }

    /*
     * Selective Repeat: accept packets within the receive window.
     * A successfully buffered packet can be ACKed even if the delivery
     * queue is full, because the receive slot retains the packet.
     */
    uint16_t distance = (uint16_t)(number - expected);

    if (distance < arq->window_size) {
        ArqReceiveSlot *slot =
            &arq->receive_slots[
                number % ARQ_MAX_WINDOW_SIZE
            ];

        if (!slot->used) {
            Packet copy = {0};

            if (!copy_packet(incoming, &copy)) {
                return false;
            }

            slot->packet = copy;
            slot->used = true;
        } else if (slot->packet.number != number) {
            /*
             * Never overwrite a different retained packet occupying
             * the same receive-slot index.
             */
            return false;
        }

        (void)deliver_in_order(arq);

        if (!packet_create_control(
                &generated_ack,
                (uint8_t)(
                    FLAG_ACK | protocol_flags(arq->protocol)
                ),
                number)) {
            return false;
        }

        *ack = generated_ack;
        return true;
    }

    /* Re-ACK an old duplicate within the previous receive window. */
    uint16_t behind = (uint16_t)(expected - number);

    if (behind <= arq->window_size) {
        if (!packet_create_control(
                &generated_ack,
                (uint8_t)(
                    FLAG_ACK | protocol_flags(arq->protocol)
                ),
                number)) {
            return false;
        }

        *ack = generated_ack;
        return true;
    }

    return false;
}

bool arq_get_delivered(
    ArqContext *arq,
    Packet *delivered)
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
        (arq->delivery_head + 1) %
        ARQ_DELIVERY_QUEUE_SIZE;

    arq->delivery_count--;

    if (arq->protocol == ARQ_SELECTIVE_REPEAT) {
        (void)deliver_in_order(arq);
    }

    return true;
}