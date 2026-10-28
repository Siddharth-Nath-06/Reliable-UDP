#include "packet.h"
#include <string.h>
#include "packet_internal.h"
#include "../common/constants.h"
#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <arpa/inet.h>
#endif

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

    // Checksum reverification
    uint8_t *copy = malloc(buffer_size);
    if (copy == NULL)
        return false;

    memcpy(copy, buffer, buffer_size);

    uint16_t received_checksum =
        ((uint16_t)copy[5] << 8) | copy[6];

    copy[5] = 0;
    copy[6] = 0;

    uint16_t calculated_checksum =
        (uint16_t)~packet_calculate_checksum(copy, buffer_size);

    free(copy);

    if (calculated_checksum != received_checksum)
        return true;  // Malformed/corrupt packet; result remains invalid

    Packet packet;

    if (!packet_deserialize(buffer, buffer_size, &packet))
        return false;

    result->valid = true;
    result->packet = packet;

    return true;
}