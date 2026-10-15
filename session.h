#ifndef AUTH_SESSION_H
#define AUTH_SESSION_H

#include <stdbool.h>

#define SESSION_TOKEN_SIZE 32
#define SESSION_HASH_SIZE 32

bool session_generate_token(unsigned char* out_token, unsigned char* out_token_hash);

#endif
