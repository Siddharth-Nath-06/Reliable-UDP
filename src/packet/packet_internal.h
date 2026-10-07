#ifndef PACKET_INTERNAL_H
#define PACKET_INTERNAL_H

#include "packet.h"

#include <stddef.h>
#include <stdint.h>

uint16_t packet_calculate_checksum(const uint8_t *data, size_t length);

bool packet_deserialize(const uint8_t *buffer,
                        size_t buffer_size,
                        Packet *packet);

#endif