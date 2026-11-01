#ifndef PACKET_H
#define PACKET_H

#include "../common/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Transfer metadata carried by the initial SYN | WINDOW packet:
 *   8 bytes: file size, unsigned 64-bit, network byte order
 *  32 bytes: raw SHA-256 digest
 */
#define PACKET_FILE_SIZE_FIELD_SIZE 8U
#define PACKET_SHA256_DIGEST_SIZE 32U

#define PACKET_TRANSFER_METADATA_SIZE \
    (PACKET_FILE_SIZE_FIELD_SIZE + PACKET_SHA256_DIGEST_SIZE)

/*
 * Construct a DATA packet by copying the supplied FileChunk.
 *
 * The caller supplies the sequence number and protocol flags.
 * The destination Packet must be empty or have its old payload
 * released before calling this function.
 *
 * On failure, packet is left unchanged.
 */
bool packet_create_data(
    Packet *packet,
    uint8_t flags,
    PacketNumber number,
    const FileChunk *chunk
);

/*
 * Construct a control packet without a payload.
 *
 * Intended for ACK, NACK, FIN, FIN | ACK, FIN | NACK,
 * and other permitted payload-free control combinations.
 *
 * SYN | WINDOW requires the payload-bearing constructor instead.
 */
bool packet_create_control(
    Packet *packet,
    uint8_t flags,
    PacketNumber number
);

/*
 * Construct a control packet with a copied payload.
 *
 * This supports SYN | WINDOW metadata and other permitted
 * payload-bearing control packets.
 *
 * payload is borrowed. The Packet owns an independent copy.
 * The destination Packet must be empty or previously released.
 * On failure, packet is left unchanged.
 */
bool packet_create_control_with_payload(
    Packet *packet,
    uint8_t flags,
    PacketNumber number,
    const uint8_t *payload,
    size_t payload_length
);

/*
 * Serialize a valid logical packet into the wire format.
 *
 * The serialized output contains the seven-byte header followed
 * by the payload. Multi-byte fields use network byte order.
 *
 * The packet's checksum field is updated on success.
 */
bool packet_serialize(
    Packet *packet,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *serialized_size
);

/*
 * Validate and decode a received datagram.
 *
 * Returns false if an internal operation fails, such as memory
 * allocation failure.
 *
 * Returns true if the datagram was examined, even if it is
 * malformed or its checksum/flags are invalid. In that case,
 * result->valid is false.
 *
 * Initialize result to {0}, or release its previous packet payload
 * before reusing it.
 */
bool packet_receive(
    const uint8_t *buffer,
    size_t buffer_size,
    ArqPacketInput *result
);

/*
 * Extract the payload from a valid DATA packet.
 *
 * The caller owns the resulting FileChunk.data and must free it.
 * The output chunk must be empty before calling this function.
 * On failure, chunk is left unchanged.
 */
bool packet_dewrap(
    const Packet *packet,
    FileChunk *chunk
);

/*
 * Release the packet's payload allocation.
 *
 * Other packet fields remain unchanged. The payload pointer,
 * payload length, and checksum are reset.
 */
void packet_release_payload(Packet *packet);

#endif