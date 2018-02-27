/*
 * aead.c - Manage AEAD ciphers
 *
 * Copyright (C) 2013 - 2017, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>
#include <stdio.h>
#include <assert.h>

#include <sodium.h>
#include <arpa/inet.h>

#include <mbedtls/version.h>
#define CIPHER_UNSUPPORTED "unsupported"

#include "sbf.h"
#include "aead.h"
#include "utils.h"

#define NONE                    (-1)
#define AES128GCM               0
#define AES192GCM               1
#define AES256GCM               2
/*
 * methods above requires gcm context
 * methods below doesn't require it,
 * then we need to fake one
 */
#define CHACHA20POLY1305        3
#define CHACHA20POLY1305IETF    4
#define XCHACHA20POLY1305IETF   5

#define CHUNK_SIZE_LEN          2
#define CHUNK_SIZE_MASK         0x3FFF

/*
 * Designed by wongsyrone with help from breakwa11 and Noisyfox
 * Session key is only applied to TCP, UDP keeps using master key.
 *
 * Master key: blake2b(user-password)
 * Session subkey: blake2b_salt_personal(master-key, salt, info)
 *    salt is from another side with length between 16 <-> length of key
 * and we only feed 16 bytes to blake2b_salt_personal()
 *
 * The first salt is either from client or server side, it is generated randomly
 * Nonce starts from 0 and increment after each use
 *
 * Data.Len is used to separate general ciphertext and Auth tag. We can start decryption
 * if and only if the verification is passed.
 * Firstly, we do length check, then decrypt it, separate ciphertext and attached data tag
 * based on the verified length, verify data tag and decrypt the corresponding data.
 * Finally, do what you supposed to do, e.g. forward user data.
 *
 * For UDP, nonces are generated randomly without the incrementation.
 *
 * TCP request (before encryption)
 * +------+---------------------+------------------+
 * | ATYP | Destination Address | Destination Port |
 * +------+---------------------+------------------+
 * |  1   |       Variable      |         2        |
 * +------+---------------------+------------------+
 *
 * TCP request (after encryption, *ciphertext*)
 * +--------+--------------+------------------+--------------+---------------+
 * | NONCE  |  *HeaderLen* |   HeaderLen_TAG  |   *Header*   |  Header_TAG   |
 * +--------+--------------+------------------+--------------+---------------+
 * | Fixed  |       2      |       Fixed      |   Variable   |     Fixed     |
 * +--------+--------------+------------------+--------------+---------------+
 *
 * Header input: atyp + dst.addr + dst.port
 * HeaderLen is length(atyp + dst.addr + dst.port)
 * HeaderLen_TAG and Header_TAG are in plaintext
 *
 * TCP Chunk (before encryption)
 * +----------+
 * |  DATA    |
 * +----------+
 * | Variable |
 * +----------+
 *
 * Data.Len is a 16-bit big-endian integer indicating the length of DATA.
 *
 * TCP Chunk (after encryption, *ciphertext*)
 * +--------------+---------------+--------------+------------+
 * |  *DataLen*   |  DataLen_TAG  |    *Data*    |  Data_TAG  |
 * +--------------+---------------+--------------+------------+
 * |      2       |     Fixed     |   Variable   |   Fixed    |
 * +--------------+---------------+--------------+------------+
 *
 * Len_TAG and DATA_TAG have the same length, they are in plaintext.
 * After encryption, DATA -> DATA*
 *
 * UDP (before encryption)
 * +------+---------------------+------------------+----------+
 * | ATYP | Destination Address | Destination Port |   DATA   |
 * +------+---------------------+------------------+----------+
 * |  1   |       Variable      |         2        | Variable |
 * +------+---------------------+------------------+----------+
 *
 * UDP (after encryption, *ciphertext*)
 * +--------+-----------+-----------+
 * | NONCE  |  *Data*   |  Data_TAG |
 * +--------+-----------+-----------+
 * | Fixed  | Variable  |   Fixed   |
 * +--------+-----------+-----------+
 *
 * *Data* is Encrypt(atyp + dst.addr + dst.port + payload)
 * RSV and FRAG are dropped
 * Since UDP packet is either received completely or missed,
 * we don't have to keep a field to track its length.
 *
 */

