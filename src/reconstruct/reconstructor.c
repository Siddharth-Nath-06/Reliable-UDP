
#include "reconstructor.h"

#include <string.h>

int reconstructor_open(
    Reconstructor *reconstructor,
    const char *destination_path
)
{
    if (reconstructor == NULL || destination_path == NULL)
    {
        return -1;
    }

    memset(reconstructor, 0, sizeof(*reconstructor));

    reconstructor->file = fopen(destination_path, "wb");

    if (reconstructor->file == NULL)
    {
        reconstructor->failed = true;
        return -1;
    }

    sha256_init(&reconstructor->hash);
    reconstructor->active = true;

    return 0;
}

int reconstructor_write_chunk(
    Reconstructor *reconstructor,
    const FileChunk *chunk
)
{
    if (reconstructor == NULL ||
        chunk == NULL ||
        !reconstructor->active ||
        reconstructor->failed)
    {
        return -1;
    }

    if (chunk->data == NULL && chunk->length != 0)
    {
        reconstructor->failed = true;
        return -1;
    }

    if (chunk->length != 0)
    {
        size_t written = fwrite(
            chunk->data,
            1,
            chunk->length,
            reconstructor->file
        );

        if (written != chunk->length)
        {
            reconstructor->failed = true;
            return -1;
        }
    }

    if (sha256_update(
            &reconstructor->hash,
            chunk->data,
            chunk->length) != 0)
    {
        reconstructor->failed = true;
        return -1;
    }

    return 0;
}

int reconstructor_finish(
    Reconstructor *reconstructor,
    const SHA256Digest *expected_digest,
    SHA256Digest *actual_digest
)
{
    if (reconstructor == NULL ||
        expected_digest == NULL ||
        !reconstructor->active ||
        reconstructor->file == NULL)
    {
        return -1;
    }

    bool write_failed = reconstructor->failed;

    int close_result = fclose(reconstructor->file);

    reconstructor->file = NULL;
    reconstructor->active = false;

    if (write_failed || close_result != 0)
    {
        reconstructor->failed = true;
        return -1;
    }

    SHA256Digest calculated_digest;

    if (sha256_final(
            &reconstructor->hash,
            &calculated_digest) != 0)
    {
        reconstructor->failed = true;
        return -1;
    }

    if (actual_digest != NULL)
    {
        *actual_digest = calculated_digest;
    }

    if (memcmp(
            calculated_digest.data,
            expected_digest->data,
            SHA256_DIGEST_SIZE) != 0)
    {
        reconstructor->failed = true;
        return 1;
    }

    return 0;
}

void reconstructor_abort(Reconstructor *reconstructor)
{
    if (reconstructor == NULL)
    {
        return;
    }

    if (reconstructor->file != NULL)
    {
        fclose(reconstructor->file);
        reconstructor->file = NULL;
    }

    reconstructor->active = false;
    reconstructor->failed = true;
}
