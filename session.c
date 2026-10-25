#include "session.h"

#include <stdio.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

bool session_decode_token(const char* token_hex, unsigned char* out_token)
{
    static const char hex_digits[] = "0123456789abcdef";
    const size_t hex_length = SESSION_TOKEN_SIZE * 2; // 2 hex chars per byte

    if (!token_hex || !out_token ||
        strlen(token_hex) != hex_length ||
        strspn(token_hex, hex_digits) != hex_length) { // ensure input only uses hex_digits
        return false;
    }

    for (size_t i = 0; i < SESSION_TOKEN_SIZE; ++i) {
        const char* high_digit = strchr(hex_digits, token_hex[i * 2]);
        const char* low_digit = strchr(hex_digits, token_hex[i * 2 + 1]);

        unsigned int high_value = (unsigned int)(high_digit - hex_digits);
        unsigned int low_value = (unsigned int)(low_digit - hex_digits);

        out_token[i] = (unsigned char)(high_value * 16 + low_value);
    }

    return true;
}

// Just for hashing
bool session_hash_token(
    const unsigned char* token,
    unsigned char* out_token_hash)
{
    if (!token || !out_token_hash) {
        return false;
    }

    unsigned int hash_size = 0;

    if (EVP_Digest(
            token, SESSION_TOKEN_SIZE,
            out_token_hash, &hash_size,
            EVP_sha256(), NULL) != 1 ||
            hash_size != SESSION_HASH_SIZE) {
        OPENSSL_cleanse(out_token_hash, SESSION_HASH_SIZE);
        return false;
    }

    return true;
}

// Fills a 32 byte buffer with random bytes and hashes it
bool session_generate_token(
    unsigned char* out_token,
    unsigned char* out_token_hash)
{
    if (!out_token || !out_token_hash) {
        fprintf(stderr, "Invalid session token arguments\n");
        return false;
    }

    // random 32 byte token
    if (RAND_bytes(out_token, SESSION_TOKEN_SIZE) != 1) {
        fprintf(stderr, "Could not generate session token\n");
        goto failure;
    }

    // hash it
    if (!session_hash_token(out_token, out_token_hash)) {
        goto failure;
    }

    return true;

failure:
    OPENSSL_cleanse(out_token, SESSION_TOKEN_SIZE);
    OPENSSL_cleanse(out_token_hash, SESSION_HASH_SIZE);
    return false;
}
