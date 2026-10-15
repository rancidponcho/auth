#include "session.h"

#include <stdio.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

// Fills a 32 byte buffer with random bytes and hashes it
bool session_generate_token(unsigned char* out_token, unsigned char* out_token_hash)
{
    if (!out_token || !out_token_hash) {
        fprintf(stderr, "Invalid session token arguments\n");
        return false;
    }

    unsigned int hash_size = 0;

    if (RAND_bytes(out_token, SESSION_TOKEN_SIZE) != 1) {
        fprintf(stderr, "Could not generate session token\n");
        goto failure;
    }

    // hash it
    if (EVP_Digest(
            out_token, SESSION_TOKEN_SIZE, 
            out_token_hash, &hash_size, 
            EVP_sha256(), NULL) != 1 || 
            hash_size != SESSION_HASH_SIZE) {
        fprintf(stderr, "Could not hash session token\n");
        goto failure;
    }

    return true;

failure:
    OPENSSL_cleanse(out_token, SESSION_TOKEN_SIZE);
    OPENSSL_cleanse(out_token_hash, SESSION_HASH_SIZE);
    return false;
}

