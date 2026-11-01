#include "packet.h"
#include "packet_internal.h"

#include "../common/constants.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <stdlib.h>
#include <string.h>

bool packet_serialize(
    Packet *packet,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *serialized_size)
{
    if (packet == NULL ||
        buffer == NULL ||
        serialized_size == NULL) {
        return false;
    }

    if (!packet_validate_fields(packet)) {
        return false;
    }

    size_t total_size =
        PACKET_HEADER_SIZE + (size_t)packet->payload_length;

    if (buffer_size < total_size) {
        return false;
    }

    buffer[0] = packet->flags;

    uint16_t number = htons(packet->number);
    uint16_t payload_length = htons(packet->payload_length);

    memcpy(buffer + 1, &number, sizeof(number));
    memcpy(buffer + 3, &payload_length, sizeof(payload_length));

    /* Checksum is zero while calculating the checksum. */
    buffer[5] = 0;
    buffer[6] = 0;

    if (packet->payload_length > 0) {
        memcpy(
            buffer + PACKET_HEADER_SIZE,
            packet->payload,
            packet->payload_length
        );
    }

    packet->checksum =
        (uint16_t)~packet_calculate_checksum(buffer, total_size);

    uint16_t checksum = htons(packet->checksum);

    memcpy(buffer + 5, &checksum, sizeof(checksum));

    *serialized_size = total_size;
    return true;
}

bool packet_deserialize(
    const uint8_t *buffer,
    size_t buffer_size,
    Packet *packet)
{
    if (buffer == NULL || packet == NULL) {
        return false;
    }

    if (buffer_size < PACKET_HEADER_SIZE) {
        return false;
    }

    uint16_t number_network;
    uint16_t length_network;
    uint16_t checksum_network;

    memcpy(&number_network, buffer + 1, sizeof(number_network));
    memcpy(&length_network, buffer + 3, sizeof(length_network));
    memcpy(&checksum_network, buffer + 5, sizeof(checksum_network));

    uint16_t payload_length = ntohs(length_network);

    if (payload_length > MAX_PAYLOAD_SIZE) {
        return false;
    }

    size_t expected_size =
        PACKET_HEADER_SIZE + (size_t)payload_length;

    if (buffer_size != expected_size) {
        return false;
    }

    Packet decoded = {0};

    decoded.flags = buffer[0];
    decoded.number = ntohs(number_network);
    decoded.payload_length = payload_length;
    decoded.checksum = ntohs(checksum_network);

    if (payload_length > 0) {
        decoded.payload = malloc(payload_length);

        if (decoded.payload == NULL) {
            return false;
        }

        memcpy(
            decoded.payload,
            buffer + PACKET_HEADER_SIZE,
            payload_length
        );
    }

    *packet = decoded;
    return true;
}