#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define FILE_MANAGER_MAX_CHUNK_SIZE 1024

typedef struct {
    unsigned char data[FILE_MANAGER_MAX_CHUNK_SIZE];
    size_t length;
} FileChunk;

typedef struct {
    FILE *file;
    uint64_t file_size;
    uint64_t bytes_read;
    size_t chunk_size;
} FileManager;

/* Open a file for reading */
int file_manager_open(FileManager *fm, const char *filename, size_t chunk_size);

/* Read the next chunk from the file */
int file_manager_read_chunk(FileManager *fm, FileChunk *chunk);

/* Close the file */
void file_manager_close(FileManager *fm);

/* Get the total file size */
uint64_t file_manager_get_size(const FileManager *fm);

#endif