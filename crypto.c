/*
 * ffpdf — read and fill PDF form fields.
 * Copyright (C) 2026 Page Brooks
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at <http://www.apache.org/licenses/LICENSE-2.0>. Distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND; see the
 * License for the specific language governing permissions and limitations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "crypto.h"
#include "pdf_lex.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ==========================================================================
 * MD5 (RFC 1321)
 * ========================================================================== */
typedef struct { unsigned int a, b, c, d; unsigned long long len; unsigned char buf[64]; size_t n; } MD5ctx;

static unsigned int md5_rl(unsigned int x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(MD5ctx *ctx, const unsigned char *p) {
    static const unsigned int K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    unsigned int M[16];
    for (int i = 0; i < 16; i++)
        M[i] = p[i*4] | (p[i*4+1]<<8) | (p[i*4+2]<<16) | ((unsigned)p[i*4+3]<<24);
    unsigned int A = ctx->a, B = ctx->b, C = ctx->c, D = ctx->d;
    for (int i = 0; i < 64; i++) {
        unsigned int F; int g;
        if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
        else if (i < 32) { F = (D & B) | (~D & C);        g = (5*i + 1) & 15; }
        else if (i < 48) { F = B ^ C ^ D;                 g = (3*i + 5) & 15; }
        else             { F = C ^ (B | ~D);              g = (7*i) & 15; }
        F = F + A + K[i] + M[g];
        A = D; D = C; C = B; B = B + md5_rl(F, S[i]);
    }
    ctx->a += A; ctx->b += B; ctx->c += C; ctx->d += D;
}

void md5(const unsigned char *data, size_t len, unsigned char out[16]) {
    MD5ctx ctx = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, {0}, 0 };
    ctx.len = (unsigned long long)len * 8;
    while (len >= 64) { md5_block(&ctx, data); data += 64; len -= 64; }
    unsigned char tail[128]; size_t t = 0;
    memcpy(tail, data, len); t = len;
    tail[t++] = 0x80;
    size_t pad = (t <= 56) ? (56 - t) : (120 - t);
    memset(tail + t, 0, pad); t += pad;
    for (int i = 0; i < 8; i++) tail[t++] = (unsigned char)(ctx.len >> (8*i));
    for (size_t i = 0; i < t; i += 64) md5_block(&ctx, tail + i);
    unsigned int v[4] = { ctx.a, ctx.b, ctx.c, ctx.d };
    for (int i = 0; i < 4; i++) {
        out[i*4]   = (unsigned char)(v[i]);
        out[i*4+1] = (unsigned char)(v[i] >> 8);
        out[i*4+2] = (unsigned char)(v[i] >> 16);
        out[i*4+3] = (unsigned char)(v[i] >> 24);
    }
}

/* ==========================================================================
 * SHA-256
 * ========================================================================== */
static unsigned int sha_rr(unsigned int x, int c) { return (x >> c) | (x << (32 - c)); }

void sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
    static const unsigned int K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    unsigned int h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    size_t total = len;
    unsigned char *msg = malloc(len + 72);
    memcpy(msg, data, len);
    size_t ml = len;
    msg[ml++] = 0x80;
    while (ml % 64 != 56) msg[ml++] = 0;
    unsigned long long bits = (unsigned long long)total * 8;
    for (int i = 7; i >= 0; i--) msg[ml++] = (unsigned char)(bits >> (8*i));
    for (size_t off = 0; off < ml; off += 64) {
        unsigned int w[64];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = msg + off + i*4;
            w[i] = ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];
        }
        for (int i = 16; i < 64; i++) {
            unsigned int s0 = sha_rr(w[i-15],7) ^ sha_rr(w[i-15],18) ^ (w[i-15]>>3);
            unsigned int s1 = sha_rr(w[i-2],17) ^ sha_rr(w[i-2],19) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned int a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],ff=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            unsigned int S1 = sha_rr(e,6)^sha_rr(e,11)^sha_rr(e,25);
            unsigned int ch = (e&ff)^(~e&g);
            unsigned int t1 = hh + S1 + ch + K[i] + w[i];
            unsigned int S0 = sha_rr(a,2)^sha_rr(a,13)^sha_rr(a,22);
            unsigned int maj = (a&b)^(a&c)^(b&c);
            unsigned int t2 = S0 + maj;
            hh=g; g=ff; ff=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=ff;h[6]+=g;h[7]+=hh;
    }
    free(msg);
    for (int i = 0; i < 8; i++) {
        out[i*4]=(unsigned char)(h[i]>>24); out[i*4+1]=(unsigned char)(h[i]>>16);
        out[i*4+2]=(unsigned char)(h[i]>>8); out[i*4+3]=(unsigned char)h[i];
    }
}

