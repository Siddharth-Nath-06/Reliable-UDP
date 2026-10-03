#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint16_t PacketNumber;
typedef uint16_t PayloadLength;
typedef uint16_t Checksum;
typedef uint32_t TimerId;

typedef struct {
    uint8_t *data;
    size_t length;
} FileChunk;

typedef struct {
    uint8_t flags;
    PacketNumber number;
    PayloadLength payload_length;
    Checksum checksum;
    uint8_t *payload;
} Packet;

typedef struct {
    bool valid;
    Packet packet;
} ArqPacketInput;

typedef enum {
    ARQ_STOP_WAIT,
    ARQ_GO_BACK_N,
    ARQ_SELECTIVE_REPEAT
} ArqProtocol;

#endif