/*
 * https://github.com/jedisct1/libsodium/issues/347
 *
 * Unless you need a subkey larger than 512 bits(64 bytes), just use Blake2b
 * namely crypto_generichash_blake2b_salt_personal() with the following parameters:
 *
 * in=NULL
 * inlen=0
 * key=the master key (aka PSK)
 * salt=received from other side
 * personal=an identifier for your application (similar to HKDF's "info")
 *
 * master key is the hashing result from user password
 *
 * 1. Derive subkey SK = blake2b_salt_personal(master-key, salt, "fuckshadows-g3nk")
 * 2. Send salt
 * 3. For each chunk, encrypt and authenticate payload using SK with a counting nonce
 *    (starting from 0 and increment by 1 after each use)
 * 4. Send encrypted chunk
 */

const char *supported_aead_ciphers[AEAD_CIPHER_NUM] = {
    "aes-128-gcm",
    "aes-192-gcm",
    "aes-256-gcm",
    "chacha20-poly1305",
    "chacha20-ietf-poly1305",
    "xchacha20-ietf-poly1305"
};

/*
 * use mbed TLS cipher wrapper to unify handling
 */
static const char *supported_aead_ciphers_mbedtls[AEAD_CIPHER_NUM] = {
    "AES-128-GCM",
    "AES-192-GCM",
    "AES-256-GCM",
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED
};

static const int supported_aead_ciphers_nonce_size[AEAD_CIPHER_NUM] = {
    12, 12, 12, 8, 12, 24
};

static const int supported_aead_ciphers_key_size[AEAD_CIPHER_NUM] = {
    16, 24, 32, 32, 32, 32
};

static const int supported_aead_ciphers_tag_size[AEAD_CIPHER_NUM] = {
    16, 16, 16, 16, 16, 16
};

static int
aead_cipher_encrypt(cipher_ctx_t *cipher_ctx,
                    uint8_t *c,
                    size_t *clen,
                    uint8_t *m,
                    size_t mlen,
                    uint8_t *ad,
                    size_t adlen,
                    uint8_t *n,
                    uint8_t *k)
{
    int err                      = CRYPTO_OK;
    unsigned long long long_clen = 0;

    size_t nlen = cipher_ctx->cipher->nonce_len;
    size_t tlen = cipher_ctx->cipher->tag_len;

    switch (cipher_ctx->cipher->method) {
    case AES128GCM:
    case AES192GCM:
    case AES256GCM:
        err = mbedtls_cipher_auth_encrypt(cipher_ctx->evp, n, nlen, ad, adlen,
                                          m, mlen, c, clen, c + mlen, tlen);
        *clen += tlen;
        break;
    case CHACHA20POLY1305:
        err = crypto_aead_chacha20poly1305_encrypt(c, &long_clen, m, mlen,
                                                   ad, adlen, NULL, n, k);
        *clen = (size_t)long_clen;
        break;
    case CHACHA20POLY1305IETF:
        err = crypto_aead_chacha20poly1305_ietf_encrypt(c, &long_clen, m, mlen,
                                                        ad, adlen, NULL, n, k);
        *clen = (size_t)long_clen;
        break;
    case XCHACHA20POLY1305IETF:
        err = crypto_aead_xchacha20poly1305_ietf_encrypt(c, &long_clen, m, mlen,
                                                         ad, adlen, NULL, n, k);
        *clen = (size_t)long_clen;
        break;
    default:
        return CRYPTO_ERROR;
    }

    return err;
}

static int
aead_cipher_decrypt(cipher_ctx_t *cipher_ctx,
                    uint8_t *p,
                    size_t *plen,
                    uint8_t *m,
                    size_t mlen,
                    uint8_t *ad,
                    size_t adlen,
                    uint8_t *n,
                    uint8_t *k)
{
    int err                      = CRYPTO_ERROR;
    unsigned long long long_plen = 0;

    size_t nlen = cipher_ctx->cipher->nonce_len;
    size_t tlen = cipher_ctx->cipher->tag_len;

    switch (cipher_ctx->cipher->method) {
    case AES128GCM:
    case AES192GCM:
    case AES256GCM:
        err = mbedtls_cipher_auth_decrypt(cipher_ctx->evp, n, nlen, ad, adlen,
                                          m, mlen - tlen, p, plen, m + mlen - tlen, tlen);
        break;
    case CHACHA20POLY1305:
        err = crypto_aead_chacha20poly1305_decrypt(p, &long_plen, NULL, m, mlen,
                                                   ad, adlen, n, k);
        *plen = (size_t)long_plen; // it's safe to cast 64bit to 32bit length here
        break;
    case CHACHA20POLY1305IETF:
        err = crypto_aead_chacha20poly1305_ietf_decrypt(p, &long_plen, NULL, m, mlen,
                                                        ad, adlen, n, k);
        *plen = (size_t)long_plen; // it's safe to cast 64bit to 32bit length here
        break;
    case XCHACHA20POLY1305IETF:
        err = crypto_aead_xchacha20poly1305_ietf_decrypt(p, &long_plen, NULL, m, mlen,
                                                         ad, adlen, n, k);
        *plen = (size_t)long_plen; // it's safe to cast 64bit to 32bit length here
        break;
    default:
        return CRYPTO_ERROR;
    }

    return err;
}