/* ==========================================================================
 * SHA-512 / SHA-384 (64-bit)
 * ========================================================================== */
static unsigned long long sha_rr64(unsigned long long x, int c) { return (x >> c) | (x << (64 - c)); }

static void sha512_core(const unsigned char *data, size_t len, unsigned long long h[8], unsigned char *out, int outbytes) {
    static const unsigned long long K[80] = {
        0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
        0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
        0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
        0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };
    size_t total = len;
    size_t alloc = len + 144;
    unsigned char *msg = malloc(alloc);
    memcpy(msg, data, len);
    size_t ml = len;
    msg[ml++] = 0x80;
    while (ml % 128 != 112) msg[ml++] = 0;
    for (int i = 0; i < 8; i++) msg[ml++] = 0;                 // high 64 bits of length (always 0 here)
    unsigned long long bits = (unsigned long long)total * 8;
    for (int i = 7; i >= 0; i--) msg[ml++] = (unsigned char)(bits >> (8*i));
    for (size_t off = 0; off < ml; off += 128) {
        unsigned long long w[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = msg + off + i*8;
            w[i] = 0;
            for (int j = 0; j < 8; j++) w[i] = (w[i]<<8) | p[j];
        }
        for (int i = 16; i < 80; i++) {
            unsigned long long s0 = sha_rr64(w[i-15],1) ^ sha_rr64(w[i-15],8) ^ (w[i-15]>>7);
            unsigned long long s1 = sha_rr64(w[i-2],19) ^ sha_rr64(w[i-2],61) ^ (w[i-2]>>6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned long long a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],ff=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 80; i++) {
            unsigned long long S1 = sha_rr64(e,14)^sha_rr64(e,18)^sha_rr64(e,41);
            unsigned long long ch = (e&ff)^(~e&g);
            unsigned long long t1 = hh + S1 + ch + K[i] + w[i];
            unsigned long long S0 = sha_rr64(a,28)^sha_rr64(a,34)^sha_rr64(a,39);
            unsigned long long maj = (a&b)^(a&c)^(b&c);
            unsigned long long t2 = S0 + maj;
            hh=g; g=ff; ff=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=ff;h[6]+=g;h[7]+=hh;
    }
    free(msg);
    for (int i = 0; i < outbytes/8 + (outbytes%8?1:0); i++) {
        for (int j = 0; j < 8 && i*8+j < outbytes; j++)
            out[i*8+j] = (unsigned char)(h[i] >> (56 - 8*j));
    }
}

void sha512(const unsigned char *data, size_t len, unsigned char out[64]) {
    unsigned long long h[8] = { 0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                                0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    sha512_core(data, len, h, out, 64);
}

void sha384(const unsigned char *data, size_t len, unsigned char out[48]) {
    unsigned long long h[8] = { 0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
                                0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL };
    sha512_core(data, len, h, out, 48);
}

/* ==========================================================================
 * RC4
 * ========================================================================== */
void rc4(const unsigned char *key, int keylen, const unsigned char *in, size_t len, unsigned char *out) {
    unsigned char s[256];
    for (int i = 0; i < 256; i++) s[i] = (unsigned char)i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + s[i] + key[i % keylen]) & 0xff;
        unsigned char t = s[i]; s[i] = s[j]; s[j] = t;
    }
    int a = 0, b = 0;
    for (size_t k = 0; k < len; k++) {
        a = (a + 1) & 0xff;
        b = (b + s[a]) & 0xff;
        unsigned char t = s[a]; s[a] = s[b]; s[b] = t;
        out[k] = in[k] ^ s[(s[a] + s[b]) & 0xff];
    }
}

/* ==========================================================================
 * AES-128 / AES-256 (FIPS-197), CBC mode
 * ========================================================================== */
