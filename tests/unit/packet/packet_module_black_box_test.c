
#include "../src/packet/packet.h"
#include "../src/common/constants.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Helpers ---------- */

static void test_creator_and_dewrapper(void)
{
    const uint8_t source[] = {0x00, 0xFF, 0x13, 0x7A, 0x80};
    FileChunk chunk = {
        .data = (uint8_t *)source,
        .length = sizeof(source)
    };

    Packet packet;

    assert(packet_create_data(&packet, FLAG_DATA, 42, &chunk));

    assert(packet.flags == FLAG_DATA);
    assert(packet.number == 42);
    assert(packet.payload_length == sizeof(source));
    assert(packet.checksum == 0);
    assert(packet.payload != NULL);
    assert(memcmp(packet.payload, source, sizeof(source)) == 0);

    /* Packet payload must be independent of the source chunk. */
    assert(packet.payload != source);

    FileChunk extracted = {0};

    assert(packet_dewrap(&packet, &extracted));
    assert(extracted.length == sizeof(source));
    assert(extracted.data != NULL);
    assert(memcmp(extracted.data, source, sizeof(source)) == 0);
    assert(extracted.data != packet.payload);

    free(extracted.data);
    packet_release_payload(&packet);

    assert(packet.payload == NULL);
    assert(packet.payload_length == 0);
}

static void test_control_packet(void)
{
    Packet packet;

    assert(packet_create_control(&packet, FLAG_ACK, 123));

    assert(packet.flags == FLAG_ACK);
    assert(packet.number == 123);
    assert(packet.payload_length == 0);
    assert(packet.checksum == 0);
    assert(packet.payload == NULL);

    packet_release_payload(&packet);
}

static void test_empty_payload(void)
{
    FileChunk chunk = {
        .data = NULL,
        .length = 0
    };

    Packet packet;

    assert(packet_create_data(&packet, FLAG_DATA, 0, &chunk));
    assert(packet.payload_length == 0);
    assert(packet.payload == NULL);

    FileChunk extracted = {0};

    assert(packet_dewrap(&packet, &extracted));
    assert(extracted.length == 0);
    assert(extracted.data == NULL);

    packet_release_payload(&packet);
}

static void test_maximum_payload(void)
{
    uint8_t *source = malloc(MAX_PAYLOAD_SIZE);
    assert(source != NULL);

    for (size_t i = 0; i < MAX_PAYLOAD_SIZE; ++i)
        source[i] = (uint8_t)(i * 37u);

    FileChunk chunk = {
        .data = source,
        .length = MAX_PAYLOAD_SIZE
    };

    Packet packet;

    assert(packet_create_data(&packet, FLAG_DATA, 65535, &chunk));
    assert(packet.payload_length == MAX_PAYLOAD_SIZE);
    assert(memcmp(packet.payload, source, MAX_PAYLOAD_SIZE) == 0);

    packet_release_payload(&packet);
    free(source);
}

static void test_invalid_creation_arguments(void)
{
    Packet packet;

    FileChunk oversized = {
        .data = (uint8_t *)"x",
        .length = MAX_PAYLOAD_SIZE + 1
    };

    FileChunk missing_data = {
        .data = NULL,
        .length = 1
    };

    assert(!packet_create_data(NULL, FLAG_DATA, 0, &oversized));

    assert(!packet_create_data(&packet, FLAG_DATA, 0, NULL));

    assert(!packet_create_data(&packet, FLAG_DATA, 0, &oversized));

    assert(!packet_create_data(&packet, FLAG_DATA, 0, &missing_data));

    assert(!packet_create_control(NULL, FLAG_ACK, 0));
}