/*
 * use Blake2b to generate key from user password
 * Since we're not generating password for storage
 * and we just want to avoid hashing result collisions
 * This method is used in ShadowVPN and xSocks
 * I think it is enough for our use case
 */
static int
aead_derive_key(const char *pass, uint8_t *key, size_t key_len)
{
    int err = crypto_generichash(key, key_len,
                                 (const unsigned char *)pass, strlen(pass),
                                 NULL, 0);
    if (err) {
        FATAL("Fail to generate hashing");
        /* satisfy compiler, we won't get here */
        return -1;
    } else {
        return key_len;
    }
}

static void
aead_cipher_ctx_set_subkey(cipher_ctx_t *cipher_ctx, int enc)
{
    // only read crypto_generichash_blake2b_SALTBYTES
    int method = cipher_ctx->cipher->method;
    int err    = crypto_generichash_blake2b_salt_personal(cipher_ctx->subkey, cipher_ctx->cipher->key_len,
                                                          NULL, 0,
                                                          /* master key */
                                                          cipher_ctx->cipher->key,
                                                          cipher_ctx->cipher->key_len,
                                                          /* salt */
                                                          cipher_ctx->salt,
                                                          /* hkdf info (personal) */
                                                          (const unsigned char *)SUBKEY_APPID);
    if (err) {
        FATAL("Unable to generate subkey");
    }

    /* use counting nonce start from 0 */
    memset(cipher_ctx->nonce, 0, cipher_ctx->cipher->nonce_len);

    if (method >= CHACHA20POLY1305) {
        // no need to set key for libsodium, just return
        return;
    } else {
        if (mbedtls_cipher_setkey(cipher_ctx->evp, cipher_ctx->subkey,
                                  cipher_ctx->cipher->key_len * 8, enc) != 0) {
            FATAL("Cannot set mbed TLS cipher subkey");
        }
        if (mbedtls_cipher_reset(cipher_ctx->evp) != 0) {
            FATAL("Cannot finish preparation of mbed TLS cipher context");
        }
    }
}

/*
 * For UDP, we use master key instead of session key since we don't have
 * many packets running out nonce space
 */
static void
aead_cipher_ctx_udp_set_key(cipher_ctx_t *cipher_ctx, int enc)
{
    /* only available for ciphers using mbed TLS */
    if (cipher_ctx->cipher->method >= CHACHA20POLY1305)
        return;

    if (mbedtls_cipher_setkey(cipher_ctx->evp, cipher_ctx->cipher->key,
                              cipher_ctx->cipher->key_len * 8, enc) != 0) {
        FATAL("[udp] Cannot set mbed TLS cipher master key");
    }
    if (mbedtls_cipher_reset(cipher_ctx->evp) != 0) {
        FATAL("[udp] Cannot finish preparation of mbed TLS cipher context");
    }
}

/*
 * get basic cipher info structure
 * it's a wrapper offered by crypto library
 */
const cipher_kt_t *
aead_get_cipher_type(int method)
{
    if (method < AES128GCM || method >= AEAD_CIPHER_NUM) {
        LOGE("aead_get_cipher_type(): Illegal method");
        return NULL;
    }

    /* cipher that don't use mbed TLS, just return */
    if (method >= CHACHA20POLY1305) {
        return NULL;
    }

    const char *ciphername  = supported_aead_ciphers[method];
    const char *mbedtlsname = supported_aead_ciphers_mbedtls[method];
    if (strcmp(mbedtlsname, CIPHER_UNSUPPORTED) == 0) {
        LOGE("Cipher %s currently is not supported by mbed TLS library",
             ciphername);
        return NULL;
    }
    return mbedtls_cipher_info_from_string(mbedtlsname);
}

