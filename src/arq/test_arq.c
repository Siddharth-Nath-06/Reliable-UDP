
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "arq.h"
#include "../common/constants.h"

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

static void test_stop_and_wait_receiver(void)
{
    ArqContext *arq = arq_create(ARQ_STOP_WAIT, 1, 100);
    assert(arq != NULL);

    Packet incoming = make_packet(0, "first");
    Packet ack = {0};

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
    ArqContext *arq = arq_create(ARQ_GO_BACK_N, 3, 100);
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
    ArqContext *arq = arq_create(ARQ_SELECTIVE_REPEAT, 3, 100);
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

int main(void)
{
    test_stop_and_wait_receiver();
    test_go_back_n_receiver();
    test_selective_repeat_receiver();

    puts("All ARQ receiver tests passed!");
    return 0;
}
