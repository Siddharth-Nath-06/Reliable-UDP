#include "../../../src/packet/packet.h"
#include "../../../src/common/constants.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Helpers ---------- */

/*
 * Independent implementation of the Internet checksum for tests.
 * The checksum field must be zero while this function is called.
 */
static uint16_t test_internet_checksum(
    const uint8_t *data,
    size_t length)
{
    uint32_t sum = 0;

    while (length > 1) {
        uint16_t word =
            (uint16_t)(((uint16_t)data[0] << 8) | data[1]);

        sum += word;
        sum = (sum & 0xFFFFU) + (sum >> 16);

        data += 2;
        length -= 2;
    }

    if (length == 1) {
        sum += (uint16_t)data[0] << 8;
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/*
 * Build a wire-format packet directly. This enables testing whether
 * packet_receive() rejects malformed packets even when their
 * checksums are internally consistent.
 */
static size_t make_wire_packet(
    uint8_t flags,
    PacketNumber number,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *buffer,
    size_t buffer_capacity)
{
    assert(buffer != NULL);
    assert(payload_length <= MAX_PAYLOAD_SIZE);
    assert(payload_length == 0 || payload != NULL);

    size_t total_size = PACKET_HEADER_SIZE + payload_length;
    assert(buffer_capacity >= total_size);

    memset(buffer, 0, total_size);

    buffer[0] = flags;

    buffer[1] = (uint8_t)(number >> 8);
    buffer[2] = (uint8_t)number;

    buffer[3] = (uint8_t)(payload_length >> 8);
    buffer[4] = (uint8_t)payload_length;

    if (payload_length > 0) {
        memcpy(
            buffer + PACKET_HEADER_SIZE,
            payload,
            payload_length
        );
    }

    /* Checksum bytes 5 and 6 are currently zero. */
    uint16_t checksum =
        test_internet_checksum(buffer, total_size);

    buffer[5] = (uint8_t)(checksum >> 8);
    buffer[6] = (uint8_t)checksum;

    return total_size;
}

static void assert_valid_round_trip(Packet *original)
{
    uint8_t buffer[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
    size_t serialized_size = 0;

    assert(packet_serialize(
        original,
        buffer,
        sizeof(buffer),
        &serialized_size
    ));

    /*
     * Independently verify that the serialized checksum matches
     * the Internet checksum calculated over a zeroed checksum field.
     */
    uint16_t serialized_checksum =
        (uint16_t)(((uint16_t)buffer[5] << 8) | buffer[6]);

    uint8_t checksum_input[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
    memcpy(checksum_input, buffer, serialized_size);

    checksum_input[5] = 0;
    checksum_input[6] = 0;

    assert(
        test_internet_checksum(checksum_input, serialized_size) ==
        serialized_checksum
    );

    ArqPacketInput received = {0};

    assert(packet_receive(buffer, serialized_size, &received));
    assert(received.valid);

    assert(received.packet.flags == original->flags);
    assert(received.packet.number == original->number);
    assert(received.packet.payload_length == original->payload_length);
    assert(received.packet.checksum == original->checksum);

    if (original->payload_length > 0) {
        assert(received.packet.payload != NULL);
        assert(memcmp(
            received.packet.payload,
            original->payload,
            original->payload_length
        ) == 0);
    } else {
        assert(received.packet.payload == NULL);
    }

    packet_release_payload(&received.packet);
}

/* ---------- Original tests ---------- */

static void test_creator_and_dewrapper(void)
{
    const uint8_t source[] = {
        0x00, 0xFF, 0x13, 0x7A, 0x80
    };

    FileChunk chunk = {
        .data = (uint8_t *)source,
        .length = sizeof(source)
    };

    Packet packet = {0};

    assert(packet_create_data(
        &packet,
        FLAG_DATA,
        42,
        &chunk
    ));

    assert(packet.flags == FLAG_DATA);
    assert(packet.number == 42);
    assert(packet.payload_length == sizeof(source));
    assert(packet.checksum == 0);
    assert(packet.payload != NULL);
    assert(memcmp(packet.payload, source, sizeof(source)) == 0);
    assert(packet.payload != source);

    /* The packet owns an independent copy. */
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
    Packet packet = {0};

    assert(packet_create_control(&packet, FLAG_ACK, 123));

    assert(packet.flags == FLAG_ACK);
    assert(packet.number == 123);
    assert(packet.payload_length == 0);
    assert(packet.checksum == 0);
    assert(packet.payload == NULL);

    assert_valid_round_trip(&packet);
    packet_release_payload(&packet);
}

static void test_empty_payload(void)
{
    FileChunk chunk = {
        .data = NULL,
        .length = 0
    };

    Packet packet = {0};

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

    for (size_t i = 0; i < MAX_PAYLOAD_SIZE; ++i) {
        source[i] = (uint8_t)(i * 37U);
    }

    FileChunk chunk = {
        .data = source,
        .length = MAX_PAYLOAD_SIZE
    };

    Packet packet = {0};

    assert(packet_create_data(
        &packet,
        FLAG_DATA,
        65535,
        &chunk
    ));

    assert(packet.payload_length == MAX_PAYLOAD_SIZE);
    assert(memcmp(packet.payload, source, MAX_PAYLOAD_SIZE) == 0);

    packet_release_payload(&packet);
    free(source);
}

static void test_invalid_creation_arguments(void)
{
    Packet packet = {0};

    FileChunk oversized = {
        .data = (uint8_t *)"x",
        .length = MAX_PAYLOAD_SIZE + 1U
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

    Packet original = {0};

    assert(packet_create_data(
        &original,
        FLAG_DATA,
        0x1234,
        &chunk
    ));

    uint8_t buffer[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
    size_t serialized_size = 0;

    assert(packet_serialize(
        &original,
        buffer,
        sizeof(buffer),
        &serialized_size
    ));

    assert(serialized_size == PACKET_HEADER_SIZE + sizeof(source));

    assert(buffer[0] == FLAG_DATA);
    assert(buffer[1] == 0x12);
    assert(buffer[2] == 0x34);
    assert(buffer[3] == 0x00);
    assert(buffer[4] == sizeof(source));

    assert(memcmp(
        buffer + PACKET_HEADER_SIZE,
        source,
        sizeof(source)
    ) == 0);

    ArqPacketInput received = {0};

    assert(packet_receive(buffer, serialized_size, &received));
    assert(received.valid);

    assert(received.packet.flags == original.flags);
    assert(received.packet.number == original.number);
    assert(received.packet.payload_length == original.payload_length);
    assert(received.packet.checksum == original.checksum);

    assert(memcmp(
        received.packet.payload,
        source,
        sizeof(source)
    ) == 0);

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

    Packet packet = {0};
    assert(packet_create_data(&packet, FLAG_DATA, 1, &chunk));

    uint8_t buffer[PACKET_HEADER_SIZE + sizeof(source) - 1];
    size_t serialized_size = 0;

    assert(!packet_serialize(
        &packet,
        buffer,
        sizeof(buffer),
        &serialized_size
    ));

    packet_release_payload(&packet);
}

static void test_invalid_receive_inputs(void)
{
    uint8_t buffer[PACKET_HEADER_SIZE + 4] = {0};
    ArqPacketInput result = {0};

    assert(!packet_receive(NULL, sizeof(buffer), &result));
    assert(!packet_receive(buffer, sizeof(buffer), NULL));

    assert(packet_receive(
        buffer,
        PACKET_HEADER_SIZE - 1,
        &result
    ));
    assert(!result.valid);

    /* Inconsistent advertised payload size. */
    buffer[3] = 0;
    buffer[4] = 4;

    assert(packet_receive(buffer, PACKET_HEADER_SIZE, &result));
    assert(!result.valid);

    /* Payload larger than the supported maximum. */
    buffer[3] = (uint8_t)((MAX_PAYLOAD_SIZE + 1U) >> 8);
    buffer[4] = (uint8_t)(MAX_PAYLOAD_SIZE + 1U);

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

    Packet packet = {0};
    assert(packet_create_data(&packet, FLAG_DATA, 7, &chunk));

    uint8_t buffer[PACKET_HEADER_SIZE + sizeof(source)];
    size_t serialized_size = 0;

    assert(packet_serialize(
        &packet,
        buffer,
        sizeof(buffer),
        &serialized_size
    ));

    buffer[PACKET_HEADER_SIZE] ^= 0x01;

    ArqPacketInput result = {0};

    assert(packet_receive(buffer, serialized_size, &result));
    assert(!result.valid);

    packet_release_payload(&packet);
}

/* ---------- New handshake metadata tests ---------- */

static void encode_file_size(
    uint8_t *metadata,
    uint64_t file_size)
{
    for (size_t i = 0; i < PACKET_FILE_SIZE_FIELD_SIZE; i++) {
        unsigned shift = (unsigned)(
            (PACKET_FILE_SIZE_FIELD_SIZE - 1U - i) * 8U
        );

        metadata[i] = (uint8_t)(file_size >> shift);
    }
}

static uint64_t decode_file_size(const uint8_t *metadata)
{
    uint64_t file_size = 0;

    for (size_t i = 0; i < PACKET_FILE_SIZE_FIELD_SIZE; i++) {
        file_size = (file_size << 8) | metadata[i];
    }

    return file_size;
}

static void test_syn_window_metadata_round_trip(void)
{
    uint8_t metadata[PACKET_TRANSFER_METADATA_SIZE] = {0};
    uint8_t expected[PACKET_TRANSFER_METADATA_SIZE] = {0};

    const uint64_t expected_file_size =
        UINT64_C(0x0102030405060708);

    encode_file_size(metadata, expected_file_size);

    for (size_t i = 0; i < PACKET_SHA256_DIGEST_SIZE; i++) {
        metadata[PACKET_FILE_SIZE_FIELD_SIZE + i] =
            (uint8_t)(0xA0U + i);
    }

    memcpy(expected, metadata, sizeof(expected));

    Packet syn = {0};

    assert(packet_create_control_with_payload(
        &syn,
        (uint8_t)(
            FLAG_SYN | FLAG_WINDOW | PROTOCOL_SELECTIVE_REPEAT
        ),
        8,
        metadata,
        sizeof(metadata)
    ));

    assert(syn.number == 8);
    assert(syn.payload_length == PACKET_TRANSFER_METADATA_SIZE);
    assert(syn.payload != metadata);
    assert(memcmp(syn.payload, expected, sizeof(expected)) == 0);

    /* Verify that Packet Creator made its own copy. */
    memset(metadata, 0xEE, sizeof(metadata));
    assert(memcmp(syn.payload, expected, sizeof(expected)) == 0);

    assert_valid_round_trip(&syn);

    uint8_t buffer[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
    size_t serialized_size = 0;

    assert(packet_serialize(
        &syn,
        buffer,
        sizeof(buffer),
        &serialized_size
    ));

    ArqPacketInput decoded = {0};

    assert(packet_receive(buffer, serialized_size, &decoded));
    assert(decoded.valid);

    assert(decoded.packet.flags == syn.flags);
    assert(decoded.packet.number == 8);
    assert(decoded.packet.payload_length == sizeof(expected));

    assert(decode_file_size(decoded.packet.payload) == expected_file_size);

    assert(memcmp(
        decoded.packet.payload + PACKET_FILE_SIZE_FIELD_SIZE,
        expected + PACKET_FILE_SIZE_FIELD_SIZE,
        PACKET_SHA256_DIGEST_SIZE
    ) == 0);

    packet_release_payload(&decoded.packet);
    packet_release_payload(&syn);
}

static void test_handshake_responses(void)
{
    Packet accepted = {0};

    assert(packet_create_control(
        &accepted,
        (uint8_t)(
            FLAG_SYN | FLAG_ACK | FLAG_WINDOW | PROTOCOL_GO_BACK_N
        ),
        4
    ));

    assert(accepted.payload == NULL);
    assert(accepted.payload_length == 0);
    assert_valid_round_trip(&accepted);
    packet_release_payload(&accepted);

    Packet rejected = {0};

    assert(packet_create_control(
        &rejected,
        (uint8_t)(
            FLAG_SYN | FLAG_NACK | FLAG_WINDOW | PROTOCOL_GO_BACK_N
        ),
        0
    ));

    assert(rejected.payload == NULL);
    assert(rejected.payload_length == 0);
    assert_valid_round_trip(&rejected);
    packet_release_payload(&rejected);
}

static void test_fin_responses(void)
{
    const uint8_t flags_to_test[] = {
        FLAG_FIN,
        FLAG_FIN | FLAG_ACK,
        FLAG_FIN | FLAG_NACK
    };

    for (size_t i = 0;
         i < sizeof(flags_to_test) / sizeof(flags_to_test[0]);
         i++) {
        Packet packet = {0};

        assert(packet_create_control(
            &packet,
            (uint8_t)(
                flags_to_test[i] | PROTOCOL_STOP_WAIT
            ),
            65535
        ));

        assert(packet.payload == NULL);
        assert(packet.payload_length == 0);

        assert_valid_round_trip(&packet);
        packet_release_payload(&packet);
    }
}

static void test_invalid_control_creation(void)
{
    uint8_t payload[PACKET_TRANSFER_METADATA_SIZE] = {0};

    Packet packet = {0};

    /* Initial SYN must include the full 40-byte metadata payload. */
    assert(!packet_create_control(
        &packet,
        (uint8_t)(FLAG_SYN | FLAG_WINDOW),
        4
    ));

    assert(!packet_create_control_with_payload(
        &packet,
        (uint8_t)(FLAG_SYN | FLAG_WINDOW),
        4,
        payload,
        PACKET_TRANSFER_METADATA_SIZE - 1
    ));

    /* SYN response must not carry a metadata payload. */
    assert(!packet_create_control_with_payload(
        &packet,
        (uint8_t)(FLAG_SYN | FLAG_ACK | FLAG_WINDOW),
        4,
        payload,
        PACKET_TRANSFER_METADATA_SIZE
    ));

    assert(!packet_create_control_with_payload(
        &packet,
        FLAG_ACK,
        4,
        payload,
        1
    ));

    /* A control constructor must not be used to create DATA. */
    assert(!packet_create_control(
        &packet,
        FLAG_DATA,
        0
    ));

    FileChunk chunk = {
        .data = payload,
        .length = 1
    };

    /* A DATA constructor must not create ACK packets. */
    assert(!packet_create_data(
        &packet,
        FLAG_ACK,
        0,
        &chunk
    ));

    /* Conflicting packet types must be rejected. */
    assert(!packet_create_control(
        &packet,
        FLAG_ACK | FLAG_NACK,
        0
    ));

    assert(!packet_create_control(
        &packet,
        FLAG_DATA | FLAG_ACK,
        0
    ));

    /* The reserved protocol identifier must be rejected. */
    assert(!packet_create_control(
        &packet,
        (uint8_t)(FLAG_ACK | PROTOCOL_RESERVED),
        0
    ));
}

/* ---------- New receive-side semantic validation tests ---------- */

static void assert_wire_packet_invalid(
    uint8_t flags,
    PacketNumber number,
    const uint8_t *payload,
    size_t payload_length)
{
    uint8_t buffer[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];

    size_t size = make_wire_packet(
        flags,
        number,
        payload,
        payload_length,
        buffer,
        sizeof(buffer)
    );

    ArqPacketInput result = {0};

    /*
     * The checksum is valid. Rejection must therefore be due
     * to the packet's semantics or flag/payload combination.
     */
    assert(packet_receive(buffer, size, &result));
    assert(!result.valid);
    assert(result.packet.payload == NULL);
}

static void test_invalid_flag_combinations_on_receive(void)
{
    uint8_t metadata[PACKET_TRANSFER_METADATA_SIZE] = {0};
    uint8_t byte = 0x42;

    assert_wire_packet_invalid(
        (uint8_t)(FLAG_DATA | FLAG_ACK | PROTOCOL_STOP_WAIT),
        1,
        NULL,
        0
    );

    assert_wire_packet_invalid(
        (uint8_t)(FLAG_ACK | FLAG_NACK | PROTOCOL_STOP_WAIT),
        1,
        NULL,
        0
    );

    assert_wire_packet_invalid(
        (uint8_t)(FLAG_ACK | PROTOCOL_RESERVED),
        1,
        NULL,
        0
    );

    /* ACK may not carry data payload. */
    assert_wire_packet_invalid(
        (uint8_t)(FLAG_ACK | PROTOCOL_STOP_WAIT),
        1,
        &byte,
        1
    );

    /* Metadata SYN must have exactly 40 bytes. */
    assert_wire_packet_invalid(
        (uint8_t)(FLAG_SYN | FLAG_WINDOW | PROTOCOL_STOP_WAIT),
        4,
        metadata,
        PACKET_TRANSFER_METADATA_SIZE - 1
    );

    /* Accepted SYN response must not carry metadata. */
    assert_wire_packet_invalid(
        (uint8_t)(
            FLAG_SYN | FLAG_ACK | FLAG_WINDOW | PROTOCOL_STOP_WAIT
        ),
        4,
        metadata,
        PACKET_TRANSFER_METADATA_SIZE
    );
}

static void test_invalid_dewrap_type(void)
{
    Packet ack = {0};

    assert(packet_create_control(&ack, FLAG_ACK, 0));

    FileChunk extracted = {0};

    assert(!packet_dewrap(&ack, &extracted));

    packet_release_payload(&ack);
}

/* ---------- Entry point ---------- */

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

    test_syn_window_metadata_round_trip();
    test_handshake_responses();
    test_fin_responses();
    test_invalid_control_creation();

    test_invalid_flag_combinations_on_receive();
    test_invalid_dewrap_type();

    puts("All original and extended packet tests passed.");
    return 0;
}