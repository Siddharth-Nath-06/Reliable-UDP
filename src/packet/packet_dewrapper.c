#include "packet.h"
#include "packet_internal.h"

#include "../common/constants.h"

#include <stdlib.h>
#include <string.h>

bool packet_dewrap(
    const Packet *packet,
    FileChunk *chunk)
{
    if (packet == NULL || chunk == NULL) {
        return false;
    }

    /*
     * Validate packet structure, protocol identifier,
     * flag combination, payload length, and payload pointer.
     *
     * Checksum verification is performed earlier by packet_receive()
     * for network-received packets.
     */
    if (!packet_validate_fields(packet)) {
        return false;
    }

    uint8_t type = (uint8_t)(
        packet->flags & (uint8_t)~FLAG_PROTOCOL_MASK
    );

    /* Only DATA packets can be unwrapped as file data. */
    if (type != FLAG_DATA) {
        return false;
    }

    /*
     * Construct the result locally so allocation failure leaves
     * the caller's output chunk unchanged.
     */
    FileChunk extracted = {0};

    extracted.length = packet->payload_length;

    if (packet->payload_length > 0) {
        extracted.data = malloc(packet->payload_length);

        if (extracted.data == NULL) {
            return false;
        }

        memcpy(
            extracted.data,
            packet->payload,
            packet->payload_length
        );
    }

    /*
     * Ownership of extracted.data passes to the caller.
     * The caller must eventually free(chunk->data).
     *
     * The caller must supply an empty output chunk.
     */
    *chunk = extracted;

    return true;
}