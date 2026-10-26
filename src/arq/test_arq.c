
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "arq.h"
#include "../common/constants.h"
#include "../rto/rto.h"

static Packet make_packet(PacketNumber number, const char *text)
{
    Packet packet = {0};
    size_t length = strlen(text);

    packet.flags = FLAG_DATA;
    packet.number = number;
    packet.payload_length = (PayloadLength)length;
    packet.payload = malloc(length);

    assert(packet.payload != NULL || length == 0);

    if (length > 0) {
        memcpy(packet.payload, text, length);
    }

    return packet;
}

static void release_input(Packet *packet)
{
    free(packet->payload);
    packet->payload = NULL;
    packet->payload_length = 0;
}

static void expect_delivery(
    ArqContext *arq,
    PacketNumber expected_number,
    const char *expected_text)
{
    Packet delivered = {0};

    assert(arq_get_delivered(arq, &delivered));
    assert(delivered.number == expected_number);
    assert(delivered.payload_length == strlen(expected_text));
    assert(memcmp(
        delivered.payload,
        expected_text,
        delivered.payload_length
    ) == 0);

    arq_packet_release(&delivered);
}

static void send_packet(ArqContext *arq, PacketNumber number)
{
    static uint8_t payload[] = {1, 2, 3};

    Packet packet = {0};
    Packet outgoing = {0};

    packet.flags = FLAG_DATA;
    packet.number = number;
    packet.payload_length = sizeof(payload);
    packet.payload = payload;

    assert(arq_send(arq, &packet, &outgoing));
    assert(outgoing.number == number);
}

static void test_stop_and_wait_sender(void)
{
    ArqContext *arq = arq_create(ARQ_STOP_WAIT, 1, 30, 10, 5000);
    assert(arq != NULL);

    send_packet(arq, 0);
    assert(arq_outstanding_count(arq) == 1);

    Sleep(50);

    assert(arq_should_retransmit(arq));

    Packet retransmission = {0};
    assert(arq_get_retransmission(arq, &retransmission));
    assert(retransmission.number == 0);

    Packet ack = {0};
    ack.flags = FLAG_ACK;
    ack.number = 0;

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 0);

    arq_destroy(arq);
}

static void test_go_back_n_sender(void)
{
    ArqContext *arq = arq_create(ARQ_GO_BACK_N, 3, 30, 10, 5000);
    assert(arq != NULL);

    send_packet(arq, 0);
    send_packet(arq, 1);
    send_packet(arq, 2);

    assert(arq_outstanding_count(arq) == 3);

    Sleep(50);

    assert(arq_should_retransmit(arq));

    Packet retransmission = {0};

    for (PacketNumber i = 0; i < 3; i++) {
        assert(arq_get_retransmission(arq, &retransmission));
        assert(retransmission.number == i);
    }

    Packet ack = {0};
    ack.flags = FLAG_ACK;
    ack.number = 0;

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 2);

    arq_destroy(arq);
}

static void test_selective_repeat_sender(void)
{
    ArqContext *arq = arq_create(ARQ_SELECTIVE_REPEAT, 3, 30, 10, 5000);
    assert(arq != NULL);

    send_packet(arq, 0);
    send_packet(arq, 1);
    send_packet(arq, 2);

    Packet ack = {0};
    ack.flags = FLAG_ACK;
    ack.number = 1;

    assert(arq_receive(arq, &ack));
    assert(arq_outstanding_count(arq) == 2);

    Sleep(50);

    assert(arq_should_retransmit(arq));

    Packet retransmission = {0};

    assert(arq_get_retransmission(arq, &retransmission));
    assert(retransmission.number == 0);

    assert(arq_get_retransmission(arq, &retransmission));
    assert(retransmission.number == 2);

    assert(!arq_get_retransmission(arq, &retransmission));

    arq_destroy(arq);
}

static void test_stop_and_wait_receiver(void)
{
    ArqContext *arq = arq_create(ARQ_STOP_WAIT, 1, 100, 10, 5000);
    assert(arq != NULL);

    Packet ack = {0};
    Packet incoming = make_packet(0, "first");

    assert(arq_receive_data(arq, &incoming, &ack));
    assert((ack.flags & FLAG_ACK) != 0);
    assert(ack.number == 0);

    release_input(&incoming);
    expect_delivery(arq, 0, "first");

    incoming = make_packet(0, "first");
    assert(arq_receive_data(arq, &incoming, &ack));
    release_input(&incoming);

    Packet delivered = {0};
    assert(!arq_get_delivered(arq, &delivered));

    incoming = make_packet(1, "second");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 1);

    release_input(&incoming);
    expect_delivery(arq, 1, "second");

    arq_destroy(arq);
}

static void test_go_back_n_receiver(void)
{
    ArqContext *arq = arq_create(ARQ_GO_BACK_N, 3, 100, 10, 5000);
    assert(arq != NULL);

    Packet ack = {0};
    Packet incoming = make_packet(0, "zero");

    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 0);

    release_input(&incoming);
    expect_delivery(arq, 0, "zero");

    incoming = make_packet(2, "two");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 0);

    release_input(&incoming);

    Packet delivered = {0};
    assert(!arq_get_delivered(arq, &delivered));

    incoming = make_packet(1, "one");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 1);

    release_input(&incoming);
    expect_delivery(arq, 1, "one");

    incoming = make_packet(2, "two");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 2);

    release_input(&incoming);
    expect_delivery(arq, 2, "two");

    arq_destroy(arq);
}

static void test_selective_repeat_receiver(void)
{
    ArqContext *arq = arq_create(ARQ_SELECTIVE_REPEAT, 3, 100, 10, 5000);
    assert(arq != NULL);

    Packet ack = {0};
    Packet incoming = make_packet(1, "one");

    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 1);

    release_input(&incoming);

    Packet delivered = {0};
    assert(!arq_get_delivered(arq, &delivered));

    incoming = make_packet(0, "zero");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 0);

    release_input(&incoming);

    expect_delivery(arq, 0, "zero");
    expect_delivery(arq, 1, "one");

    incoming = make_packet(2, "two");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 2);

    release_input(&incoming);
    expect_delivery(arq, 2, "two");

    incoming = make_packet(1, "one");
    assert(arq_receive_data(arq, &incoming, &ack));
    assert(ack.number == 1);

    release_input(&incoming);
    assert(!arq_get_delivered(arq, &delivered));

    arq_destroy(arq);
}

static void test_rto_integration(void)
{
    ArqContext *arq = arq_create(ARQ_STOP_WAIT, 1, 1000, 100, 5000);
    assert(arq != NULL);

    assert(arq_get_rto(arq) == 1000);

    arq_record_rtt(arq, 100, false);
    assert(arq_get_rto(arq) == 300);

    uint64_t previous_rto = arq_get_rto(arq);

    arq_record_rtt(arq, 500, true);
    assert(arq_get_rto(arq) == previous_rto);

    arq_record_rtt(arq, 120, false);
    assert(arq_get_rto(arq) != previous_rto);

    arq_destroy(arq);
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
}

int main(void)
{
    test_stop_and_wait_sender();
    test_go_back_n_sender();
    test_selective_repeat_sender();

    test_stop_and_wait_receiver();
    test_go_back_n_receiver();
    test_selective_repeat_receiver();

    test_rto_integration();
    test_rto_module();

    puts("All ARQ, receiver, retransmission, RTO and Karn tests passed!");
    return 0;
}
