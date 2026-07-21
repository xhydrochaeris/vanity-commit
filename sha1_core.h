/*
 * sha1_core.h - a single SHA-1 compression-function implementation meant
 * to be compiled for BOTH host and device (CUDA's __host__ __device__
 * qualifiers), so there is exactly one place that can get the algorithm
 * wrong, not two independently-written copies that have to agree.
 *
 * When compiled with nvcc, HD expands to "__host__ __device__" so the
 * same function is usable from both host code (to precompute the
 * midstate) and kernel code (to process the per-attempt tail). When
 * compiled with plain gcc (as here, for testing on a machine with no
 * CUDA toolkit), HD is empty and this is just ordinary portable C.
 */

#ifndef SHA1_CORE_H
#define SHA1_CORE_H

#include <stdint.h>
#include <string.h>

#ifdef __CUDACC__
  #define HD __host__ __device__
#else
  #define HD
#endif

typedef struct {
    uint32_t h[5];
} sha1_state;

HD static inline uint32_t sha1_rol(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

/* Process exactly one 64-byte block, updating state in place.
 * `block` must be exactly 64 bytes, big-endian word order (as SHA-1
 * defines it) — the caller is responsible for byte order on the host
 * side; on a little-endian GPU you handle it the same way. */
HD static void sha1_compress(sha1_state *s, const unsigned char block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

        uint32_t temp = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rol(b, 30); b = a; a = temp;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

HD static inline void sha1_init(sha1_state *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
}

/* Process a tail of `len` bytes (len < 128, enough for our use case:
 * a small fixed remainder + a 20-digit counter, always 1-2 blocks) plus
 * standard SHA-1 padding, given the TOTAL original message bit length
 * (not just this tail's length - the length field in the padding
 * encodes the whole message from byte 0, per the SHA-1 spec). Writes
 * the final digest (20 bytes) into `out`. */
HD static void sha1_finalize_tail(sha1_state state, const unsigned char *tail, int len,
                                   uint64_t total_bit_len, unsigned char out[20]) {
    unsigned char buf[128] = {0};
    memcpy(buf, tail, len);
    buf[len] = 0x80;

    int total_len = len + 1 + 8;                 /* content + 0x80 + 8-byte length field */
    int padded_len = ((total_len + 63) / 64) * 64; /* round up to next multiple of 64 */

    /* big-endian 64-bit total bit length goes in the LAST 8 bytes */
    for (int i = 0; i < 8; i++) {
        buf[padded_len - 1 - i] = (unsigned char)(total_bit_len >> (8 * i));
    }

    for (int off = 0; off < padded_len; off += 64) {
        sha1_compress(&state, buf + off);
    }

    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(state.h[i] >> 24);
        out[i*4+1] = (unsigned char)(state.h[i] >> 16);
        out[i*4+2] = (unsigned char)(state.h[i] >> 8);
        out[i*4+3] = (unsigned char)(state.h[i]);
    }
}

#endif