static void
aead_cipher_ctx_init(cipher_ctx_t *cipher_ctx, int method, int enc)
{
    if (method < AES128GCM || method >= AEAD_CIPHER_NUM) {
        LOGE("cipher_context_init(): Illegal method");
        return;
    }

    if (method >= CHACHA20POLY1305) {
        return;
    }

    const char *ciphername = supported_aead_ciphers[method];

    const cipher_kt_t *cipher = aead_get_cipher_type(method);

    cipher_ctx->evp = ss_malloc(sizeof(cipher_evp_t));
    memset(cipher_ctx->evp, 0, sizeof(cipher_evp_t));
    cipher_evp_t *evp = cipher_ctx->evp;

    if (cipher == NULL) {
        LOGE("Cipher %s not found in mbed TLS library", ciphername);
        FATAL("Cannot initialize mbed TLS cipher");
    }
    mbedtls_cipher_init(evp);
    if (mbedtls_cipher_setup(evp, cipher) != 0) {
        FATAL("Cannot initialize mbed TLS cipher context");
    }

#ifdef FS_DEBUG
    dump("KEY", (char *)cipher_ctx->cipher->key, cipher_ctx->cipher->key_len);
#endif
}

void
aead_ctx_init(cipher_t *cipher, cipher_ctx_t *cipher_ctx, int enc)
{
    sodium_memzero(cipher_ctx, sizeof(cipher_ctx_t));
    cipher_ctx->cipher = cipher;

    aead_cipher_ctx_init(cipher_ctx, cipher->method, enc);

    if (enc) {
        rand_bytes(cipher_ctx->salt, cipher->key_len);
    }
}

void
aead_ctx_release(cipher_ctx_t *cipher_ctx)
{
    if (cipher_ctx->chunk != NULL) {
        bfree(cipher_ctx->chunk);
        ss_free(cipher_ctx->chunk);
        cipher_ctx->chunk = NULL;
    }

    if (cipher_ctx->cipher->method >= CHACHA20POLY1305) {
        return;
    }

    mbedtls_cipher_free(cipher_ctx->evp);
    ss_free(cipher_ctx->evp);
}

int
aead_encrypt_all(buffer_t *plaintext, cipher_t *cipher, size_t capacity)
{
    cipher_ctx_t cipher_ctx;
    aead_ctx_init(cipher, &cipher_ctx, 1);

    size_t salt_len = cipher->key_len;
    size_t tag_len  = cipher->tag_len;
    int err         = CRYPTO_OK;

    static buffer_t tmp = { 0, 0, 0, NULL };
    brealloc(&tmp, salt_len + tag_len + plaintext->len, capacity);
    buffer_t *ciphertext = &tmp;
    ciphertext->len = tag_len + plaintext->len;

    /* copy salt to first pos */
    memcpy(ciphertext->data, cipher_ctx.salt, salt_len);

    aead_cipher_ctx_udp_set_key(&cipher_ctx, 1);

    size_t clen = ciphertext->len;
    err = aead_cipher_encrypt(&cipher_ctx,
                              (uint8_t *)ciphertext->data + salt_len,
                              &clen,
                              (uint8_t *)plaintext->data,
                              plaintext->len,
                              NULL,
                              0,
                              cipher_ctx.nonce,
                              cipher_ctx.cipher->key);

    aead_ctx_release(&cipher_ctx);

    if (err)
        return CRYPTO_ERROR;

#ifdef FS_DEBUG
    dump("PLAIN", plaintext->data, plaintext->len);
    dump("CIPHER", ciphertext->data + salt_len, ciphertext->len);
#endif

    assert(ciphertext->len == clen);

    brealloc(plaintext, salt_len + ciphertext->len, capacity);
    memcpy(plaintext->data, ciphertext->data, salt_len + ciphertext->len);
    plaintext->len = salt_len + ciphertext->len;

    return CRYPTO_OK;
}