static const unsigned char AES_SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static unsigned char AES_INV[256];
static int aes_inv_ready = 0;
static void aes_build_inv(void) {
    if (aes_inv_ready) return;
    for (int i = 0; i < 256; i++) AES_INV[AES_SBOX[i]] = (unsigned char)i;
    aes_inv_ready = 1;
}

static unsigned char aes_xtime(unsigned char x) { return (unsigned char)((x << 1) ^ ((x >> 7) * 0x1b)); }
static unsigned char aes_mul(unsigned char a, unsigned char b) {
    unsigned char r = 0;
    while (b) { if (b & 1) r ^= a; a = aes_xtime(a); b >>= 1; }
    return r;
}

typedef struct { unsigned char rk[240]; int nr; } AESkey;

static void aes_expand(const unsigned char *key, int keybytes, AESkey *ks) {
    int nk = keybytes / 4;
    ks->nr = nk + 6;
    static const unsigned char rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    int total = 4 * (ks->nr + 1);                 // words
    memcpy(ks->rk, key, keybytes);
    unsigned char *w = ks->rk;
    for (int i = nk; i < total; i++) {
        unsigned char t[4];
        memcpy(t, w + (i-1)*4, 4);
        if (i % nk == 0) {
            unsigned char tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;   // RotWord
            for (int j = 0; j < 4; j++) t[j] = AES_SBOX[t[j]];                      // SubWord
            t[0] ^= rcon[i/nk];
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = AES_SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++) w[i*4+j] = w[(i-nk)*4+j] ^ t[j];
    }
}

static void aes_encrypt_block(const AESkey *ks, unsigned char *s) {
    const unsigned char *rk = ks->rk;
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= ks->nr; round++) {
        for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];                        // SubBytes
        unsigned char t;                                                           // ShiftRows
        t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
        if (round < ks->nr) {                                                      // MixColumns
            for (int c = 0; c < 4; c++) {
                unsigned char *col = s + c*4;
                unsigned char a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=aes_xtime(a0)^(aes_xtime(a1)^a1)^a2^a3;
                col[1]=a0^aes_xtime(a1)^(aes_xtime(a2)^a2)^a3;
                col[2]=a0^a1^aes_xtime(a2)^(aes_xtime(a3)^a3);
                col[3]=(aes_xtime(a0)^a0)^a1^a2^aes_xtime(a3);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16 + i];                     // AddRoundKey
    }
}

static void aes_decrypt_block(const AESkey *ks, unsigned char *s) {
    aes_build_inv();
    const unsigned char *rk = ks->rk;
    for (int i = 0; i < 16; i++) s[i] ^= rk[ks->nr*16 + i];
    for (int round = ks->nr - 1; round >= 0; round--) {
        unsigned char t;                                                           // InvShiftRows
        t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
        for (int i = 0; i < 16; i++) s[i] = AES_INV[s[i]];                         // InvSubBytes
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16 + i];                     // AddRoundKey
        if (round > 0) {                                                           // InvMixColumns
            for (int c = 0; c < 4; c++) {
                unsigned char *col = s + c*4;
                unsigned char a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=aes_mul(a0,14)^aes_mul(a1,11)^aes_mul(a2,13)^aes_mul(a3,9);
                col[1]=aes_mul(a0,9)^aes_mul(a1,14)^aes_mul(a2,11)^aes_mul(a3,13);
                col[2]=aes_mul(a0,13)^aes_mul(a1,9)^aes_mul(a2,14)^aes_mul(a3,11);
                col[3]=aes_mul(a0,11)^aes_mul(a1,13)^aes_mul(a2,9)^aes_mul(a3,14);
            }
        }
    }
}

void aes_cbc_decrypt_nopad(const unsigned char *key, int keylen, const unsigned char *iv,
                           const unsigned char *in, size_t len, unsigned char *out) {
    AESkey ks; aes_expand(key, keylen, &ks);
    unsigned char prev[16]; memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        unsigned char block[16]; memcpy(block, in + off, 16);
        unsigned char dec[16]; memcpy(dec, block, 16);
        aes_decrypt_block(&ks, dec);
        for (int i = 0; i < 16; i++) out[off + i] = dec[i] ^ prev[i];
        memcpy(prev, block, 16);
    }
}

