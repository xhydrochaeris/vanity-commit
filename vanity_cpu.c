/*
 * vanity_cpu.c - brute-force search for a git commit-object SHA-1 with a
 * chosen hex prefix, by appending an incrementing counter to a fixed
 * "message suffix" region of the commit object.
 *
 * A git commit object's hash is SHA1("commit <byte-length>\0" + <commit content>).
 * We hold everything in <commit content> fixed except a trailer line we
 * append at the end (e.g. "\nVanity: 481923"), and try counters until the
 * resulting hash starts with the target hex prefix.
 *
 * Build:
 *   gcc -O3 -fopenmp vanity_cpu.c -o vanity_cpu -lcrypto
 *
 * Run:
 *   ./vanity_cpu "<commit-content-before-trailer>" "69420"
 *
 * The wrapper is responsible for constructing the real commit object text
 * (tree/parent/author/committer headers + message) and passing it in as
 * argv[1]; this program only appends "\nVanity: N" and searches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <openssl/sha.h>

#define MAX_BLOB 8192
#define TRAILER_PREFIX "\n\nVanity: "
#define TRAILER_PREFIX_LEN (sizeof(TRAILER_PREFIX) - 1) // - 1 to drop '/0' at the end
#define COUNTER_DIGITS 20 // ULLONG_MAX = 18446744073709551615 is 20 decimal digits
#define TRAILER_LEN (TRAILER_PREFIX_LEN + COUNTER_DIGITS)
static const char hex[] = "0123456789abcdef";

int hexval(char c)
{
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <commit_content_before_trailer> <target_hex_prefix>\n", argv[0]);
        return 1;
    }
    const char *before = argv[1];
    const char *target = argv[2];
    size_t target_len = strlen(target);
    size_t before_len = strlen(before);

    for (size_t i = 0; i < target_len; i++) {
        char c = target[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            fprintf(stderr, "target prefix must be lowercase hex\n");
            return 1;
        }
    }

    size_t full_bytes = target_len / 2;
    int has_half_byte = target_len & 1;

    unsigned char target_bytes[SHA_DIGEST_LENGTH];

    for (size_t i = 0; i < full_bytes; i++) {
        int hi = hexval(target[2*i]);
        int lo = hexval(target[2*i + 1]);

        if (hi < 0 || lo < 0) {
            fprintf(stderr, "invalid hex digit\n");
            exit(1);
        }

        target_bytes[i] = (hi << 4) | lo;
    }

    int last_nibble = 0;
    if (has_half_byte) {
        last_nibble = hexval(target[target_len - 1]);
        if (last_nibble < 0) {
            fprintf(stderr, "invalid hex digit\n");
            exit(1);
        }
    }

    // content_len and header_len are now constant since trailer is always exactly TRAILER_LEN bytes long, regardless of counter value
    size_t content_len = before_len + TRAILER_LEN;
    char header[32];
    int header_len = snprintf(header, sizeof(header), "commit %zu", content_len);

    // Hash the prefix exactly once. Every attempt clones this context instead of re-hashing header+before+trailer_prefix.
    SHA_CTX base_ctx;
    SHA1_Init(&base_ctx);
    SHA1_Update(&base_ctx, header, header_len);
    SHA1_Update(&base_ctx, "\0", 1);
    SHA1_Update(&base_ctx, before, before_len);
    SHA1_Update(&base_ctx, TRAILER_PREFIX, TRAILER_PREFIX_LEN);

    volatile int found = 0;
    unsigned long long winning_counter = -1;
    char winning_hex[SHA_DIGEST_LENGTH * 2 + 1];
    char winning_trailer[64];

    double t0 = omp_get_wtime();
    unsigned long long total_tried = 0;

    #pragma omp parallel reduction(+:total_tried)
    {
        int tid = omp_get_thread_num();
        int nthreads = omp_get_num_threads();
        unsigned long long counter = tid;

        char counter_str[COUNTER_DIGITS + 1];
        unsigned char digest[SHA_DIGEST_LENGTH];

        while (!found) {
            snprintf(counter_str, sizeof(counter_str), "%0*llu", COUNTER_DIGITS, counter);

            SHA_CTX ctx = base_ctx; // copy CTX instead of re-hashing
            SHA1_Update(&ctx, counter_str, COUNTER_DIGITS);
            SHA1_Final(digest, &ctx);

            total_tried++;

            if (memcmp(digest, target_bytes, full_bytes) == 0 &&
                (!has_half_byte || ((digest[full_bytes] >> 4) == last_nibble)))
            {
                #pragma omp critical
                {
                    if (!found) {
                        found = 1;
                        winning_counter = counter;
                        for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
                            winning_hex[2*i]     = hex[digest[i] >> 4];
                            winning_hex[2*i + 1] = hex[digest[i] & 0xF];
                        }
                        winning_hex[40] = '\0';
                        memcpy(winning_trailer, TRAILER_PREFIX, TRAILER_PREFIX_LEN);
                        memcpy(winning_trailer + TRAILER_PREFIX_LEN, counter_str, COUNTER_DIGITS);
                        winning_trailer[TRAILER_LEN] = '\0';
                    }
                }
            }

            counter += nthreads;
        }
    }

    double elapsed = omp_get_wtime() - t0;
    fprintf(stderr, "tried %llu hashes in %.2fs (%.1fM/s)\n",
            total_tried, elapsed, total_tried / elapsed / 1e6);

    printf("hash: %s\n", winning_hex);
    printf("trailer: %s\n", winning_trailer);
    printf("counter: %llu\n", winning_counter);

    return 0;
}
