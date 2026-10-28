#ifndef PACKET_H
#define PACKET_H

#include "../common/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool packet_create_data(Packet *packet,
                        uint8_t flags,
                        PacketNumber number,
                        const FileChunk *chunk);

bool packet_create_control(Packet *packet,
                           uint8_t flags,
                           PacketNumber number);

bool packet_serialize(Packet *packet,
                      uint8_t *buffer,
                      size_t buffer_size,
                      size_t *serialized_size);

bool packet_receive(const uint8_t *buffer,
                    size_t buffer_size,
                    ArqPacketInput *result);

bool packet_dewrap(const Packet *packet,
                   FileChunk *chunk);

void packet_release_payload(Packet *packet);

#endif