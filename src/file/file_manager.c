#include "file_manager.h"

int file_manager_open(FileManager *fm, const char *filename, size_t chunk_size)
{
    if (fm == NULL || filename == NULL)
        return -1;

    if (chunk_size == 0 || chunk_size > FILE_MANAGER_MAX_CHUNK_SIZE)
        return -1;

    fm->file = fopen(filename, "rb");

    if (fm->file == NULL)
        return -1;

    fseek(fm->file, 0, SEEK_END);
    fm->file_size = ftell(fm->file);
    fseek(fm->file, 0, SEEK_SET);

    fm->bytes_read = 0;
    fm->chunk_size = chunk_size;

    return 0;
}

int file_manager_read_chunk(FileManager *fm, FileChunk *chunk)
{
    if (fm == NULL || chunk == NULL || fm->file == NULL)
        return -1;

    if (fm->bytes_read >= fm->file_size)
        return 0;

    size_t bytes_to_read = fm->chunk_size;

    if (fm->file_size - fm->bytes_read < bytes_to_read)
        bytes_to_read = fm->file_size - fm->bytes_read;

    size_t bytes_read = fread(chunk->data, 1, bytes_to_read, fm->file);

    if (bytes_read != bytes_to_read)
        return -1;

    chunk->length = bytes_read;
    fm->bytes_read += bytes_read;

    return 1;
}

void file_manager_close(FileManager *fm)
{
    if (fm == NULL || fm->file == NULL)
        return;

    fclose(fm->file);
    fm->file = NULL;
}

uint64_t file_manager_get_size(const FileManager *fm)
{
    if (fm == NULL)
        return 0;

    return fm->file_size;
}