#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../../../src/arq/arq.h"
#include "../../../src/common/constants.h"
#include "../../../src/packet/packet.h"
#include "../../../src/rto/rto.h"

/* ---------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------- */

static uint8_t test_protocol_flags(ArqProtocol protocol)
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

/*
 * Creates an owned DATA packet to simulate an incoming,
 * decoded and verified packet.
 */
static Packet make_data_packet(
    ArqProtocol protocol,
    PacketNumber number,
    const uint8_t *data,
    size_t length)
{
    assert(length <= MAX_PAYLOAD_SIZE);
    assert(length == 0 || data != NULL);

    Packet packet = {0};

    packet.flags = (uint8_t)(
        FLAG_DATA | test_protocol_flags(protocol)
    );
    packet.number = number;
    packet.payload_length = (PayloadLength)length;
    packet.checksum = 0;

    if (length > 0) {
        packet.payload = malloc(length);
        assert(packet.payload != NULL);

        memcpy(packet.payload, data, length);
    }

    return packet;
}

static Packet make_control_packet(
    ArqProtocol protocol,
    uint8_t type,
    PacketNumber number)
{
    assert(type == FLAG_ACK || type == FLAG_NACK);

    Packet packet = {0};

    bool created = packet_create_control(
        &packet,
        (uint8_t)(type | test_protocol_flags(protocol)),
        number
    );

    assert(created);
    return packet;
}

static void assert_ack(
    const Packet *ack,
    ArqProtocol protocol,
    PacketNumber expected_number)
{
    assert(ack != NULL);
    assert(ack->flags ==
           (uint8_t)(FLAG_ACK | test_protocol_flags(protocol)));
    assert(ack->number == expected_number);
    assert(ack->payload_length == 0);
    assert(ack->payload == NULL);
}

/*
 * Send one FileChunk through ARQ, and check the resulting logical
 * DATA packet. The input buffer is borrowed; the output packet
 * is caller-owned.
 */
static void send_chunk(
    ArqContext *arq,
    ArqProtocol protocol,
    PacketNumber expected_number,
    uint8_t *data,
    size_t length)
{
    FileChunk chunk = {
        .data = data,
        .length = length
    };

    Packet outgoing = {0};

    assert(arq_can_send(arq));
    assert(arq_send(arq, &chunk, &outgoing));

    assert(outgoing.number == expected_number);
    assert(outgoing.flags ==
           (uint8_t)(FLAG_DATA | test_protocol_flags(protocol)));
    assert(outgoing.payload_length == length);

    if (length > 0) {
        assert(outgoing.payload != NULL);
        assert(outgoing.payload != data);
        assert(memcmp(outgoing.payload, data, length) == 0);
    }

    assert(arq_next_sequence_number(arq) ==
           (PacketNumber)(expected_number + 1));

    arq_packet_release(&outgoing);
}

static void expect_delivery(
    ArqContext *arq,
    PacketNumber expected_number,
    const char *expected_text)
{
    Packet delivered = {0};
    size_t expected_length = strlen(expected_text);

    assert(arq_get_delivered(arq, &delivered));

    assert(delivered.number == expected_number);
    assert(delivered.payload_length == expected_length);
    assert(delivered.payload != NULL);

    assert(memcmp(
        delivered.payload,
        expected_text,
        expected_length
    ) == 0);

    arq_packet_release(&delivered);
}

/* ---------------------------------------------------------
 * ARQ configuration and input validation
 * --------------------------------------------------------- */

static void test_create_validation(void)
{
    assert(arq_create(
        ARQ_STOP_WAIT, 0, 100, 10, 5000
    ) == NULL);

    assert(arq_create(
        ARQ_STOP_WAIT, 2, 100, 10, 5000
    ) == NULL);

    assert(arq_create(
        ARQ_GO_BACK_N, ARQ_MAX_WINDOW_SIZE + 1,
        100, 10, 5000
    ) == NULL);

    assert(arq_create(
        ARQ_SELECTIVE_REPEAT, 0, 100, 10, 5000
    ) == NULL);

    assert(arq_create(
        (ArqProtocol)99, 1, 100, 10, 5000
    ) == NULL);

    puts("PASS: ARQ creation validation");
}

