#include "packet.h"
#include "packet_internal.h"
#include "../common/constants.h"

bool packet_receive(const uint8_t *buffer,
                    size_t buffer_size,
                    ArqPacketInput *result)
{
    if (buffer == NULL || result == NULL)
        return false;

    result->valid = false;
    result->packet.payload = NULL;

    if (buffer_size < PACKET_HEADER_SIZE)
        return true;

    uint16_t payload_length;

    memcpy(&payload_length, buffer + 3, sizeof(payload_length));
    payload_length = ntohs(payload_length);

    if (payload_length > MAX_PAYLOAD_SIZE)
        return true;

    size_t expected_size = PACKET_HEADER_SIZE + payload_length;

    if (buffer_size != expected_size)
        return true;

    if (packet_calculate_checksum(buffer, buffer_size) != 0xFFFF)
        return true;

    Packet packet;

    if (!packet_deserialize(buffer, buffer_size, &packet))
        return false;

    result->valid = true;
    result->packet = packet;

    return true;
}