void aes_cbc_encrypt_nopad(const unsigned char *key, int keylen, const unsigned char *iv,
                           const unsigned char *in, size_t len, unsigned char *out) {
    AESkey ks; aes_expand(key, keylen, &ks);
    unsigned char prev[16]; memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        unsigned char block[16];
        for (int i = 0; i < 16; i++) block[i] = in[off + i] ^ prev[i];
        aes_encrypt_block(&ks, block);
        memcpy(out + off, block, 16);
        memcpy(prev, block, 16);
    }
}

int aes_cbc_decrypt(const unsigned char *key, int keylen,
                    const unsigned char *in, size_t len, unsigned char *out) {
    if (len < 32 || (len % 16) != 0) return -1;      // need iv + >=1 block, block-aligned
    aes_cbc_decrypt_nopad(key, keylen, in, in + 16, len - 16, out);
    size_t plen = len - 16;
    unsigned char pad = out[plen - 1];               // strip PKCS#7
    if (pad < 1 || pad > 16 || pad > plen) return (int)plen;   // tolerate bad padding
    return (int)(plen - pad);
}

int aes_cbc_encrypt(const unsigned char *key, int keylen, const unsigned char *iv,
                    const unsigned char *in, size_t len, unsigned char *out) {
    memcpy(out, iv, 16);
    unsigned char pad = (unsigned char)(16 - (len % 16));      // PKCS#7 (always 1..16)
    size_t padded = len + pad;
    unsigned char *buf = malloc(padded);
    memcpy(buf, in, len);
    memset(buf + len, pad, pad);
    aes_cbc_encrypt_nopad(key, keylen, iv, buf, padded, out + 16);
    free(buf);
    return (int)(16 + padded);
}

/* ==========================================================================
 * PDF standard security handler
 * ========================================================================== */
static const unsigned char PDF_PAD[32] = {
    0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A };

// Parse a hex string "<...>" starting at *p (which points just past '<') into
// buf; returns byte count. Also handles literal "(...)" if p points at '('.
static int parse_pdf_bytes(const char *v, unsigned char *buf, int cap) {
    if (*v == '<') {
        v++;
        int n = 0, hi = -1;
        for (; *v && *v != '>' && n < cap; v++) {
            int d;
            if (*v >= '0' && *v <= '9') d = *v - '0';
            else if (*v >= 'a' && *v <= 'f') d = *v - 'a' + 10;
            else if (*v >= 'A' && *v <= 'F') d = *v - 'A' + 10;
            else continue;
            if (hi < 0) hi = d;
            else { buf[n++] = (unsigned char)(hi*16 + d); hi = -1; }
        }
        if (hi >= 0 && n < cap) buf[n++] = (unsigned char)(hi*16);
        return n;
    }
    if (*v == '(') {                                  // literal string with escapes
        v++;
        int n = 0;
        while (*v && *v != ')' && n < cap) {
            if (*v == '\\' && v[1]) { v++; buf[n++] = (unsigned char)*v++; continue; }
            buf[n++] = (unsigned char)*v++;
        }
        return n;
    }
    return 0;
}

// R6 hardened hash (ISO 32000-2 Algorithm 2.B). `pw` is the password bytes,
// `salt` is 8 bytes, `udata` is the 48-byte U string for owner auth (empty/NULL
// with ulen 0 for user auth). Writes 32 bytes to out.
static void hash_r6(const unsigned char *pw, size_t pwlen, const unsigned char *salt,
                    const unsigned char *udata, size_t ulen, unsigned char out[32]) {
    unsigned char K[64];
    int klen = 32;
    {
        unsigned char in[128 + 64], *q = in;
        memcpy(q, pw, pwlen); q += pwlen;
        memcpy(q, salt, 8);   q += 8;
        if (ulen) { memcpy(q, udata, ulen); q += ulen; }
        sha256(in, (size_t)(q - in), K);
    }
    for (int round = 0; ; round++) {
        // K1 = (pw || K || udata) x 64
        size_t unit = pwlen + klen + ulen;
        unsigned char *K1 = malloc(unit * 64);
        for (int i = 0; i < 64; i++) {
            unsigned char *q = K1 + i*unit;
            memcpy(q, pw, pwlen); q += pwlen;
            memcpy(q, K, klen);   q += klen;
            if (ulen) memcpy(q, udata, ulen);
        }
        // E = AES-128-CBC-encrypt(key=K[0:16], iv=K[16:32], K1), no padding
        unsigned char *E = malloc(unit * 64);
        aes_cbc_encrypt_nopad(K, 16, K + 16, K1, unit * 64, E);
        int mod = 0;
        for (int i = 0; i < 16; i++) mod += E[i];
        mod %= 3;
        if (mod == 0) { sha256(E, unit*64, K); klen = 32; }
        else if (mod == 1) { sha384(E, unit*64, K); klen = 48; }
        else { sha512(E, unit*64, K); klen = 64; }
        unsigned char last = E[unit*64 - 1];
        free(K1); free(E);
        // Spec (Algorithm 2.B) stops when the 1-based round number r >= 64 and
        // the last byte of E <= r - 32. With this 0-based counter r = round + 1,
        // so the threshold is round - 31 (>= 64 rounds => round >= 63).
        if (round >= 63 && last <= (unsigned char)(round - 31)) break;
    }
    memcpy(out, K, 32);
}

