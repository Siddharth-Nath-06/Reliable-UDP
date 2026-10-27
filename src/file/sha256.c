#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotate_right(uint32_t value, uint32_t amount)
{
    return (value >> amount) | (value << (32 - amount));
}

static uint32_t choose(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static uint32_t majority(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t big_sigma0(uint32_t x)
{
    return rotate_right(x, 2) ^
           rotate_right(x, 13) ^
           rotate_right(x, 22);
}

static uint32_t big_sigma1(uint32_t x)
{
    return rotate_right(x, 6) ^
           rotate_right(x, 11) ^
           rotate_right(x, 25);
}

static uint32_t small_sigma0(uint32_t x)
{
    return rotate_right(x, 7) ^
           rotate_right(x, 18) ^
           (x >> 3);
}

static uint32_t small_sigma1(uint32_t x)
{
    return rotate_right(x, 17) ^
           rotate_right(x, 19) ^
           (x >> 10);
}

static void process_block(
    uint32_t state[8],
    const uint8_t block[64]
)
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++)
    {
        w[i] =
            ((uint32_t)block[i * 4] << 24) |
            ((uint32_t)block[i * 4 + 1] << 16) |
            ((uint32_t)block[i * 4 + 2] << 8) |
            ((uint32_t)block[i * 4 + 3]);
    }

    for (int i = 16; i < 64; i++)
    {
        w[i] =
            small_sigma1(w[i - 2]) +
            w[i - 7] +
            small_sigma0(w[i - 15]) +
            w[i - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; i++)
    {
        uint32_t temp1 =
            h +
            big_sigma1(e) +
            choose(e, f, g) +
            K[i] +
            w[i];

        uint32_t temp2 =
            big_sigma0(a) +
            majority(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(SHA256Context *context)
{
    if (context == NULL)
        return;

    context->state[0] = 0x6a09e667;
    context->state[1] = 0xbb67ae85;
    context->state[2] = 0x3c6ef372;
    context->state[3] = 0xa54ff53a;
    context->state[4] = 0x510e527f;
    context->state[5] = 0x9b05688c;
    context->state[6] = 0x1f83d9ab;
    context->state[7] = 0x5be0cd19;

    context->bit_length = 0;
    context->buffer_length = 0;
}

int sha256_update(
    SHA256Context *context,
    const uint8_t *data,
    size_t length
)
{
    if (context == NULL)
        return -1;

    if (data == NULL && length != 0)
        return -1;

    context->bit_length += (uint64_t)length * 8;

    size_t offset = 0;

    while (offset < length)
    {
        size_t space = 64 - context->buffer_length;
        size_t copy_length = length - offset;

        if (copy_length > space)
            copy_length = space;

        memcpy(
            context->buffer + context->buffer_length,
            data + offset,
            copy_length
        );

        context->buffer_length += copy_length;
        offset += copy_length;

        if (context->buffer_length == 64)
        {
            process_block(context->state, context->buffer);
            context->buffer_length = 0;
        }
    }

    return 0;
}

int sha256_final(
    SHA256Context *context,
    SHA256Digest *digest
)
{
    if (context == NULL || digest == NULL)
        return -1;

    size_t original_length = context->buffer_length;

    context->buffer[original_length] = 0x80;
    context->buffer_length++;

    if (context->buffer_length > 56)
    {
        while (context->buffer_length < 64)
        {
            context->buffer[context->buffer_length] = 0;
            context->buffer_length++;
        }

        process_block(context->state, context->buffer);
        context->buffer_length = 0;
    }

    while (context->buffer_length < 56)
    {
        context->buffer[context->buffer_length] = 0;
        context->buffer_length++;
    }

    for (int i = 0; i < 8; i++)
    {
        context->buffer[56 + i] =
            (uint8_t)(context->bit_length >> (56 - i * 8));
    }

    process_block(context->state, context->buffer);

    for (int i = 0; i < 8; i++)
    {
        digest->data[i * 4] =
            (uint8_t)(context->state[i] >> 24);

        digest->data[i * 4 + 1] =
            (uint8_t)(context->state[i] >> 16);

        digest->data[i * 4 + 2] =
            (uint8_t)(context->state[i] >> 8);

        digest->data[i * 4 + 3] =
            (uint8_t)context->state[i];
    }

    return 0;
}

int sha256_compute(
    const uint8_t *data,
    size_t length,
    SHA256Digest *digest
)
{
    if (digest == NULL)
        return -1;

    if (data == NULL && length != 0)
        return -1;

    SHA256Context context;

    sha256_init(&context);

    if (sha256_update(
            &context,
            data,
            length) != 0)
    {
        return -1;
    }

    return sha256_final(&context, digest);
}