
#ifndef RECONSTRUCTOR_H
#define RECONSTRUCTOR_H

#include <stdbool.h>
#include <stdio.h>

#include "../common/types.h"
#include "../file/sha256.h"

typedef struct {
    FILE *file;
    SHA256Context hash;
    bool active;
    bool failed;
} Reconstructor;

/*
 * Open the destination file for reconstruction.
 * Returns 0 on success, -1 on error.
 */
int reconstructor_open(
    Reconstructor *reconstructor,
    const char *destination_path
);

/*
 * Write one ordered chunk and update the running SHA-256 hash.
 * Returns 0 on success, -1 on error.
 */
int reconstructor_write_chunk(
    Reconstructor *reconstructor,
    const FileChunk *chunk
);

/*
 * Finish reconstruction and verify the complete file hash.
 * Returns:
 *   0  = hash matches
 *   1  = hash mismatch
 *  -1  = file or hashing error
 *
 * actual_digest may be NULL if the caller does not need the result.
 * The destination file remains on disk even if verification fails.
 */
int reconstructor_finish(
    Reconstructor *reconstructor,
    const SHA256Digest *expected_digest,
    SHA256Digest *actual_digest
);

/*
 * Close an unfinished reconstruction and release its file handle.
 */
void reconstructor_abort(Reconstructor *reconstructor);

#endif