static void test_round_trip(void)
{
    const uint8_t source[] = {
        0x00, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A
    };

    FileChunk chunk = {
        .data = (uint8_t *)source,
        .length = sizeof(source)
    };

    Packet original;

    assert(packet_create_data(&original, FLAG_DATA, 0x1234, &chunk));

    uint8_t buffer[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
    size_t serialized_size = 0;

    assert(packet_serialize(
        &original, buffer, sizeof(buffer), &serialized_size));

    assert(serialized_size == PACKET_HEADER_SIZE + sizeof(source));

    /* Verify the public wire-format fields. */
    assert(buffer[0] == FLAG_DATA);
    assert(buffer[1] == 0x12);
    assert(buffer[2] == 0x34);
    assert(buffer[3] == 0x00);
    assert(buffer[4] == sizeof(source));

    assert(memcmp(
        buffer + PACKET_HEADER_SIZE,
        source,
        sizeof(source)) == 0);

    ArqPacketInput received = {0};

    assert(packet_receive(buffer, serialized_size, &received));
    printf("packet_receive returned valid = %d\n", received.valid);
    assert(received.valid);

    assert(received.packet.flags == original.flags);
    assert(received.packet.number == original.number);
    assert(received.packet.payload_length == original.payload_length);
    assert(received.packet.checksum == original.checksum);
    assert(memcmp(
        received.packet.payload,
        source,
        sizeof(source)) == 0);

    packet_release_payload(&received.packet);
    packet_release_payload(&original);
}

static void test_insufficient_serialization_buffer(void)
{
    const uint8_t source[] = {1, 2, 3, 4};

    FileChunk chunk = {
        .data = (uint8_t *)source,
        .length = sizeof(source)
    };

    Packet packet;
    assert(packet_create_data(&packet, FLAG_DATA, 1, &chunk));

    uint8_t buffer[PACKET_HEADER_SIZE + sizeof(source) - 1];
    size_t serialized_size = 0;

    assert(!packet_serialize(
        &packet, buffer, sizeof(buffer), &serialized_size));

    packet_release_payload(&packet);
}

static void test_invalid_receive_inputs(void)
{
    uint8_t buffer[PACKET_HEADER_SIZE + 4] = {0};
    ArqPacketInput result = {0};

    /* NULL arguments are internal API errors. */
    assert(!packet_receive(NULL, sizeof(buffer), &result));
    assert(!packet_receive(buffer, sizeof(buffer), NULL));

    /* Truncated header is malformed network input. */
    assert(packet_receive(buffer, PACKET_HEADER_SIZE - 1, &result));
    assert(!result.valid);

    /* Advertise a payload length inconsistent with datagram size. */
    buffer[3] = 0;
    buffer[4] = 4;

    assert(packet_receive(buffer, PACKET_HEADER_SIZE, &result));
    assert(!result.valid);

    /* Advertise a payload larger than the supported maximum. */
    buffer[3] = (uint8_t)((MAX_PAYLOAD_SIZE + 1) >> 8);
    buffer[4] = (uint8_t)(MAX_PAYLOAD_SIZE + 1);

    assert(packet_receive(buffer, sizeof(buffer), &result));
    assert(!result.valid);
}

static void test_corrupted_packet_rejected(void)
{
    const uint8_t source[] = {0x11, 0x22, 0x33, 0x44};

    FileChunk chunk = {
        .data = (uint8_t *)source,
        .length = sizeof(source)
    };

    Packet packet;
    assert(packet_create_data(&packet, FLAG_DATA, 7, &chunk));

    uint8_t buffer[PACKET_HEADER_SIZE + sizeof(source)];
    size_t serialized_size;

    assert(packet_serialize(
        &packet, buffer, sizeof(buffer), &serialized_size));

    /* Change one payload byte without updating the checksum. */
    buffer[PACKET_HEADER_SIZE] ^= 0x01;

    ArqPacketInput result = {0};

    assert(packet_receive(buffer, serialized_size, &result));
    assert(!result.valid);

    packet_release_payload(&packet);
}

int main(void)
{
    test_creator_and_dewrapper();
    test_control_packet();
    test_empty_payload();
    test_maximum_payload();
    test_invalid_creation_arguments();
    test_round_trip();
    test_insufficient_serialization_buffer();
    test_invalid_receive_inputs();
    test_corrupted_packet_rejected();

    puts("All packet tests passed.");
    return 0;
}
