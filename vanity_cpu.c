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

    volatile int found = 0;
    long winning_counter = -1;
    char winning_hex[SHA_DIGEST_LENGTH * 2 + 1];
    char winning_trailer[64];

    double t0 = omp_get_wtime();
    uint64_t total_tried = 0;

    #pragma omp parallel reduction(+:total_tried)
    {
        int tid = omp_get_thread_num();
        int nthreads = omp_get_num_threads();
        uint64_t counter = tid;

        char blob[MAX_BLOB];
        char header[32];
        unsigned char digest[SHA_DIGEST_LENGTH];
        char hexdigest[SHA_DIGEST_LENGTH * 2 + 1];
        char trailer[64];

        memcpy(blob, before, before_len);

        while (!found) {
            int trailer_len = snprintf(trailer, sizeof(trailer), "\n\nVanity: %ld", counter);
            memcpy(blob + before_len, trailer, trailer_len);
            size_t content_len = before_len + trailer_len;

            int header_len = snprintf(header, sizeof(header), "commit %zu", content_len);

            /* full git object = "commit <len>\0" + content, hashed as one buffer */
            unsigned char full[MAX_BLOB + 32];
            memcpy(full, header, header_len);
            full[header_len] = '\0';
            memcpy(full + header_len + 1, blob, content_len);

            SHA1(full, header_len + 1 + content_len, digest);
            for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
                sprintf(hexdigest + i * 2, "%02x", digest[i]);

            total_tried++;

            if (strncmp(hexdigest, target, target_len) == 0) {
                #pragma omp critical
                {
                    if (!found) {
                        found = 1;
                        winning_counter = counter;
                        memcpy(winning_hex, hexdigest, sizeof(hexdigest));
                        memcpy(winning_trailer, trailer, trailer_len + 1);
                    }
                }
            }

            counter += nthreads;
        }
    }

    double elapsed = omp_get_wtime() - t0;
    fprintf(stderr, "tried %ld hashes in %.2fs (%.1fM/s)\n",
            total_tried, elapsed, total_tried / elapsed / 1e6);

    printf("hash: %s\n", winning_hex);
    printf("trailer: %s\n", winning_trailer);
    printf("counter: %ld\n", winning_counter);

    return 0;
}
