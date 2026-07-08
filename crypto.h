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

#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>

/* ==========================================================================
 * Cryptographic primitives and the PDF standard security handler.
 *
 * Enough to read (and re-encrypt) PDFs protected with the standard handler and
 * an empty user password -- the common "secured form" case. Supports RC4
 * (V1/V2, R2/R3), AESV2/AES-128 (V4/R4) and AESV3/AES-256 (V5/R6).
 * ========================================================================== */

/* --- hashes --- */
void md5(const unsigned char *data, size_t len, unsigned char out[16]);
void sha256(const unsigned char *data, size_t len, unsigned char out[32]);
void sha384(const unsigned char *data, size_t len, unsigned char out[48]);
void sha512(const unsigned char *data, size_t len, unsigned char out[64]);

/* --- RC4 (symmetric: same call encrypts and decrypts) --- */
void rc4(const unsigned char *key, int keylen,
         const unsigned char *in, size_t len, unsigned char *out);

/* --- AES-CBC. keylen is 16 (AES-128) or 32 (AES-256). ---
 * *_nopad variants take an explicit iv and do no padding (whole blocks only).
 * The padded decrypt reads the 16-byte iv from the front of `in` and strips
 * PKCS#7 padding; returns plaintext length or -1 on malformed input.
 * The padded encrypt writes iv(16) || ciphertext and returns the total length. */
int aes_cbc_decrypt(const unsigned char *key, int keylen,
                    const unsigned char *in, size_t len, unsigned char *out);
int aes_cbc_encrypt(const unsigned char *key, int keylen, const unsigned char *iv,
                    const unsigned char *in, size_t len, unsigned char *out);
void aes_cbc_decrypt_nopad(const unsigned char *key, int keylen, const unsigned char *iv,
                           const unsigned char *in, size_t len, unsigned char *out);
void aes_cbc_encrypt_nopad(const unsigned char *key, int keylen, const unsigned char *iv,
                           const unsigned char *in, size_t len, unsigned char *out);

/* --- PDF standard security handler --- */
typedef struct {
    int V, R;
    int key_len;              // file key length in bytes (5, 16 or 32)
    int cfm;                  // stream/string cipher: CFM_RC4 / CFM_AESV2 / CFM_AESV3
    unsigned char key[32];    // derived file encryption key
} PdfCrypt;

enum { CFM_RC4 = 0, CFM_AESV2 = 1, CFM_AESV3 = 2 };

/* Parse the /Encrypt dictionary text and the first /ID element, then derive the
 * file key for the empty user password. Returns 1 on success (c->key ready),
 * 0 if the handler is unsupported or authentication fails. */
int pdf_crypt_init(PdfCrypt *c, const char *encrypt_dict,
                   const unsigned char *id0, size_t id0_len);

/* Decrypt `len` bytes belonging to object (num,gen) into `out` (caller supplies
 * a buffer of at least `len` bytes). Returns the plaintext length. */
int pdf_decrypt(const PdfCrypt *c, int num, int gen,
                const unsigned char *in, size_t len, unsigned char *out);

/* Encrypt `len` bytes for object (num,gen) into `out`. For AES the output is up
 * to len + 32 bytes (iv + padding), so `out_cap` must allow for it. Returns the
 * ciphertext length, or -1 if it would not fit. */
int pdf_encrypt(const PdfCrypt *c, int num, int gen,
                const unsigned char *in, size_t len, unsigned char *out, size_t out_cap);

/* Return a new malloc'd copy of dictionary text `dict` with every literal/hex
 * string decrypted for object (num,gen), each string keeping its original
 * delimiter (literal stays literal, hex stays hex). Names/numbers are untouched. */
char *pdf_decrypt_dict_strings(const PdfCrypt *c, int num, int gen, const char *dict);

/* Inverse of the above: encrypt every string in `dict` for object (num,gen),
 * emitting each as a (binary-safe) hex string. Use when re-emitting an object
 * whose strings were decrypted on read, into a still-encrypted document. */
char *pdf_encrypt_dict_strings(const PdfCrypt *c, int num, int gen, const char *dict);

#endif
