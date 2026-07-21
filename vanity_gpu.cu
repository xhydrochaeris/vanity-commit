/*
 * vanity_gpu.cu - CUDA port of the vanity search, reusing sha1_core.h's
 * __host__ __device__ compression function.
 *
 * Build:
 *   nvcc -O3 vanity_gpu.cu -o vanity_gpu
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include "sha1_core.h"

#define COUNTER_DIGITS 20

__global__ void vanity_kernel(
    sha1_state midstate,
    const unsigned char *tail_prefix,   /* the fixed "remainder" bytes, same for every thread */
    int tail_prefix_len,
    uint64_t total_bit_len,
    const unsigned char *target_bytes,
    int full_bytes,
    int has_half_byte,
    int last_nibble,
    unsigned long long start_counter,
    unsigned long long stride,          /* gridDim*blockDim - total thread count */
    unsigned long long attempts_per_thread,
    unsigned long long *found_counter,  /* -1 sentinel until a match is written */
    int *found_flag)
{
    unsigned long long tid = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
    unsigned long long counter = start_counter + tid;

    unsigned char tail[128];
    memcpy(tail, tail_prefix, tail_prefix_len);

    for (unsigned long long a = 0; a < attempts_per_thread; a++) {
        if (*found_flag) return;   /* cheap early-out check */

        /* fixed-width 20-digit decimal counter, built by hand since
         * there's no sprintf on-device in the general case (nvcc does
         * support device printf, but building a fixed-width string by
         * hand is faster and avoids relying on it here) */
        unsigned long long c = counter;
        for (int i = COUNTER_DIGITS - 1; i >= 0; i--) {
            tail[tail_prefix_len + i] = '0' + (c % 10);
            c /= 10;
        }

        unsigned char digest[20];
        sha1_finalize_tail(midstate, tail, tail_prefix_len + COUNTER_DIGITS, total_bit_len, digest);

        bool match = true;
        for (int i = 0; i < full_bytes; i++) {
            if (digest[i] != target_bytes[i]) { match = false; break; }
        }
        if (match && has_half_byte) {
            if ((digest[full_bytes] >> 4) != last_nibble) match = false;
        }

        if (match) {
            if (atomicCAS(found_flag, 0, 1) == 0) {
                *found_counter = counter;
            }
            return;
        }

        counter += stride;
    }
}

/* ---------------- host driver ---------------- */

static const char hex_chars[] = "0123456789abcdef";

static int hexval(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    return -1;
}

