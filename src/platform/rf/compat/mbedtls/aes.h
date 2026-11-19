/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * mbedtls AES shim mapping the small surface used by rf_keeloq.cpp
 * (AES-256-CBC decrypt of the embedded manufacturer keystore) onto the
 * vendored tiny-AES-c backend, so the Bruce source compiles unchanged on Linux.
 */
#pragma once
#include "../tiny_aes/aes.h"
#include <string.h>

#ifndef MBEDTLS_AES_DECRYPT
#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0
#endif

typedef struct {
    uint8_t key[32];
    unsigned int keybits;
} mbedtls_aes_context;

static inline void mbedtls_aes_init(mbedtls_aes_context *ctx) {
    if (ctx) memset(ctx, 0, sizeof(*ctx));
}
static inline void mbedtls_aes_free(mbedtls_aes_context *ctx) {
    if (ctx) memset(ctx, 0, sizeof(*ctx));
}
static inline int mbedtls_aes_setkey_dec(mbedtls_aes_context *ctx, const unsigned char *key,
                                         unsigned int keybits) {
    if (!ctx || keybits != 256) return -1;
    memcpy(ctx->key, key, 32);
    ctx->keybits = keybits;
    return 0;
}
static inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key,
                                         unsigned int keybits) {
    return mbedtls_aes_setkey_dec(ctx, key, keybits);
}
// CBC: decrypt only (what KeeLoq needs). input/output may differ; IV mutated.
static inline int mbedtls_aes_crypt_cbc(mbedtls_aes_context *ctx, int mode, size_t length,
                                        unsigned char iv[16], const unsigned char *input,
                                        unsigned char *output) {
    if (!ctx || (length % 16) != 0) return -1;
    struct AES_ctx c;
    AES_init_ctx_iv(&c, ctx->key, iv);
    if (input != output) memcpy(output, input, length);
    if (mode == MBEDTLS_AES_DECRYPT) {
        AES_CBC_decrypt_buffer(&c, output, length);
    } else {
        AES_CBC_encrypt_buffer(&c, output, length);
    }
    memcpy(iv, c.Iv, 16);
    return 0;
}
