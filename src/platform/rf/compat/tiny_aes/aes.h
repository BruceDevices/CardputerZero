/*
 * tiny-AES-c (https://github.com/kokke/tiny-AES-c) — Public Domain / Unlicense.
 * Vendored for the CardputerZero Linux port to provide AES-256-CBC for the
 * KeeLoq manufacturer keystore, replacing the ESP32 mbedtls backend.
 * Configured for AES256 + CBC.
 */
#ifndef _TINY_AES_H_
#define _TINY_AES_H_

#include <stdint.h>
#include <stddef.h>

#define AES256 1
#define CBC 1

#define AES_BLOCKLEN 16
#define AES_KEYLEN 32
#define AES_keyExpSize 240

struct AES_ctx {
    uint8_t RoundKey[AES_keyExpSize];
    uint8_t Iv[AES_BLOCKLEN];
};

#ifdef __cplusplus
extern "C" {
#endif

void AES_init_ctx(struct AES_ctx *ctx, const uint8_t *key);
void AES_init_ctx_iv(struct AES_ctx *ctx, const uint8_t *key, const uint8_t *iv);
void AES_ctx_set_iv(struct AES_ctx *ctx, const uint8_t *iv);

void AES_CBC_encrypt_buffer(struct AES_ctx *ctx, uint8_t *buf, size_t length);
void AES_CBC_decrypt_buffer(struct AES_ctx *ctx, uint8_t *buf, size_t length);

#ifdef __cplusplus
}
#endif

#endif
