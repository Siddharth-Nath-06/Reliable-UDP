#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t buffer[64];
    size_t buffer_length;
} SHA256Context;

typedef struct {
    uint8_t data[SHA256_DIGEST_SIZE];
} SHA256Digest;

/*
 * Initialize a SHA-256 context.
 */
void sha256_init(SHA256Context *context);

/*
 * Add data to the SHA-256 calculation.
 *
 * Returns:
 *   0  on success
 *  -1  on invalid arguments
 */
int sha256_update(
    SHA256Context *context,
    const uint8_t *data,
    size_t length
);

/*
 * Finish the SHA-256 calculation.
 */
int sha256_final(
    SHA256Context *context,
    SHA256Digest *digest
);

/*
 * Calculate SHA-256 hash of a memory buffer.
 *
 * Returns:
 *   0  on success
 *  -1  on invalid arguments
 */
int sha256_compute(
    const uint8_t *data,
    size_t length,
    SHA256Digest *digest
);

#endif