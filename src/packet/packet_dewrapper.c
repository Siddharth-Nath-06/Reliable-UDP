#include "packet.h"

#include <stdlib.h>
#include <string.h>

bool packet_dewrap(const Packet *packet,
                   FileChunk *chunk)
{
    if (packet == NULL || chunk == NULL)
        return false;

    if (packet->payload_length > 0 && packet->payload == NULL)
        return false;

    chunk->data = NULL;
    chunk->length = packet->payload_length;

    if (packet->payload_length > 0) {
        chunk->data = malloc(packet->payload_length);

        if (chunk->data == NULL) {
            chunk->length = 0;
            return false;
        }

        memcpy(chunk->data,
               packet->payload,
               packet->payload_length);
    }

    return true;
}