int
aead_decrypt_all(buffer_t *ciphertext, cipher_t *cipher, size_t capacity)
{
    size_t salt_len = cipher->key_len;
    size_t tag_len  = cipher->tag_len;
    int err         = CRYPTO_OK;

    if (ciphertext->len <= salt_len + tag_len) {
        return CRYPTO_ERROR;
    }

    cipher_ctx_t cipher_ctx;
    aead_ctx_init(cipher, &cipher_ctx, 0);

    static buffer_t tmp = { 0, 0, 0, NULL };
    brealloc(&tmp, ciphertext->len, capacity);
    buffer_t *plaintext = &tmp;
    plaintext->len = ciphertext->len - salt_len - tag_len;

    /* get salt */
    uint8_t *salt = cipher_ctx.salt;
    memcpy(salt, ciphertext->data, salt_len);

#ifdef MODULE_REMOTE
    err = fs_sbf_check((void *)salt, salt_len);
    if (err == 1) {
        LOGE("[udp] crypto: AEAD: repeat salt detected");
        return CRYPTO_ERROR;
    } else if (err < 0) {
        LOGE("[udp] crypto: AEAD: failed to check salt");
    }
#endif

    aead_cipher_ctx_udp_set_key(&cipher_ctx, 0);

    size_t plen = plaintext->len;
    err = aead_cipher_decrypt(&cipher_ctx,
                              (uint8_t *)plaintext->data,
                              &plen,
                              (uint8_t *)ciphertext->data + salt_len,
                              ciphertext->len - salt_len,
                              NULL,
                              0,
                              cipher_ctx.nonce,
                              cipher_ctx.cipher->key);

#ifdef FS_DEBUG
    dump("PLAIN", plaintext->data, plaintext->len);
    dump("CIPHER", ciphertext->data + salt_len, ciphertext->len - salt_len);
#endif

    aead_ctx_release(&cipher_ctx);

    if (err)
        return CRYPTO_ERROR;

#ifdef MODULE_REMOTE
    if (fs_sbf_add((void *)salt, salt_len) < 0)
        LOGE("[udp] crypto: AEAD: failed to add salt");
#endif

    brealloc(ciphertext, plaintext->len, capacity);
    memcpy(ciphertext->data, plaintext->data, plaintext->len);
    ciphertext->len = plaintext->len;

    return CRYPTO_OK;
}

static int
aead_chunk_encrypt(cipher_ctx_t *ctx, uint8_t *p, uint8_t *c, uint8_t *n,
                   uint16_t plen)
{
    size_t nlen = ctx->cipher->nonce_len;
    size_t tlen = ctx->cipher->tag_len;

    assert(plen <= CHUNK_SIZE_MASK);

    int err;
    size_t clen;
    uint8_t len_buf[CHUNK_SIZE_LEN];
    uint16_t real_plen = min(plen, CHUNK_SIZE_MASK);
    uint16_t t         = htons(real_plen);
    memcpy(len_buf, &t, CHUNK_SIZE_LEN);

    clen = CHUNK_SIZE_LEN + tlen;
    err  = aead_cipher_encrypt(ctx, c, &clen, len_buf, CHUNK_SIZE_LEN,
                               NULL, 0, n, ctx->subkey);
    if (err)
        return CRYPTO_ERROR;

    assert(clen == CHUNK_SIZE_LEN + tlen);

    sodium_increment(n, nlen);

    clen = real_plen + tlen;
    err  = aead_cipher_encrypt(ctx, c + CHUNK_SIZE_LEN + tlen, &clen, p, real_plen,
                               NULL, 0, n, ctx->subkey);
    if (err)
        return CRYPTO_ERROR;

    assert(clen == real_plen + tlen);

    sodium_increment(n, nlen);

    return CRYPTO_OK;
}

