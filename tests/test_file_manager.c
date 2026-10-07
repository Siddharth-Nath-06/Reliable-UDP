#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file/file_manager.h"

static int create_test_file(const char *filename, size_t size)
{
    FILE *file = fopen(filename, "wb");

    if (file == NULL)
        return -1;

    for (size_t i = 0; i < size; i++)
    {
        unsigned char value = (unsigned char)('A' + (i % 26));

        if (fwrite(&value, 1, 1, file) != 1)
        {
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    return 0;
}

static int test_read_chunks(
    const char *filename,
    size_t chunk_size,
    uint64_t expected_size
)
{
    FileManager fm;
    FileChunk chunk;

    if (file_manager_open(&fm, filename, chunk_size) != 0)
    {
        printf("FAIL: Could not open %s\n", filename);
        return 0;
    }

    if (file_manager_get_size(&fm) != expected_size)
    {
        printf("FAIL: Incorrect file size for %s\n", filename);
        file_manager_close(&fm);
        return 0;
    }

    size_t total_bytes = 0;
    int chunk_count = 0;
    int result;

    while ((result = file_manager_read_chunk(&fm, &chunk)) == 1)
    {
        if (chunk.length == 0 || chunk.length > chunk_size)
        {
            printf("FAIL: Invalid chunk size\n");
            file_manager_close(&fm);
            return 0;
        }

        total_bytes += chunk.length;
        chunk_count++;

        free(chunk.data);
    }

    file_manager_close(&fm);

    if (result < 0)
    {
        printf("FAIL: Error while reading %s\n", filename);
        return 0;
    }

    if (total_bytes != expected_size)
    {
        printf(
            "FAIL: Expected %llu bytes, got %zu bytes\n",
            (unsigned long long)expected_size,
            total_bytes
        );
        return 0;
    }

    printf(
        "PASS: %s | size=%llu | chunk_size=%zu | chunks=%d\n",
        filename,
        (unsigned long long)expected_size,
        chunk_size,
        chunk_count
    );

    return 1;
}

int main(void)
{
    int passed = 1;

    printf("===== File Manager Tests =====\n\n");

    /*
     * Test 1:
     * Existing test input.
     */
    if (!test_read_chunks(
            "tests/test_input.txt",
            10,
            55))
    {
        passed = 0;
    }

    /*
     * Test 2:
     * Empty file.
     */
    create_test_file("tests/test_empty.bin", 0);

    if (!test_read_chunks(
            "tests/test_empty.bin",
            10,
            0))
    {
        passed = 0;
    }

    /*
     * Test 3:
     * One-byte file.
     */
    create_test_file("tests/test_1byte.bin", 1);

    if (!test_read_chunks(
            "tests/test_1byte.bin",
            10,
            1))
    {
        passed = 0;
    }

    /*
     * Test 4:
     * Exactly one chunk.
     */
    create_test_file("tests/test_exact.bin", 10);

    if (!test_read_chunks(
            "tests/test_exact.bin",
            10,
            10))
    {
        passed = 0;
    }

    /*
     * Test 5:
     * Slightly larger than one chunk.
     */
    create_test_file("tests/test_11bytes.bin", 11);

    if (!test_read_chunks(
            "tests/test_11bytes.bin",
            10,
            11))
    {
        passed = 0;
    }

    /*
     * Test 6:
     * Maximum allowed chunk size.
     */
    create_test_file("tests/test_1024bytes.bin", 1024);

    if (!test_read_chunks(
            "tests/test_1024bytes.bin",
            1024,
            1024))
    {
        passed = 0;
    }

    /*
     * Test 7:
     * Invalid chunk size = 0.
     */
    {
        FileManager fm;

        if (file_manager_open(
                &fm,
                "tests/test_input.txt",
                0) == 0)
        {
            printf("FAIL: Chunk size 0 should be rejected\n");
            file_manager_close(&fm);
            passed = 0;
        }
        else
        {
            printf("PASS: Chunk size 0 rejected\n");
        }
    }

    /*
     * Test 8:
     * Chunk size greater than MAX_PAYLOAD_SIZE.
     */
    {
        FileManager fm;

        if (file_manager_open(
                &fm,
                "tests/test_input.txt",
                1025) == 0)
        {
            printf("FAIL: Chunk size 1025 should be rejected\n");
            file_manager_close(&fm);
            passed = 0;
        }
        else
        {
            printf("PASS: Chunk size 1025 rejected\n");
        }
    }

    /*
     * Test 9:
     * Non-existent file.
     */
    {
        FileManager fm;

        if (file_manager_open(
                &fm,
                "tests/does_not_exist.txt",
                10) == 0)
        {
            printf("FAIL: Non-existent file should be rejected\n");
            file_manager_close(&fm);
            passed = 0;
        }
        else
        {
            printf("PASS: Non-existent file rejected\n");
        }
    }

    /*
     * Final result.
     */
    printf("\n==============================\n");

    if (passed)
    {
        printf("ALL FILE MANAGER TESTS PASSED!\n");
        return 0;
    }

    printf("SOME FILE MANAGER TESTS FAILED!\n");
    return 1;
}