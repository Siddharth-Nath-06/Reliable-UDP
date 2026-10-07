#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "../common/types.h"

/*
 * File Manager
 *
 * Responsible for:
 * - Opening source files
 * - Reading file chunks
 * - Tracking file size
 * - Providing source-file integrity information
 *
 * It does NOT:
 * - Assign packet sequence numbers
 * - Create packets
 * - Perform ARQ
 * - Communicate over UDP
 */

typedef struct {
    void *file;
    uint64_t file_size;
    uint64_t bytes_read;
    size_t chunk_size;
} FileManager;

/*
 * Open a source file for reading.
 *
 * chunk_size must be greater than 0 and
 * must not exceed MAX_PAYLOAD_SIZE.
 *
 * Returns:
 *   0  on success
 *  -1  on failure
 */
int file_manager_open(
    FileManager *fm,
    const char *filename,
    size_t chunk_size
);

/*
 * Read the next FileChunk from the source file.
 *
 * Returns:
 *   1  if a chunk was successfully read
 *   0  if there are no more chunks
 *  -1  on error
 */
int file_manager_read_chunk(
    FileManager *fm,
    FileChunk *chunk
);

/*
 * Close the source file.
 */
void file_manager_close(FileManager *fm);

/*
 * Return the total source-file size in bytes.
 */
uint64_t file_manager_get_size(
    const FileManager *fm
);

#endif