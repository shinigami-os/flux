// A from-scratch SHA-256 (FIPS 180-4), used only to replace sha256_of_file's old
// popen("sha256sum ...") shell-out - that spawned a whole process per file, which barely
// mattered for one package's own install but made flux_checksum_backfill and flux_diff
// (every file of every installed package) take minutes on a real system.
#include "../include/sha256.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t buf_len;
    uint64_t total_len;
} sha256_ctx_t;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx_t *ctx, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx) {
    static const uint32_t h0[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };
    memcpy(ctx->h, h0, sizeof(h0));
    ctx->buf_len = 0;
    ctx->total_len = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    ctx->total_len += len;
    while (len > 0) {
        size_t take = 64 - ctx->buf_len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]) {
    uint64_t bit_len = ctx->total_len * 8;
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while (ctx->buf_len != 56) sha256_update(ctx, &zero, 1);

    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bit_len >> (56 - 8 * i));
    // bypass sha256_update's own total_len bookkeeping - we're appending the length field itself
    memcpy(ctx->buf + ctx->buf_len, len_be, 8);
    sha256_block(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)(ctx->h[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->h[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->h[i]);
    }
}

static void hex_encode(const uint8_t digest[32], char hexbuf[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hexbuf[i*2]   = hex[digest[i] >> 4];
        hexbuf[i*2+1] = hex[digest[i] & 0xf];
    }
    hexbuf[64] = '\0';
}

void flux_sha256_file(const char *path, char *out, size_t outlen) {
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return;

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&ctx, buf, n);

    if (ferror(f)) { fclose(f); out[0] = '\0'; return; }
    fclose(f);

    uint8_t digest[32];
    sha256_final(&ctx, digest);

    char hexbuf[65];
    hex_encode(digest, hexbuf);

    if (outlen < 65) { out[0] = '\0'; return; }
    memcpy(out, hexbuf, 65);
}

void flux_sha256_string(const char *s, char *out, size_t outlen) {
    out[0] = '\0';
    if (outlen < 65) return;

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)s, strlen(s));

    uint8_t digest[32];
    sha256_final(&ctx, digest);

    char hexbuf[65];
    hex_encode(digest, hexbuf);
    memcpy(out, hexbuf, 65);
}
