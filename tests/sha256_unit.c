/*
 * tests/sha256_unit.c -- runtime/sha256.c, and the properties pkg_hash_dir
 * depends on.
 *
 * Covers:
 *   1  FIPS 180-4 / RFC 6234 vectors, so the digest is the real SHA-256 and a
 *      lockfile value stays checkable by any other tool.
 *   2  Streaming equals one-shot for every split of the input.  The tree hash
 *      feeds a path, a length and a file's bytes as separate updates, so a
 *      chunk-boundary bug there would produce a wrong-but-stable digest --
 *      which no fixture could tell from a right one.
 *   3  The 55/56/64-byte boundaries, where the length pad spills into a second
 *      block.
 *   4  tur_sha256_file on a file bigger than the read buffer.
 */

#include "runtime/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }            \
    else         { printf("ok:   %s\n", (msg)); }                        \
} while (0)

static void hex_of(const void *data, size_t len, char out[65]) {
    TurSha256 ctx;
    uint8_t digest[TUR_SHA256_DIGEST_LEN];
    tur_sha256_init(&ctx);
    tur_sha256_update(&ctx, data, len);
    tur_sha256_final(&ctx, digest);
    tur_sha256_hex(digest, out);
}

static void check_vector(const char *input, const char *want, const char *label) {
    char got[65];
    hex_of(input, strlen(input), got);
    if (strcmp(got, want) != 0) {
        printf("FAIL: %s\n  want %s\n  got  %s\n", label, want, got);
        failures++;
    } else {
        printf("ok:   %s\n", label);
    }
}

int main(void) {
    /* 1: known-answer vectors. */
    check_vector("",
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 "empty string");
    check_vector("abc",
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "\"abc\"");
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                 "56-byte multi-block string");

    /* A million 'a', the classic long vector -- exercises the update loop and
     * the 64-bit length field. */
    {
        TurSha256 ctx;
        uint8_t digest[TUR_SHA256_DIGEST_LEN];
        char got[65];
        char chunk[1000];
        memset(chunk, 'a', sizeof chunk);
        tur_sha256_init(&ctx);
        for (int i = 0; i < 1000; i++) tur_sha256_update(&ctx, chunk, sizeof chunk);
        tur_sha256_final(&ctx, digest);
        tur_sha256_hex(digest, got);
        CHECK(strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e"
                          "046d39ccc7112cd0") == 0,
              "one million 'a'");
    }

    /* 2: streaming == one-shot at every split.  200 bytes spans three blocks
     * and the boundaries between them. */
    {
        uint8_t data[200];
        for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + 3);
        char whole[65];
        hex_of(data, sizeof data, whole);

        int mismatches = 0;
        for (size_t cut = 0; cut <= sizeof data; cut++) {
            TurSha256 ctx;
            uint8_t digest[TUR_SHA256_DIGEST_LEN];
            char got[65];
            tur_sha256_init(&ctx);
            tur_sha256_update(&ctx, data, cut);
            tur_sha256_update(&ctx, data + cut, sizeof data - cut);
            tur_sha256_final(&ctx, digest);
            tur_sha256_hex(digest, got);
            if (strcmp(got, whole) != 0) mismatches++;
        }
        CHECK(mismatches == 0, "streaming equals one-shot at all 201 splits");

        /* Byte at a time, the worst case for the block buffer. */
        {
            TurSha256 ctx;
            uint8_t digest[TUR_SHA256_DIGEST_LEN];
            char got[65];
            tur_sha256_init(&ctx);
            for (size_t i = 0; i < sizeof data; i++) tur_sha256_update(&ctx, data + i, 1);
            tur_sha256_final(&ctx, digest);
            tur_sha256_hex(digest, got);
            CHECK(strcmp(got, whole) == 0, "streaming one byte at a time");
        }
    }

    /* 3: the pad boundaries.  55 bytes fits the length in the same block, 56
     * does not, 64 is exactly full -- each takes a different branch. */
    {
        static const struct { size_t len; const char *want; } cases[] = {
            { 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
            { 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
            { 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char input[65];
            char got[65];
            char label[64];
            memset(input, 'a', cases[i].len);
            hex_of(input, cases[i].len, got);
            snprintf(label, sizeof label, "%zu-byte pad boundary", cases[i].len);
            if (strcmp(got, cases[i].want) != 0) {
                printf("FAIL: %s\n  want %s\n  got  %s\n", label, cases[i].want, got);
                failures++;
            } else {
                printf("ok:   %s\n", label);
            }
        }
    }

    /* 4: tur_sha256_file, over more than one read buffer (64 KiB). */
    {
        const char *path = "sha256_unit_tmp.bin";
        size_t n = 200000;
        uint8_t *data = (uint8_t *)malloc(n);
        CHECK(data != NULL, "allocated the file body");
        if (data) {
            for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(i * 31 + 17);
            FILE *f = fopen(path, "wb");
            CHECK(f != NULL, "opened the temp file");
            if (f) {
                size_t put = fwrite(data, 1, n, f);
                fclose(f);
                CHECK(put == n, "wrote the temp file");

                char want[65], got[65];
                uint8_t digest[TUR_SHA256_DIGEST_LEN];
                hex_of(data, n, want);
                CHECK(tur_sha256_file(path, digest), "tur_sha256_file read it");
                tur_sha256_hex(digest, got);
                CHECK(strcmp(got, want) == 0,
                      "tur_sha256_file matches the in-memory digest");
                remove(path);
            }
            free(data);
        }
        uint8_t unused[TUR_SHA256_DIGEST_LEN];
        CHECK(!tur_sha256_file("sha256_unit_no_such_file.bin", unused),
              "tur_sha256_file reports a missing file");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
