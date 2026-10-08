#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file/file_manager.h"
#include "../src/file/sha256.h"

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
    {
    FileManager fm;
    SHA256Digest digest;

    const char *expected =
        "d6e3e5c2e6f6c1c6f8e3c7f7d5e5e5e5";

    if (file_manager_open(
            &fm,
            "tests/test_input.txt",
            10) != 0)
    {
        printf("FAIL: Could not open file for SHA-256 test\n");
        passed = 0;
    }
    else
    {
        if (file_manager_get_sha256(&fm, &digest) != 0)
        {
            printf("FAIL: Could not calculate file SHA-256\n");
            passed = 0;
        }
        else
        {
            printf("PASS: File SHA-256 calculated\n");
        }

        file_manager_close(&fm);
    }
}
{
    FileManager fm;
    SHA256Digest digest;

    const char *expected =
        "357e1c00b4e2760d65e4b11af571f53"
        "e470958d364a1c4b1e3f4c208fd024951";

    if (file_manager_open(
            &fm,
            "tests/test_input.txt",
            10) != 0)
    {
        printf("FAIL: Could not open file for SHA-256 test\n");
        passed = 0;
    }
    else
    {
        if (file_manager_get_sha256(&fm, &digest) != 0)
        {
            printf("FAIL: Could not calculate file SHA-256\n");
            passed = 0;
        }
        else
        {
            char actual[SHA256_DIGEST_SIZE * 2 + 1];

            for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
            {
                sprintf(
                    &actual[i * 2],
                    "%02x",
                    digest.data[i]
                );
            }

            actual[SHA256_DIGEST_SIZE * 2] = '\0';

            if (strcmp(actual, expected) != 0)
            {
                printf("FAIL: Incorrect file SHA-256\n");
                printf("Expected: %s\n", expected);
                printf("Actual:   %s\n", actual);
                passed = 0;
            }
            else
            {
                printf("PASS: File SHA-256 verified\n");
                printf("      %s\n", actual);
            }
        }

        file_manager_close(&fm);
    }
}
{
    FileManager fm;
    FileManager reference;

    FileChunk first_chunk;
    FileChunk second_chunk;
    FileChunk reference_first;
    FileChunk reference_second;

    SHA256Digest digest;

    if (file_manager_open(
            &fm,
            "tests/test_input.txt",
            10) != 0)
    {
        printf("FAIL: Could not open file for position test\n");
        passed = 0;
    }
    else if (file_manager_open(
                 &reference,
                 "tests/test_input.txt",
                 10) != 0)
    {
        printf("FAIL: Could not open reference file\n");
        file_manager_close(&fm);
        passed = 0;
    }
    else
    {
        /*
         * Read the first chunk normally.
         */
        if (file_manager_read_chunk(
                &fm,
                &first_chunk) != 1)
        {
            printf("FAIL: Could not read first chunk\n");
            passed = 0;
        }
        else
        {
            /*
             * Read the first chunk from a fresh File Manager.
             */
            if (file_manager_read_chunk(
                    &reference,
                    &reference_first) != 1)
            {
                printf("FAIL: Could not read reference first chunk\n");
                passed = 0;
            }
            else
            {
                /*
                 * Calculate SHA-256 in the middle of reading.
                 */
                if (file_manager_get_sha256(
                        &fm,
                        &digest) != 0)
                {
                    printf(
                        "FAIL: SHA-256 failed during position test\n"
                    );
                    passed = 0;
                }
                else
                {
                    /*
                     * Both File Managers should now produce
                     * the same second chunk.
                     */
                    int result1 = file_manager_read_chunk(
                        &fm,
                        &second_chunk
                    );

                    int result2 = file_manager_read_chunk(
                        &reference,
                        &reference_second
                    );

                    if (result1 != 1 || result2 != 1)
                    {
                        printf(
                            "FAIL: Could not read second chunks\n"
                        );
                        passed = 0;
                    }
                    else if (
                        second_chunk.length !=
                            reference_second.length ||
                        memcmp(
                            second_chunk.data,
                            reference_second.data,
                            second_chunk.length) != 0)
                    {
                        printf(
                            "FAIL: File position was not preserved\n"
                        );
                        passed = 0;
                    }
                    else
                    {
                        printf(
                            "PASS: SHA-256 preserves file position\n"
                        );
                    }

                    free(second_chunk.data);
                    free(reference_second.data);
                }

                free(reference_first.data);
            }

            free(first_chunk.data);
        }

        file_manager_close(&fm);
        file_manager_close(&reference);
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