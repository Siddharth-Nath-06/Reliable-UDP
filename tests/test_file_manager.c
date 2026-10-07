#include <stdio.h>
#include "../src/file/file_manager.h"

int main(void)
{
    FileManager fm;
    FileChunk chunk;

    if (file_manager_open(&fm, "test_input.txt", 10) != 0)
    {
        printf("Failed to open file\n");
        return 1;
    }

    printf("File size: %llu bytes\n",
           (unsigned long long)file_manager_get_size(&fm));

    int result;

    while ((result = file_manager_read_chunk(&fm, &chunk)) == 1)
    {
        printf("Chunk: %zu bytes -> ", chunk.length);

        for (size_t i = 0; i < chunk.length; i++)
            putchar(chunk.data[i]);

        printf("\n");
    }

    file_manager_close(&fm);

    if (result < 0)
    {
        printf("Error while reading file\n");
        return 1;
    }

    printf("File Manager test passed!\n");

    return 0;
}