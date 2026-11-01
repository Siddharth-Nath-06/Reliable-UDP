#include "packet.h"
#include "packet_internal.h"

#include "../common/constants.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

bool packet_validate_fields(const Packet *packet)
{
    if (packet == NULL ||
        packet->payload_length > MAX_PAYLOAD_SIZE) {
        return false;
    }

    if (packet->payload_length > 0 && packet->payload == NULL) {
        return false;
    }

    if (packet->payload_length == 0 && packet->payload != NULL) {
        return false;
    }

    uint8_t protocol =
        (uint8_t)(packet->flags & FLAG_PROTOCOL_MASK);

    if (protocol == PROTOCOL_RESERVED) {
        return false;
    }

    uint8_t type = (uint8_t)(
        packet->flags & (uint8_t)~FLAG_PROTOCOL_MASK
    );

    switch (type) {
        case FLAG_DATA:
            /*
             * DATA may contain any permitted payload size,
             * including zero bytes.
             */
            return true;

        case FLAG_ACK:
        case FLAG_NACK:
        case FLAG_FIN:
        case FLAG_FIN | FLAG_ACK:
        case FLAG_FIN | FLAG_NACK:
            return packet->payload_length == 0;

        case FLAG_SYN | FLAG_WINDOW:
            /*
             * Initial handshake:
             * number = proposed window size
             * payload = file size (8 bytes) + SHA-256 digest (32 bytes)
             */
            return packet->number != 0 &&
                   packet->payload_length ==
                       PACKET_TRANSFER_METADATA_SIZE;

        case FLAG_SYN | FLAG_ACK | FLAG_WINDOW:
            /*
             * Accepted handshake:
             * number = accepted window size; no payload.
             */
            return packet->number != 0 &&
                   packet->payload_length == 0;

        case FLAG_SYN | FLAG_NACK | FLAG_WINDOW:
            /*
             * Rejected handshake; no payload.
             * The number field is not interpreted as an accepted window.
             */
            return packet->payload_length == 0;

        default:
            /*
             * Reject unsupported combinations, including DATA mixed
             * with control flags and simultaneous ACK plus NACK.
             */
            return false;
    }
}

bool packet_receive(
    const uint8_t *buffer,
    size_t buffer_size,
    ArqPacketInput *result)
{
    if (buffer == NULL || result == NULL) {
        return false;
    }

    /*
     * Contract: the caller must have initialized result to {0}
     * or released its previous packet payload before reusing it.
     */
    result->valid = false;
    memset(&result->packet, 0, sizeof(result->packet));

    if (buffer_size < PACKET_HEADER_SIZE) {
        return true;
    }

    uint16_t length_network;

    memcpy(&length_network, buffer + 3, sizeof(length_network));

    uint16_t payload_length = ntohs(length_network);

    if (payload_length > MAX_PAYLOAD_SIZE) {
        return true;
    }

    size_t expected_size =
        PACKET_HEADER_SIZE + (size_t)payload_length;

    if (buffer_size != expected_size) {
        return true;
    }

    /*
     * Verify the checksum against a copy with the checksum field
     * cleared. A checksum mismatch marks the packet invalid.
     */
    uint8_t *copy = malloc(buffer_size);

    if (copy == NULL) {
        return false;
    }

    memcpy(copy, buffer, buffer_size);

    uint16_t received_checksum =
        (uint16_t)(((uint16_t)copy[5] << 8) | copy[6]);

    copy[5] = 0;
    copy[6] = 0;

    uint16_t calculated_checksum =
        (uint16_t)~packet_calculate_checksum(copy, buffer_size);

    free(copy);

    if (calculated_checksum != received_checksum) {
        return true;
    }

    Packet decoded = {0};

    if (!packet_deserialize(buffer, buffer_size, &decoded)) {
        /*
         * A length-valid packet can still fail decoding, e.g. due
         * to an allocation failure. Report an internal failure.
         */
        return false;
    }

    if (!packet_validate_fields(&decoded)) {
        packet_release_payload(&decoded);
        return true;
    }

    result->packet = decoded;
    result->valid = true;

    return true;
}