static void test_invalid_send_input(void)
{
    ArqContext *arq = arq_create(
        ARQ_STOP_WAIT, 1, 1000, 100, 5000
    );
    assert(arq != NULL);

    Packet outgoing = {0};

    /* Non-empty chunk with a NULL data pointer. */
    FileChunk missing_data = {
        .data = NULL,
        .length = 1
    };

    assert(!arq_send(arq, &missing_data, &outgoing));
    assert(arq_next_sequence_number(arq) == 0);
    assert(arq_outstanding_count(arq) == 0);

    /* Chunk exceeds the packet payload limit. */
    uint8_t small_buffer[] = {1, 2, 3};

    FileChunk oversized = {
        .data = small_buffer,
        .length = MAX_PAYLOAD_SIZE + 1
    };

    assert(!arq_send(arq, &oversized, &outgoing));
    assert(arq_next_sequence_number(arq) == 0);
    assert(arq_outstanding_count(arq) == 0);

    /* A failed call must not populate the output packet. */
    assert(outgoing.flags == 0);
    assert(outgoing.payload == NULL);
    assert(outgoing.payload_length == 0);

    arq_destroy(arq);

    puts("PASS: invalid send inputs");
}

/* ---------------------------------------------------------
 * Stop-and-Wait sender
 * --------------------------------------------------------- */

static void test_stop_and_wait_sender(void)
{
    ArqContext *arq = arq_create(
        ARQ_STOP_WAIT, 1, 30, 10, 5000
    );
    assert(arq != NULL);

    uint8_t payload[] = {1, 2, 3};

    send_chunk(
        arq, ARQ_STOP_WAIT, 0,
        payload, sizeof(payload)
    );

    assert(arq_outstanding_count(arq) == 1);
    assert(!arq_can_send(arq));

    /* A mismatched protocol ACK must be rejected. */
    Packet wrong_ack = make_control_packet(
        ARQ_GO_BACK_N, FLAG_ACK, 0
    );

    assert(!arq_receive(arq, &wrong_ack));
    arq_packet_release(&wrong_ack);

    /* Wait past the configured initial retransmission timeout. */
    sleep_ms(80);

    assert(arq_should_retransmit(arq));

    Packet retransmission = {0};

    assert(arq_get_retransmission(arq, &retransmission));
    assert(retransmission.number == 0);
    assert(retransmission.payload_length == sizeof(payload));
    assert(memcmp(
        retransmission.payload,
        payload,
        sizeof(payload)
    ) == 0);

    arq_packet_release(&retransmission);

    assert(!arq_get_retransmission(arq, &retransmission));

    Packet ack = make_control_packet(
        ARQ_STOP_WAIT, FLAG_ACK, 0
    );

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 0);
    assert(arq_can_send(arq));

    arq_packet_release(&ack);
    arq_destroy(arq);

    puts("PASS: Stop-and-Wait sender and retransmission");
}

/* ---------------------------------------------------------
 * Go-Back-N sender
 * --------------------------------------------------------- */

