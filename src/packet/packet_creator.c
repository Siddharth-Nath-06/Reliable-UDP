#include "packet.h"
#include "packet_internal.h"

#include "../common/constants.h"

#include <stdlib.h>
#include <string.h>

bool packet_create_data(
    Packet *packet,
    uint8_t flags,
    PacketNumber number,
    const FileChunk *chunk)
{
    if (packet == NULL || chunk == NULL) {
        return false;
    }

    if (chunk->length > MAX_PAYLOAD_SIZE ||
        (chunk->length > 0 && chunk->data == NULL)) {
        return false;
    }

    Packet created = {0};

    created.flags = flags;
    created.number = number;
    created.payload_length = (PayloadLength)chunk->length;
    created.checksum = 0;

    if (chunk->length > 0) {
        created.payload = malloc(chunk->length);

        if (created.payload == NULL) {
            return false;
        }

        memcpy(created.payload, chunk->data, chunk->length);
    }

    if (!packet_validate_fields(&created)) {
        free(created.payload);
        return false;
    }

    *packet = created;
    return true;
}

bool packet_create_control_with_payload(
    Packet *packet,
    uint8_t flags,
    PacketNumber number,
    const uint8_t *payload,
    size_t payload_length)
{
    if (packet == NULL ||
        payload_length > MAX_PAYLOAD_SIZE ||
        (payload_length > 0 && payload == NULL)) {
        return false;
    }

    Packet created = {0};

    created.flags = flags;
    created.number = number;
    created.payload_length = (PayloadLength)payload_length;
    created.checksum = 0;

    if (payload_length > 0) {
        created.payload = malloc(payload_length);

        if (created.payload == NULL) {
            return false;
        }

        memcpy(created.payload, payload, payload_length);
    }

    if (!packet_validate_fields(&created)) {
        free(created.payload);
        return false;
    }

    *packet = created;
    return true;
}

bool packet_create_control(
    Packet *packet,
    uint8_t flags,
    PacketNumber number)
{
    return packet_create_control_with_payload(
        packet,
        flags,
        number,
        NULL,
        0
    );
}

void packet_release_payload(Packet *packet)
{
    if (packet == NULL) {
        return;
    }

    free(packet->payload);

    packet->payload = NULL;
    packet->payload_length = 0;
    packet->checksum = 0;
}