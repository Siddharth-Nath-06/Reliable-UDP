#ifndef PACKET_INTERNAL_H
#define PACKET_INTERNAL_H

#include "packet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Calculate the existing packet checksum over a byte sequence.
 * The checksum field must be zeroed by the caller when calculating
 * a checksum for a packet being serialized or verified.
 */
uint16_t packet_calculate_checksum(
    const uint8_t *data,
    size_t length
);

/*
 * Decode a structurally well-formed wire-format packet.
 *
 * This function checks header and payload length consistency.
 * Semantic flag validation is handled separately by
 * packet_validate_fields().
 *
 * packet must be empty or previously released.
 */
bool packet_deserialize(
    const uint8_t *buffer,
    size_t buffer_size,
    Packet *packet
);

/*
 * Validate protocol bits, packet-type flag combinations,
 * payload size, and type-specific payload requirements.
 */
bool packet_validate_fields(const Packet *packet);

#endif