static void test_go_back_n_sender(void)
{
    ArqContext *arq = arq_create(
        ARQ_GO_BACK_N, 3, 30, 10, 5000
    );
    assert(arq != NULL);

    uint8_t zero[] = {'z', 'e', 'r', 'o'};
    uint8_t one[]  = {'o', 'n', 'e'};
    uint8_t two[]  = {'t', 'w', 'o'};

    send_chunk(arq, ARQ_GO_BACK_N, 0, zero, sizeof(zero));
    send_chunk(arq, ARQ_GO_BACK_N, 1, one, sizeof(one));
    send_chunk(arq, ARQ_GO_BACK_N, 2, two, sizeof(two));

    assert(arq_outstanding_count(arq) == 3);
    assert(!arq_can_send(arq));

    sleep_ms(80);

    assert(arq_should_retransmit(arq));

    /* Go-Back-N timeout schedules the entire outstanding window. */
    Packet retransmission = {0};

    for (PacketNumber i = 0; i < 3; i++) {
        assert(arq_get_retransmission(arq, &retransmission));
        assert(retransmission.number == i);

        arq_packet_release(&retransmission);
    }

    assert(!arq_get_retransmission(arq, &retransmission));

    /* Cumulative ACK for sequence 0 frees the first slot. */
    Packet ack = make_control_packet(
        ARQ_GO_BACK_N, FLAG_ACK, 0
    );

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 2);

    arq_packet_release(&ack);

    /*
     * NACK for sequence 1 should schedule sequence 1 and
     * every later outstanding packet.
     */
    Packet nack = make_control_packet(
        ARQ_GO_BACK_N, FLAG_NACK, 1
    );

    assert(arq_receive(arq, &nack));
    arq_packet_release(&nack);

    for (PacketNumber expected = 1; expected <= 2; expected++) {
        assert(arq_get_retransmission(arq, &retransmission));
        assert(retransmission.number == expected);

        arq_packet_release(&retransmission);
    }

    assert(!arq_get_retransmission(arq, &retransmission));

    /* Cumulative ACK for sequence 2 frees the rest of the window. */
    ack = make_control_packet(ARQ_GO_BACK_N, FLAG_ACK, 2);

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 0);
    assert(arq_can_send(arq));

    arq_packet_release(&ack);
    arq_destroy(arq);

    puts("PASS: Go-Back-N sender, cumulative ACK and NACK");
}

/* ---------------------------------------------------------
 * Selective Repeat sender
 * --------------------------------------------------------- */

static void test_selective_repeat_sender(void)
{
    ArqContext *arq = arq_create(
        ARQ_SELECTIVE_REPEAT, 3, 30, 10, 5000
    );
    assert(arq != NULL);

    uint8_t zero[] = {'z', 'e', 'r', 'o'};
    uint8_t one[]  = {'o', 'n', 'e'};
    uint8_t two[]  = {'t', 'w', 'o'};

    send_chunk(
        arq, ARQ_SELECTIVE_REPEAT, 0, zero, sizeof(zero)
    );
    send_chunk(
        arq, ARQ_SELECTIVE_REPEAT, 1, one, sizeof(one)
    );
    send_chunk(
        arq, ARQ_SELECTIVE_REPEAT, 2, two, sizeof(two)
    );

    assert(arq_outstanding_count(arq) == 3);

    /* ACK only sequence 1. The others remain outstanding. */
    Packet ack = make_control_packet(
        ARQ_SELECTIVE_REPEAT, FLAG_ACK, 1
    );

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 2);

    arq_packet_release(&ack);

    sleep_ms(80);

    assert(arq_should_retransmit(arq));

    Packet retransmission = {0};

    /* Individual timers expire for sequences 0 and 2. */
    assert(arq_get_retransmission(arq, &retransmission));
    assert(retransmission.number == 0);
    arq_packet_release(&retransmission);

    assert(arq_get_retransmission(arq, &retransmission));
    assert(retransmission.number == 2);
    arq_packet_release(&retransmission);

    assert(!arq_get_retransmission(arq, &retransmission));

    /* ACKs for 0 and 2 independently clear the remaining slots. */
    ack = make_control_packet(ARQ_SELECTIVE_REPEAT, FLAG_ACK, 0);
    assert(arq_receive(arq, &ack));
    arq_packet_release(&ack);

    ack = make_control_packet(ARQ_SELECTIVE_REPEAT, FLAG_ACK, 2);
    assert(arq_receive(arq, &ack));
    arq_packet_release(&ack);

    assert(arq_outstanding_count(arq) == 0);
    assert(arq_can_send(arq));

    arq_destroy(arq);

    puts("PASS: Selective Repeat sender and individual ACKs");
}

/* ---------------------------------------------------------
 * Stop-and-Wait receiver
 * --------------------------------------------------------- */

