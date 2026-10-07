#include "packet.h"
#include "../common/constants.h"

#include <stdlib.h>
#include <string.h>

bool packet_create_data(Packet *packet,
                        uint8_t flags,
                        PacketNumber number,
                        const FileChunk *chunk)
{
    if (packet == NULL || chunk == NULL)
        return false;

    if (chunk->length > MAX_PAYLOAD_SIZE)
        return false;

    if (chunk->length > 0 && chunk->data == NULL)
        return false;

    packet->flags = flags;
    packet->number = number;
    packet->payload_length = (PayloadLength)chunk->length;
    packet->checksum = 0;
    packet->payload = NULL;

    if (chunk->length > 0) {
        packet->payload = malloc(chunk->length);

        if (packet->payload == NULL)
            return false;

        memcpy(packet->payload, chunk->data, chunk->length);
    }

    return true;
}

bool packet_create_control(Packet *packet,
                           uint8_t flags,
                           PacketNumber number)
{
    if (packet == NULL)
        return false;

    packet->flags = flags;
    packet->number = number;
    packet->payload_length = 0;
    packet->checksum = 0;
    packet->payload = NULL;

    return true;
}

void packet_release_payload(Packet *packet)
{
    if (packet == NULL)
        return;

    free(packet->payload);

    packet->payload = NULL;
    packet->payload_length = 0;
    packet->checksum = 0;
}