#include "packet_internal.h"

#include <stddef.h>
#include <stdint.h>

uint16_t packet_calculate_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;

    while (length > 1) {
        uint16_t word = ((uint16_t)data[0] << 8) | data[1];

        sum += word;
        sum = (sum & 0xFFFF) + (sum >> 16);

        data += 2;
        length -= 2;
    }

    if (length == 1) {
        sum += (uint16_t)data[0] << 8;
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)sum;
}