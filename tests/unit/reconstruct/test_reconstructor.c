
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/reconstruct/reconstructor.h"

#define TEST_OUTPUT_PATH "tests/reconstructed_test_output.bin"

static int file_matches(
    const char *path,
    const uint8_t *expected,
    size_t expected_length
)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL)
    {
        return 0;
    }

    uint8_t buffer[256];
    size_t total = 0;
    size_t bytes_read;

    while ((bytes_read = fread(
                buffer,
                1,
                sizeof(buffer),
                file)) > 0)
    {
        if (total + bytes_read > expected_length ||
            memcmp(buffer, expected + total, bytes_read) != 0)
        {
            fclose(file);
            return 0;
        }

        total += bytes_read;
    }

    int read_error = ferror(file);
    fclose(file);

    return !read_error && total == expected_length;
}

static int test_multiple_chunks(void)
{
    const uint8_t first[] = "Reliable ";
    const uint8_t second[] = "UDP file ";
    const uint8_t third[] = "transfer!";
    const uint8_t expected_data[] = "Reliable UDP file transfer!";

    SHA256Digest expected_digest;
    SHA256Digest actual_digest;

    if (sha256_compute(
            expected_data,
            sizeof(expected_data) - 1,
            &expected_digest) != 0)
    {
        return 0;
    }

    Reconstructor reconstructor;

    if (reconstructor_open(
            &reconstructor,
            TEST_OUTPUT_PATH) != 0)
    {
        return 0;
    }

    FileChunk chunks[] = {
        {(uint8_t *)first, sizeof(first) - 1},
        {(uint8_t *)second, sizeof(second) - 1},
        {(uint8_t *)third, sizeof(third) - 1}
    };

    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
    {
        if (reconstructor_write_chunk(
                &reconstructor,
                &chunks[i]) != 0)
        {
            reconstructor_abort(&reconstructor);
            return 0;
        }
    }

    int result = reconstructor_finish(
        &reconstructor,
        &expected_digest,
        &actual_digest);

    if (result != 0)
    {
        return 0;
    }

    return file_matches(
        TEST_OUTPUT_PATH,
        expected_data,
        sizeof(expected_data) - 1);
}

static int test_empty_file(void)
{
    SHA256Digest expected_digest;

    if (sha256_compute(NULL, 0, &expected_digest) != 0)
    {
        return 0;
    }

    Reconstructor reconstructor;

    if (reconstructor_open(
            &reconstructor,
            TEST_OUTPUT_PATH) != 0)
    {
        return 0;
    }

    int result = reconstructor_finish(
        &reconstructor,
        &expected_digest,
        NULL);

    return result == 0 &&
           file_matches(TEST_OUTPUT_PATH, NULL, 0);
}

static int test_hash_mismatch(void)
{
    const uint8_t data[] = "received data";

    SHA256Digest incorrect_digest = {{0}};

    Reconstructor reconstructor;

    if (reconstructor_open(
            &reconstructor,
            TEST_OUTPUT_PATH) != 0)
    {
        return 0;
    }

    FileChunk chunk = {
        (uint8_t *)data,
        sizeof(data) - 1
    };

    if (reconstructor_write_chunk(
            &reconstructor,
            &chunk) != 0)
    {
        reconstructor_abort(&reconstructor);
        return 0;
    }

    int result = reconstructor_finish(
        &reconstructor,
        &incorrect_digest,
        NULL);

    return result == 1;
}

static int test_invalid_arguments(void)
{
    Reconstructor reconstructor;
    FileChunk chunk = {NULL, 1};

    if (reconstructor_open(NULL, TEST_OUTPUT_PATH) != -1)
    {
        return 0;
    }

    if (reconstructor_open(&reconstructor, NULL) != -1)
    {
        return 0;
    }

    if (reconstructor_write_chunk(&reconstructor, &chunk) != -1)
    {
        return 0;
    }

    if (reconstructor_finish(&reconstructor, NULL, NULL) != -1)
    {
        return 0;
    }

    return 1;
}

int main(void)
{
    int passed = 1;

    printf("===== Reconstructor Tests =====\n\n");

    if (test_multiple_chunks())
        printf("PASS: Multiple chunks reconstruct correctly\n");
    else
    {
        printf("FAIL: Multiple chunks reconstruct correctly\n");
        passed = 0;
    }

    if (test_empty_file())
        printf("PASS: Empty file reconstruction\n");
    else
    {
        printf("FAIL: Empty file reconstruction\n");
        passed = 0;
    }

    if (test_hash_mismatch())
        printf("PASS: SHA-256 mismatch detected\n");
    else
    {
        printf("FAIL: SHA-256 mismatch detected\n");
        passed = 0;
    }

    if (test_invalid_arguments())
        printf("PASS: Invalid arguments rejected\n");
    else
    {
        printf("FAIL: Invalid arguments rejected\n");
        passed = 0;
    }

    remove(TEST_OUTPUT_PATH);

    printf("\n==============================\n");

    if (passed)
    {
        printf("ALL RECONSTRUCTOR TESTS PASSED!\n");
        return 0;
    }

    printf("SOME RECONSTRUCTOR TESTS FAILED!\n");
    return 1;
}
