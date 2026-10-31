#include <stdio.h>
#include <string.h>

#include "../src/file/sha256.h"

static void print_hash(const SHA256Digest *digest)
{
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        printf("%02x", digest->data[i]);
    }

    printf("\n");
}

static int hash_matches(
    const char *input,
    const char *expected
)
{
    SHA256Digest digest;

    if (sha256_compute(
            (const uint8_t *)input,
            strlen(input),
            &digest) != 0)
    {
        printf("FAIL: SHA-256 computation failed\n");
        return 0;
    }

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
        printf("FAIL\n");
        printf("Input:    \"%s\"\n", input);
        printf("Expected: %s\n", expected);
        printf("Actual:   %s\n", actual);
        return 0;
    }

    printf("PASS: \"%s\"\n", input);
    printf("      %s\n", actual);

    return 1;
}

int main(void)
{
    int passed = 1;

    printf("===== SHA-256 Tests =====\n\n");

    /*
     * Standard SHA-256 test vectors.
     */

    if (!hash_matches(
            "",
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855"))
    {
        passed = 0;
    }

    if (!hash_matches(
            "abc",
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad"))
    {
        passed = 0;
    }

    if (!hash_matches(
            "hello world",
            "b94d27b9934d3e08a52e52d7da7dabfa"
            "c484efe37a5380ee9088f7ace2efcde9"))
    {
        passed = 0;
    }

    printf("\n==========================\n");

    if (passed)
    {
        printf("ALL SHA-256 TESTS PASSED!\n");
        return 0;
    }

    printf("SOME SHA-256 TESTS FAILED!\n");
    return 1;
}