int pdf_crypt_init(PdfCrypt *c, const char *enc, const unsigned char *id0, size_t id0_len) {
    memset(c, 0, sizeof(*c));
    const char *p;
    c->V = (p = dict_find(enc, "/V")) ? atoi(p) : 0;
    c->R = (p = dict_find(enc, "/R")) ? atoi(p) : 0;
    int length_bits = (p = dict_find(enc, "/Length")) ? atoi(p) : 40;
    c->key_len = length_bits / 8;

    // Cipher method: V<=3 is always RC4; V4/V5 read /CFM from the crypt filter.
    c->cfm = CFM_RC4;
    if (c->V >= 4) {
        const char *cfm = dict_find(enc, "/CFM");
        if (cfm && !strncmp(cfm, "/AESV2", 6)) { c->cfm = CFM_AESV2; c->key_len = 16; }
        else if (cfm && !strncmp(cfm, "/AESV3", 6)) { c->cfm = CFM_AESV3; c->key_len = 32; }
        else if (cfm && !strncmp(cfm, "/V2", 3)) { c->cfm = CFM_RC4; }
    }

    unsigned char O[48], U[48];
    const char *po = dict_find(enc, "/O"), *pu = dict_find(enc, "/U");
    if (!po || !pu) return 0;
    int olen = parse_pdf_bytes(po, O, sizeof(O));
    int ulen = parse_pdf_bytes(pu, U, sizeof(U));
    (void)ulen;

    if (c->R >= 5) {
        // R6 (AES-256): validate empty user password and unwrap the file key.
        // U layout: hash(32) || validation salt(8) || key salt(8).
        unsigned char test[32];
        unsigned char vsalt_in[8]; memcpy(vsalt_in, U + 32, 8);
        hash_r6((const unsigned char *)"", 0, vsalt_in, NULL, 0, test);
        if (memcmp(test, U, 32) != 0) return 0;              // empty user pw rejected
        unsigned char ik[32];
        hash_r6((const unsigned char *)"", 0, U + 40, NULL, 0, ik);
        const char *pue = dict_find(enc, "/UE");
        unsigned char UE[32];
        if (!pue || parse_pdf_bytes(pue, UE, sizeof(UE)) != 32) return 0;
        unsigned char iv[16] = {0};
        aes_cbc_decrypt_nopad(ik, 32, iv, UE, 32, c->key);   // file key = 32 bytes
        c->key_len = 32;
        c->cfm = CFM_AESV3;   // R>=5 is AES-256; force it so pdf_decrypt never
                              // routes a 32-byte key into object_key (stack smash)
        return 1;
    }

    // R2-4 (Algorithm 2), empty user password.
    int em = 1;                                              // /EncryptMetadata (default true)
    const char *pe = dict_find(enc, "/EncryptMetadata");
    if (pe && !strncmp(pe, "false", 5)) em = 0;
    int P = (p = dict_find(enc, "/P")) ? (int)strtol(p, NULL, 10) : 0;

    unsigned char buf[32 + 48 + 4 + 64 + 4];
    int n = 0;
    memcpy(buf + n, PDF_PAD, 32); n += 32;                   // padded empty password
    memcpy(buf + n, O, olen < 32 ? olen : 32); n += (olen < 32 ? olen : 32);
    buf[n++] = (unsigned char)(P);
    buf[n++] = (unsigned char)(P >> 8);
    buf[n++] = (unsigned char)(P >> 16);
    buf[n++] = (unsigned char)(P >> 24);
    memcpy(buf + n, id0, id0_len); n += (int)id0_len;
    if (c->R >= 4 && !em) { buf[n++]=0xff; buf[n++]=0xff; buf[n++]=0xff; buf[n++]=0xff; }

    // Clamp BEFORE key_len is used as a length: /Length is attacker-controlled,
    // so a huge or negative value would make md5(h, key_len, ...) read off the
    // end of the 16-byte h[] buffer.
    if (c->key_len < 5) c->key_len = 5;
    if (c->key_len > 16) c->key_len = 16;
    unsigned char h[16];
    md5(buf, n, h);
    if (c->R >= 3) for (int i = 0; i < 50; i++) md5(h, c->key_len, h);
    memcpy(c->key, h, c->key_len);
    return 1;
}