#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
        exit(1); \
    } \
} while (0)

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <commit_content_before_trailer> <target_hex_prefix>\n", argv[0]);
        return 1;
    }
    const char *before = argv[1];
    const char *target = argv[2];
    size_t target_len = strlen(target);
    size_t before_len = strlen(before);
    const char trailer_prefix[] = "\n\nVanity: ";
    size_t trailer_prefix_len = strlen(trailer_prefix);

    for (size_t i = 0; i < target_len; i++) {
        if (hexval(target[i]) < 0) {
            fprintf(stderr, "target prefix must be lowercase hex\n");
            return 1;
        }
    }
    int full_bytes = (int)(target_len / 2);
    int has_half_byte = target_len & 1;
    unsigned char target_bytes[20] = {0};
    for (int i = 0; i < full_bytes; i++)
        target_bytes[i] = (hexval(target[2*i]) << 4) | hexval(target[2*i+1]);
    int last_nibble = has_half_byte ? hexval(target[target_len - 1]) : 0;

    /* Build the fixed prefix: header + '\0' + before + trailer_prefix.
     * Same construction as vanity_cpu.c. */
    size_t content_len = before_len + trailer_prefix_len + COUNTER_DIGITS;
    char header[32];
    int header_len = snprintf(header, sizeof(header), "commit %zu", content_len);

    unsigned char fixed[8192];
    size_t fixed_len = 0;
    memcpy(fixed + fixed_len, header, header_len); fixed_len += header_len;
    fixed[fixed_len++] = '\0';
    memcpy(fixed + fixed_len, before, before_len); fixed_len += before_len;
    memcpy(fixed + fixed_len, trailer_prefix, trailer_prefix_len); fixed_len += trailer_prefix_len;

    size_t full_blocks = fixed_len / 64;
    size_t aligned_len = full_blocks * 64;
    int remainder_len = (int)(fixed_len - aligned_len);

    sha1_state midstate;
    sha1_init(&midstate);
    for (size_t off = 0; off < aligned_len; off += 64)
        sha1_compress(&midstate, fixed + off);

    uint64_t total_message_len = fixed_len + COUNTER_DIGITS;
    uint64_t total_bit_len = total_message_len * 8;

    unsigned char tail_prefix[64];
    memcpy(tail_prefix, fixed + aligned_len, remainder_len);

    /* device buffers */
    unsigned char *d_tail_prefix, *d_target_bytes;
    unsigned long long *d_found_counter;
    int *d_found_flag;

    CUDA_CHECK(cudaMalloc(&d_tail_prefix, remainder_len));
    CUDA_CHECK(cudaMalloc(&d_target_bytes, 20));
    CUDA_CHECK(cudaMalloc(&d_found_counter, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMalloc(&d_found_flag, sizeof(int)));

    CUDA_CHECK(cudaMemcpy(d_tail_prefix, tail_prefix, remainder_len, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_target_bytes, target_bytes, 20, cudaMemcpyHostToDevice));

    int zero = 0;
    CUDA_CHECK(cudaMemcpy(d_found_flag, &zero, sizeof(int), cudaMemcpyHostToDevice));

    int threads_per_block, min_grid_size;
    CUDA_CHECK(cudaOccupancyMaxPotentialBlockSize(&min_grid_size, &threads_per_block, vanity_kernel, 0, 0));

    int num_sms;
    CUDA_CHECK(cudaDeviceGetAttribute(&num_sms, cudaDevAttrMultiProcessorCount, 0));

    /* Oversubscribe past the occupancy-suggested minimum grid size to give
     * the scheduler enough blocks to hide memory/branch latency — a common
     * rule of thumb is several dozen blocks per SM, not just enough to hit
     * min_grid_size exactly. */
    int blocks = num_sms * 32;
    if (blocks < min_grid_size) blocks = min_grid_size;

    fprintf(stderr, "auto-tuned: %d threads/block, %d blocks (%d SMs detected, occupancy-suggested min grid %d)\n",
            threads_per_block, blocks, num_sms, min_grid_size);

    unsigned long long stride = (unsigned long long)threads_per_block * blocks;
    unsigned long long attempts_per_thread = 100000ULL;

    unsigned long long start_counter = 0;
    int found = 0;
    unsigned long long winning_counter = 0;

    while (!found) {
        vanity_kernel<<<blocks, threads_per_block>>>(
            midstate, d_tail_prefix, remainder_len, total_bit_len,
            d_target_bytes, full_bytes, has_half_byte, last_nibble,
            start_counter, stride, attempts_per_thread,
            d_found_counter, d_found_flag);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());

        CUDA_CHECK(cudaMemcpy(&found, d_found_flag, sizeof(int), cudaMemcpyDeviceToHost));
        if (found) {
            CUDA_CHECK(cudaMemcpy(&winning_counter, d_found_counter, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
            break;
        }
        start_counter += stride * attempts_per_thread;
    }

    char counter_str[COUNTER_DIGITS + 1];
    snprintf(counter_str, sizeof(counter_str), "%0*llu", COUNTER_DIGITS, winning_counter);

    unsigned char final_tail[128];
    memcpy(final_tail, tail_prefix, remainder_len);
    memcpy(final_tail + remainder_len, counter_str, COUNTER_DIGITS);
    unsigned char digest[20];
    sha1_finalize_tail(midstate, final_tail, remainder_len + COUNTER_DIGITS, total_bit_len, digest);

    char hexdigest[41];
    for (int i = 0; i < 20; i++) {
        hexdigest[2*i]   = hex_chars[digest[i] >> 4];
        hexdigest[2*i+1] = hex_chars[digest[i] & 0xF];
    }
    hexdigest[40] = '\0';

    printf("hash: %s\n", hexdigest);
    printf("trailer: %s%s\n", trailer_prefix, counter_str);
    printf("counter: %llu\n", winning_counter);

    cudaFree(d_tail_prefix);
    cudaFree(d_target_bytes);
    cudaFree(d_found_counter);
    cudaFree(d_found_flag);

    return 0;
}