/* TCP */
int
aead_encrypt(buffer_t *plaintext, cipher_ctx_t *cipher_ctx, size_t capacity)
{
    if (cipher_ctx == NULL)
        return CRYPTO_ERROR;

    if (plaintext->len == 0) {
        return CRYPTO_OK;
    }

    static buffer_t tmp = { 0, 0, 0, NULL };
    buffer_t *ciphertext;

    cipher_t *cipher = cipher_ctx->cipher;
    int err          = CRYPTO_ERROR;
    size_t salt_ofst = 0;
    size_t salt_len  = cipher->key_len;
    size_t tag_len   = cipher->tag_len;

    if (!cipher_ctx->init) {
        salt_ofst = salt_len;
    }

    size_t out_len = salt_ofst + 2 * tag_len + plaintext->len + CHUNK_SIZE_LEN;
    brealloc(&tmp, out_len, capacity);
    ciphertext      = &tmp;
    ciphertext->len = out_len;

    if (!cipher_ctx->init) {
        memcpy(ciphertext->data, cipher_ctx->salt, salt_len);
        aead_cipher_ctx_set_subkey(cipher_ctx, 1);
        cipher_ctx->init = 1;
    }

    err = aead_chunk_encrypt(cipher_ctx,
                             (uint8_t *)plaintext->data,
                             (uint8_t *)ciphertext->data + salt_ofst,
                             cipher_ctx->nonce, plaintext->len);
    if (err)
        return err;

#ifdef FS_DEBUG
    dump("PLAIN", plaintext->data, plaintext->len);
    dump("CIPHER", ciphertext->data + salt_ofst, ciphertext->len);
#endif

    brealloc(plaintext, ciphertext->len, capacity);
    memcpy(plaintext->data, ciphertext->data, ciphertext->len);
    plaintext->len = ciphertext->len;

    return 0;
}

static int
aead_chunk_decrypt(cipher_ctx_t *ctx, uint8_t *p, uint8_t *c, uint8_t *n,
                   size_t *plen, size_t *clen)
{
    int err;
    size_t mlen;
    size_t dec_plen;
    size_t nlen = ctx->cipher->nonce_len;
    size_t tlen = ctx->cipher->tag_len;

    if (*clen <= 2 * tlen + CHUNK_SIZE_LEN)
        return CRYPTO_NEED_MORE;

    uint8_t len_buf[CHUNK_SIZE_LEN];
    err = aead_cipher_decrypt(ctx, len_buf, &dec_plen, c, CHUNK_SIZE_LEN + tlen,
                              NULL, 0, n, ctx->subkey);
    if (err)
        return CRYPTO_ERROR;
    assert(dec_plen == CHUNK_SIZE_LEN);

    mlen = ntohs(*(uint16_t *)len_buf);

    if (mlen > CHUNK_SIZE_MASK) {
        LOGE("aead_chunk_decrypt: chunk too big");
        return CRYPTO_ERROR;
    }

    if (mlen == 0) {
        LOGE("aead_chunk_decrypt: mlen is 0");
        return CRYPTO_ERROR;
    }

    size_t chunk_len = 2 * tlen + CHUNK_SIZE_LEN + mlen;

    if (*clen < chunk_len)
        return CRYPTO_NEED_MORE;

    sodium_increment(n, nlen);

    err = aead_cipher_decrypt(ctx, p, plen, c + CHUNK_SIZE_LEN + tlen, mlen + tlen,
                              NULL, 0, n, ctx->subkey);
    if (err)
        return CRYPTO_ERROR;
    assert(*plen == mlen);

    sodium_increment(n, nlen);

    if (*clen > chunk_len)
        memmove(c, c + chunk_len, *clen - chunk_len);

    *clen -= chunk_len;

    return CRYPTO_OK;
}