// Derive the per-object key (Algorithm 1) for RC4/AESV2 into `ok`, returns len.
static int object_key(const PdfCrypt *c, int num, int gen, unsigned char ok[16]) {
    unsigned char in[16 + 5 + 4];
    int n = c->key_len;
    if (n > 16) n = 16;   // defense in depth: object_key is only for <=16-byte
                          // RC4/AESV2 keys; never overflow in[] with a 32-byte key
    memcpy(in, c->key, n);
    in[n++] = (unsigned char)(num);
    in[n++] = (unsigned char)(num >> 8);
    in[n++] = (unsigned char)(num >> 16);
    in[n++] = (unsigned char)(gen);
    in[n++] = (unsigned char)(gen >> 8);
    if (c->cfm == CFM_AESV2) { in[n++]='s'; in[n++]='A'; in[n++]='l'; in[n++]='T'; }
    unsigned char h[16];
    md5(in, n, h);
    int oklen = c->key_len + 5;
    if (oklen > 16) oklen = 16;
    memcpy(ok, h, oklen);
    return oklen;
}

int pdf_decrypt(const PdfCrypt *c, int num, int gen,
                const unsigned char *in, size_t len, unsigned char *out) {
    if (c->cfm == CFM_AESV3) {
        int r = aes_cbc_decrypt(c->key, 32, in, len, out);
        return r < 0 ? 0 : r;
    }
    unsigned char ok[16];
    int oklen = object_key(c, num, gen, ok);
    if (c->cfm == CFM_AESV2) {
        int r = aes_cbc_decrypt(ok, oklen, in, len, out);
        return r < 0 ? 0 : r;
    }
    rc4(ok, oklen, in, len, out);
    return (int)len;
}

