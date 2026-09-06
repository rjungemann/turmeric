/* sha256.c -- streaming SHA-256 (FIPS 180-4).  See sha256.h for why. */

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(uint32_t st[8], const uint8_t *blk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)
             | ((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ((w[i-15]>>7)|(w[i-15]<<25))^((w[i-15]>>18)|(w[i-15]<<14))^(w[i-15]>>3);
        uint32_t s1 = ((w[i-2]>>17)|(w[i-2]<<15))^((w[i-2]>>19)|(w[i-2]<<13))^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1=((e>>6)|(e<<26))^((e>>11)|(e<<21))^((e>>25)|(e<<7));
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+K[i]+w[i];
        uint32_t S0=((a>>2)|(a<<30))^((a>>13)|(a<<19))^((a>>22)|(a<<10));
        uint32_t mj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

void tur_sha256_init(TurSha256 *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->nbytes = 0;
    ctx->blocklen = 0;
}

void tur_sha256_update(TurSha256 *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->nbytes += len;
    if (ctx->blocklen > 0) {
        size_t want = 64 - ctx->blocklen;
        size_t take = len < want ? len : want;
        memcpy(ctx->block + ctx->blocklen, p, take);
        ctx->blocklen += take;
        p += take;
        len -= take;
        if (ctx->blocklen < 64) return;
        sha256_block(ctx->state, ctx->block);
        ctx->blocklen = 0;
    }
    while (len >= 64) { sha256_block(ctx->state, p); p += 64; len -= 64; }
    if (len > 0) { memcpy(ctx->block, p, len); ctx->blocklen = len; }
}

void tur_sha256_final(TurSha256 *ctx, uint8_t out[TUR_SHA256_DIGEST_LEN]) {
    uint64_t bits = ctx->nbytes * 8;
    uint8_t pad[72];
    /* 0x80, then zeros so that (nbytes + padlen) % 64 == 56, then the length. */
    size_t padlen = (ctx->blocklen < 56) ? (56 - ctx->blocklen) : (120 - ctx->blocklen);
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int j = 0; j < 8; j++) pad[padlen + j] = (uint8_t)(bits >> (8 * (7 - j)));
    /* nbytes must not count the padding, so write it back after the update. */
    uint64_t saved = ctx->nbytes;
    tur_sha256_update(ctx, pad, padlen + 8);
    ctx->nbytes = saved;
    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(ctx->state[j] >> 24);
        out[j*4+1] = (uint8_t)(ctx->state[j] >> 16);
        out[j*4+2] = (uint8_t)(ctx->state[j] >> 8);
        out[j*4+3] = (uint8_t)(ctx->state[j]);
    }
}

void tur_sha256_hex(const uint8_t digest[TUR_SHA256_DIGEST_LEN], char out[65]) {
    static const char hexdig[] = "0123456789abcdef";
    for (int i = 0; i < TUR_SHA256_DIGEST_LEN; i++) {
        out[i*2]     = hexdig[(digest[i] >> 4) & 0xf];
        out[i*2 + 1] = hexdig[digest[i] & 0xf];
    }
    out[64] = '\0';
}

bool tur_sha256_file(const char *path, uint8_t out[TUR_SHA256_DIGEST_LEN]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    TurSha256 ctx;
    tur_sha256_init(&ctx);
    uint8_t buf[64 * 1024];
    for (;;) {
        size_t got = fread(buf, 1, sizeof buf, f);
        if (got > 0) tur_sha256_update(&ctx, buf, got);
        if (got < sizeof buf) break;
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
    tur_sha256_final(&ctx, out);
    return true;
}