int
aead_decrypt(buffer_t *ciphertext, cipher_ctx_t *cipher_ctx, size_t capacity)
{
    int err             = CRYPTO_OK;
    static buffer_t tmp = { 0, 0, 0, NULL };

    cipher_t *cipher = cipher_ctx->cipher;

    size_t salt_len = cipher->key_len;

    if (cipher_ctx->chunk == NULL) {
        cipher_ctx->chunk = (buffer_t *)ss_malloc(sizeof(buffer_t));
        memset(cipher_ctx->chunk, 0, sizeof(buffer_t));
        balloc(cipher_ctx->chunk, capacity);
    }

    brealloc(cipher_ctx->chunk,
             cipher_ctx->chunk->len + ciphertext->len, capacity);
    memcpy(cipher_ctx->chunk->data + cipher_ctx->chunk->len,
           ciphertext->data, ciphertext->len);
    cipher_ctx->chunk->len += ciphertext->len;

    brealloc(&tmp, cipher_ctx->chunk->len, capacity);
    buffer_t *plaintext = &tmp;

    if (!cipher_ctx->init) {
        if (cipher_ctx->chunk->len <= salt_len)
            return CRYPTO_NEED_MORE;
        memcpy(cipher_ctx->salt, cipher_ctx->chunk->data, salt_len);

        aead_cipher_ctx_set_subkey(cipher_ctx, 0);

#ifdef MODULE_REMOTE
        err = fs_sbf_check((void *)cipher_ctx->salt, salt_len);
        if (err == 1) {
            LOGE("crypto: AEAD: repeat salt detected");
            return CRYPTO_ERROR;
        } else if (err < 0) {
            LOGE("crypto: AEAD: fail to check salt");
        }
        if (fs_sbf_add((void *)cipher_ctx->salt, salt_len) < 0)
            LOGE("crypto: AEAD: fail to add salt");
#endif

        memmove(cipher_ctx->chunk->data, cipher_ctx->chunk->data + salt_len,
                cipher_ctx->chunk->len - salt_len);
        cipher_ctx->chunk->len -= salt_len;

        cipher_ctx->init = 1;
    }

    size_t plen = 0;
    while (cipher_ctx->chunk->len > 0) {
        size_t chunk_clen = cipher_ctx->chunk->len;
        size_t chunk_plen = 0;
        err = aead_chunk_decrypt(cipher_ctx,
                                 (uint8_t *)plaintext->data + plen,
                                 (uint8_t *)cipher_ctx->chunk->data,
                                 cipher_ctx->nonce,
                                 &chunk_plen, &chunk_clen);
        if (err == CRYPTO_ERROR) {
            dump("[E] TCP chunk", cipher_ctx->chunk->data, chunk_clen);
            return err;
        } else if (err == CRYPTO_NEED_MORE) {
            if (plen == 0) {
                return err;
            } else {
                break;
            }
        }
        cipher_ctx->chunk->len = chunk_clen;
        plen                  += chunk_plen;
    }
    plaintext->len = plen;

#ifdef FS_DEBUG
    dump("PLAIN", plaintext->data, plaintext->len);
    dump("CIPHER", ciphertext->data + salt_len, ciphertext->len - salt_len);
#endif

    brealloc(ciphertext, plaintext->len, capacity);
    memcpy(ciphertext->data, plaintext->data, plaintext->len);
    ciphertext->len = plaintext->len;

    return CRYPTO_OK;
}

cipher_t *
aead_key_init(int method, const char *pass)
{
    if (method < AES128GCM || method >= AEAD_CIPHER_NUM) {
        LOGE("aead_key_init(): Illegal method");
        return NULL;
    }

    cipher_t *cipher = (cipher_t *)ss_malloc(sizeof(cipher_t));
    memset(cipher, 0, sizeof(cipher_t));

    if (method >= CHACHA20POLY1305) {
        cipher_kt_t *cipher_info = (cipher_kt_t *)ss_malloc(sizeof(cipher_kt_t));
        cipher->info             = cipher_info;
        cipher->info->base       = NULL;
        cipher->info->key_bitlen = supported_aead_ciphers_key_size[method] * 8;
        cipher->info->iv_size    = supported_aead_ciphers_nonce_size[method];
    } else {
        cipher->info = (cipher_kt_t *)aead_get_cipher_type(method);
    }

    if (cipher->info == NULL && cipher->key_len == 0) {
        LOGE("Cipher %s not found in crypto library", supported_aead_ciphers[method]);
        FATAL("Cannot initialize cipher");
    }

    cipher->key_len = aead_derive_key(pass, cipher->key, supported_aead_ciphers_key_size[method]);

    if (cipher->key_len == 0) {
        FATAL("Cannot generate key and nonce");
    }

    cipher->nonce_len = supported_aead_ciphers_nonce_size[method];
    cipher->tag_len   = supported_aead_ciphers_tag_size[method];
    cipher->method    = method;

    return cipher;
}

cipher_t *
aead_init(const char *pass, const char *method)
{
    int m = AES128GCM;
    if (method != NULL) {
        /* check method validity */
        for (m = AES128GCM; m < AEAD_CIPHER_NUM; m++)
            if (strcmp(method, supported_aead_ciphers[m]) == 0) {
                break;
            }
        if (m >= AEAD_CIPHER_NUM) {
            LOGE("Invalid cipher name: %s, use aes-256-gcm instead", method);
            m = AES256GCM;
        }
    }
    return aead_key_init(m, pass);
}
