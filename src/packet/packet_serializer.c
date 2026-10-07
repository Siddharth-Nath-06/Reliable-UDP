#include "packet.h"
#include "packet_internal.h"
#include "../common/constants.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

bool packet_serialize(Packet *packet,
                      uint8_t *buffer,
                      size_t buffer_size,
                      size_t *serialized_size)
{
    if (packet == NULL || buffer == NULL || serialized_size == NULL)
        return false;

    if (packet->payload_length > MAX_PAYLOAD_SIZE)
        return false;

    if (packet->payload_length > 0 && packet->payload == NULL)
        return false;

    size_t total_size = PACKET_HEADER_SIZE + packet->payload_length;

    if (buffer_size < total_size)
        return false;

    buffer[0] = packet->flags;

    uint16_t number = htons(packet->number);
    uint16_t payload_length = htons(packet->payload_length);

    memcpy(buffer + 1, &number, sizeof(number));
    memcpy(buffer + 3, &payload_length, sizeof(payload_length));

    buffer[5] = 0;
    buffer[6] = 0;

    if (packet->payload_length > 0)
        memcpy(buffer + PACKET_HEADER_SIZE,
               packet->payload,
               packet->payload_length);

    packet->checksum = ~packet_calculate_checksum(buffer, total_size);

    uint16_t checksum = htons(packet->checksum);

    memcpy(buffer + 5, &checksum, sizeof(checksum));

    *serialized_size = total_size;

    return true;
}

bool packet_deserialize(const uint8_t *buffer,
                        size_t buffer_size,
                        Packet *packet)
{
    if (buffer == NULL || packet == NULL)
        return false;

    if (buffer_size < PACKET_HEADER_SIZE)
        return false;

    uint16_t number;
    uint16_t payload_length;
    uint16_t checksum;

    memcpy(&number, buffer + 1, sizeof(number));
    memcpy(&payload_length, buffer + 3, sizeof(payload_length));
    memcpy(&checksum, buffer + 5, sizeof(checksum));

    payload_length = ntohs(payload_length);

    if (payload_length > MAX_PAYLOAD_SIZE)
        return false;

    size_t expected_size = PACKET_HEADER_SIZE + payload_length;

    if (buffer_size != expected_size)
        return false;

    packet->flags = buffer[0];
    packet->number = ntohs(number);
    packet->payload_length = payload_length;
    packet->checksum = ntohs(checksum);
    packet->payload = NULL;

    if (payload_length > 0) {
        packet->payload = malloc(payload_length);

        if (packet->payload == NULL)
            return false;

        memcpy(packet->payload,
               buffer + PACKET_HEADER_SIZE,
               payload_length);
    }

    return true;
}