static void test_stop_and_wait_receiver(void)
{
    ArqContext *arq = arq_create(
        ARQ_STOP_WAIT, 1, 1000, 100, 5000
    );
    assert(arq != NULL);

    Packet ack = {0};
    Packet incoming = make_data_packet(
        ARQ_STOP_WAIT, 0,
        (const uint8_t *)"first", 5
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_STOP_WAIT, 0);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 0, "first");

    /* A duplicate DATA packet is ACKed but not delivered again. */
    incoming = make_data_packet(
        ARQ_STOP_WAIT, 0,
        (const uint8_t *)"first", 5
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_STOP_WAIT, 0);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    Packet delivered = {0};
    assert(!arq_get_delivered(arq, &delivered));

    incoming = make_data_packet(
        ARQ_STOP_WAIT, 1,
        (const uint8_t *)"second", 6
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_STOP_WAIT, 1);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 1, "second");

    arq_destroy(arq);

    puts("PASS: Stop-and-Wait receiver and duplicate handling");
}

/* ---------------------------------------------------------
 * Go-Back-N receiver
 * --------------------------------------------------------- */

static void test_go_back_n_receiver(void)
{
    ArqContext *arq = arq_create(
        ARQ_GO_BACK_N, 3, 1000, 100, 5000
    );
    assert(arq != NULL);

    Packet ack = {0};

    Packet incoming = make_data_packet(
        ARQ_GO_BACK_N, 0,
        (const uint8_t *)"zero", 4
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_GO_BACK_N, 0);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 0, "zero");

    /*
     * Go-Back-N discards out-of-order sequence 2 and repeats
     * the cumulative ACK for the last in-order sequence, 0.
     */
    incoming = make_data_packet(
        ARQ_GO_BACK_N, 2,
        (const uint8_t *)"two", 3
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_GO_BACK_N, 0);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    Packet delivered = {0};
    assert(!arq_get_delivered(arq, &delivered));

    /* Sequence 1 is accepted; sequence 2 must be retransmitted. */
    incoming = make_data_packet(
        ARQ_GO_BACK_N, 1,
        (const uint8_t *)"one", 3
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_GO_BACK_N, 1);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 1, "one");
    assert(!arq_get_delivered(arq, &delivered));

    incoming = make_data_packet(
        ARQ_GO_BACK_N, 2,
        (const uint8_t *)"two", 3
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_GO_BACK_N, 2);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 2, "two");

    arq_destroy(arq);

    puts("PASS: Go-Back-N receiver and cumulative ACK handling");
}

/* ---------------------------------------------------------
 * Selective Repeat receiver
 * --------------------------------------------------------- */

static void test_selective_repeat_receiver(void)
{
    ArqContext *arq = arq_create(
        ARQ_SELECTIVE_REPEAT, 3, 1000, 100, 5000
    );
    assert(arq != NULL);

    Packet ack = {0};

    /* Sequence 1 arrives before sequence 0 and is buffered. */
    Packet incoming = make_data_packet(
        ARQ_SELECTIVE_REPEAT, 1,
        (const uint8_t *)"one", 3
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_SELECTIVE_REPEAT, 1);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    Packet delivered = {0};
    assert(!arq_get_delivered(arq, &delivered));

    /* Sequence 0 allows 0 and the already-buffered 1 to be delivered. */
    incoming = make_data_packet(
        ARQ_SELECTIVE_REPEAT, 0,
        (const uint8_t *)"zero", 4
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_SELECTIVE_REPEAT, 0);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 0, "zero");
    expect_delivery(arq, 1, "one");

    incoming = make_data_packet(
        ARQ_SELECTIVE_REPEAT, 2,
        (const uint8_t *)"two", 3
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_SELECTIVE_REPEAT, 2);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    expect_delivery(arq, 2, "two");

    /* An old duplicate is ACKed again but not delivered twice. */
    incoming = make_data_packet(
        ARQ_SELECTIVE_REPEAT, 1,
        (const uint8_t *)"one", 3
    );

    assert(arq_receive_data(arq, &incoming, &ack));
    assert_ack(&ack, ARQ_SELECTIVE_REPEAT, 1);

    arq_packet_release(&incoming);
    arq_packet_release(&ack);

    assert(!arq_get_delivered(arq, &delivered));

    arq_destroy(arq);

    puts("PASS: Selective Repeat receiver, buffering and ordering");
}