char *pdf_decrypt_dict_strings(const PdfCrypt *c, int num, int gen, const char *dict) {
    size_t len = strlen(dict);
    size_t cap = len + 64, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
#define PUT(ch) do { if (n + 1 >= cap) { cap = cap*2 + 16; out = realloc(out, cap); } out[n++] = (char)(ch); } while (0)
    const char *p = dict;
    while (*p) {
        if (p[0] == '<' && p[1] == '<') { PUT('<'); PUT('<'); p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { PUT('>'); PUT('>'); p += 2; continue; }
        if (*p == '(' || *p == '<') {                 // a literal or hex string value
            int hex = (*p == '<');
            const char *q = p + 1;
            unsigned char *ct = malloc(len + 1);
            size_t cl = 0;
            if (hex) {
                int hi = -1;
                while (*q && *q != '>') {
                    int d; char ch = *q++;
                    if (ch >= '0' && ch <= '9') d = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                    else continue;
                    if (hi < 0) hi = d; else { ct[cl++] = (unsigned char)(hi*16 + d); hi = -1; }
                }
                if (hi >= 0) ct[cl++] = (unsigned char)(hi*16);
                if (*q == '>') q++;
            } else {
                int sdepth = 1;
                while (*q && sdepth > 0) {
                    if (*q == '\\' && q[1]) { q++; ct[cl++] = (unsigned char)*q++; continue; }
                    if (*q == '(') { sdepth++; ct[cl++] = '('; q++; continue; }
                    if (*q == ')') { sdepth--; if (sdepth == 0) { q++; break; } ct[cl++] = ')'; q++; continue; }
                    ct[cl++] = (unsigned char)*q++;
                }
            }
            unsigned char *pt = malloc(cl + 1);
            int pl = pdf_decrypt(c, num, gen, ct, cl, pt);
            if (pl < 0) pl = 0;
            if (hex) {
                PUT('<');
                static const char HX[] = "0123456789abcdef";
                for (int i = 0; i < pl; i++) { PUT(HX[pt[i] >> 4]); PUT(HX[pt[i] & 15]); }
                PUT('>');
            } else {
                PUT('(');
                for (int i = 0; i < pl; i++) {
                    unsigned char b = pt[i];
                    if (b == '(' || b == ')' || b == '\\') PUT('\\');
                    PUT(b);
                }
                PUT(')');
            }
            free(ct); free(pt);
            p = q;
            continue;
        }
        PUT(*p); p++;
    }
    out[n] = '\0';
    return out;
#undef PUT
}

char *pdf_encrypt_dict_strings(const PdfCrypt *c, int num, int gen, const char *dict) {
    size_t len = strlen(dict);
    size_t cap = len * 2 + 64, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    static const char HX[] = "0123456789abcdef";
#define PUT(ch) do { if (n + 1 >= cap) { cap = cap*2 + 16; out = realloc(out, cap); } out[n++] = (char)(ch); } while (0)
    const char *p = dict;
    while (*p) {
        if (p[0] == '<' && p[1] == '<') { PUT('<'); PUT('<'); p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { PUT('>'); PUT('>'); p += 2; continue; }
        if (*p == '(' || *p == '<') {                 // a string value -> encrypt
            int hex = (*p == '<');
            const char *q = p + 1;
            unsigned char *pt = malloc(len + 1);
            size_t pl = 0;
            if (hex) {
                int hi = -1;
                while (*q && *q != '>') {
                    int d; char ch = *q++;
                    if (ch >= '0' && ch <= '9') d = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                    else continue;
                    if (hi < 0) hi = d; else { pt[pl++] = (unsigned char)(hi*16 + d); hi = -1; }
                }
                if (hi >= 0) pt[pl++] = (unsigned char)(hi*16);
                if (*q == '>') q++;
            } else {
                int sdepth = 1;
                while (*q && sdepth > 0) {
                    if (*q == '\\' && q[1]) { q++; pt[pl++] = (unsigned char)*q++; continue; }
                    if (*q == '(') { sdepth++; pt[pl++] = '('; q++; continue; }
                    if (*q == ')') { sdepth--; if (sdepth == 0) { q++; break; } pt[pl++] = ')'; q++; continue; }
                    pt[pl++] = (unsigned char)*q++;
                }
            }
            unsigned char *ct = malloc(pl + 40);
            int cl = pdf_encrypt(c, num, gen, pt, pl, ct, pl + 40);
            if (cl < 0) cl = 0;
            PUT('<');
            for (int i = 0; i < cl; i++) { PUT(HX[ct[i] >> 4]); PUT(HX[ct[i] & 15]); }
            PUT('>');
            free(pt); free(ct);
            p = q;
            continue;
        }
        PUT(*p); p++;
    }
    out[n] = '\0';
    return out;
#undef PUT
}

int pdf_encrypt(const PdfCrypt *c, int num, int gen,
                const unsigned char *in, size_t len, unsigned char *out, size_t out_cap) {
    if (c->cfm == CFM_AESV3) {
        if (out_cap < len + 32) return -1;
        unsigned char iv[16];
        for (int i = 0; i < 16; i++) iv[i] = (unsigned char)((num * 2654435761u + gen * 40503u + i * 97u) & 0xff);
        return aes_cbc_encrypt(c->key, 32, iv, in, len, out);
    }
    unsigned char ok[16];
    int oklen = object_key(c, num, gen, ok);
    if (c->cfm == CFM_AESV2) {
        if (out_cap < len + 32) return -1;
        unsigned char iv[16];
        for (int i = 0; i < 16; i++) iv[i] = (unsigned char)((num * 2654435761u + gen * 40503u + i * 97u) & 0xff);
        return aes_cbc_encrypt(ok, oklen, iv, in, len, out);
    }
    if (out_cap < len) return -1;
    rc4(ok, oklen, in, len, out);
    return (int)len;
}
