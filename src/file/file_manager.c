#include "file_manager.h"

#include <stdio.h>
#include <stdlib.h>

#include "../common/constants.h"

int file_manager_open(
    FileManager *fm,
    const char *filename,
    size_t chunk_size
)
{
    if (fm == NULL || filename == NULL)
        return -1;

    if (chunk_size == 0 || chunk_size > MAX_PAYLOAD_SIZE)
        return -1;

    FILE *file = fopen(filename, "rb");

    if (file == NULL)
        return -1;

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return -1;
    }

    long file_size = ftell(file);

    if (file_size < 0)
    {
        fclose(file);
        return -1;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return -1;
    }

    fm->file = file;
    fm->file_size = (uint64_t)file_size;
    fm->bytes_read = 0;
    fm->chunk_size = chunk_size;

    return 0;
}

int file_manager_read_chunk(
    FileManager *fm,
    FileChunk *chunk
)
{
    if (fm == NULL || chunk == NULL || fm->file == NULL)
        return -1;

    if (fm->bytes_read >= fm->file_size)
        return 0;

    size_t bytes_to_read = fm->chunk_size;

    uint64_t remaining = fm->file_size - fm->bytes_read;

    if (remaining < bytes_to_read)
        bytes_to_read = (size_t)remaining;

    uint8_t *buffer = malloc(bytes_to_read);

    if (buffer == NULL)
        return -1;

    size_t bytes_read = fread(
        buffer,
        1,
        bytes_to_read,
        (FILE *)fm->file
    );

    if (bytes_read != bytes_to_read)
    {
        free(buffer);
        return -1;
    }

    chunk->data = buffer;
    chunk->length = bytes_read;

    fm->bytes_read += bytes_read;

    return 1;
}

void file_manager_close(FileManager *fm)
{
    if (fm == NULL || fm->file == NULL)
        return;

    fclose((FILE *)fm->file);

    fm->file = NULL;
    fm->file_size = 0;
    fm->bytes_read = 0;
    fm->chunk_size = 0;
}

uint64_t file_manager_get_size(
    const FileManager *fm
)
{
    if (fm == NULL)
        return 0;

    return fm->file_size;
}