/* ---------------------------------------------------------
 * Ownership
 * --------------------------------------------------------- */

static void test_packet_ownership(void)
{
    ArqContext *arq = arq_create(
        ARQ_STOP_WAIT, 1, 30, 10, 5000
    );
    assert(arq != NULL);

    uint8_t input_data[] = {'t', 'e', 's', 't'};

    FileChunk chunk = {
        .data = input_data,
        .length = sizeof(input_data)
    };

    Packet outgoing = {0};

    assert(arq_send(arq, &chunk, &outgoing));

    assert(outgoing.payload != NULL);
    assert(outgoing.payload != input_data);
    assert(outgoing.payload_length == sizeof(input_data));
    assert(memcmp(
        outgoing.payload,
        "test",
        sizeof(input_data)
    ) == 0);

    /*
     * Change the original buffer and free the caller's transmission
     * copy. ARQ's retained copy must remain valid for retransmission.
     */
    memset(input_data, 'X', sizeof(input_data));
    arq_packet_release(&outgoing);

    sleep_ms(80);

    assert(arq_should_retransmit(arq));
    assert(arq_get_retransmission(arq, &outgoing));

    assert(outgoing.number == 0);
    assert(outgoing.payload_length == 4);
    assert(memcmp(outgoing.payload, "test", 4) == 0);

    arq_packet_release(&outgoing);

    Packet ack = make_control_packet(
        ARQ_STOP_WAIT, FLAG_ACK, 0
    );

    assert(arq_receive(arq, &ack));
    arq_packet_release(&ack);

    assert(arq_outstanding_count(arq) == 0);

    arq_destroy(arq);

    puts("PASS: packet payload ownership");
}

/* ---------------------------------------------------------
 * RTT/RTO integration
 * --------------------------------------------------------- */

static void test_rto_integration(void)
{
    ArqContext *arq = arq_create(
        ARQ_STOP_WAIT, 1, 1000, 100, 5000
    );
    assert(arq != NULL);

    assert(arq_get_rto(arq) == 1000);

    arq_record_rtt(arq, 100, false);
    assert(arq_get_rto(arq) == 300);

    uint64_t previous_rto = arq_get_rto(arq);

    /* Karn's algorithm ignores RTT samples from retransmissions. */
    arq_record_rtt(arq, 500, true);
    assert(arq_get_rto(arq) == previous_rto);

    arq_record_rtt(arq, 120, false);
    assert(arq_get_rto(arq) == 270);

    arq_destroy(arq);

    puts("PASS: ARQ RTT/RTO integration and Karn's algorithm");
}

static void test_rto_module(void)
{
    RtoEstimator *rto = rto_create(1000, 100, 5000);
    assert(rto != NULL);

    assert(rto_get(rto) == 1000);

    rto_update(rto, 100, false);
    assert(rto_get_srtt(rto) == 100);
    assert(rto_get_rttvar(rto) == 50);
    assert(rto_get(rto) == 300);

    rto_update(rto, 500, true);
    assert(rto_get(rto) == 300);

    rto_update(rto, 120, false);
    assert(rto_get_srtt(rto) == 102);
    assert(rto_get_rttvar(rto) == 42);
    assert(rto_get(rto) == 270);

    rto_destroy(rto);

    puts("PASS: standalone RTO estimator");
}

/* ---------------------------------------------------------
 * Entry point
 * --------------------------------------------------------- */

int main(void)
{
    test_create_validation();
    test_invalid_send_input();

    test_stop_and_wait_sender();
    test_go_back_n_sender();
    test_selective_repeat_sender();

    test_stop_and_wait_receiver();
    test_go_back_n_receiver();
    test_selective_repeat_receiver();

    test_packet_ownership();

    test_rto_integration();
    test_rto_module();

    puts("\nAll ARQ tests passed.");
    return 0;
}