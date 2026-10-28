#ifndef AUTH_DATABASE_H
#define AUTH_DATABASE_H

#include <stdint.h>
#include <stdbool.h>

#include <libpq-fe.h>

#define AUTH_PROVIDER_STEAM "steam"
#define AUTH_PROVIDER_GOOGLE "google"
#define AUTH_PROVIDER_APPLE "apple"

typedef enum DatabaseLookupResult {
    DATABASE_LOOKUP_ERROR,
    DATABASE_LOOKUP_NOT_FOUND,
    DATABASE_LOOKUP_FOUND
} DatabaseLookupResult;

DatabaseLookupResult database_find_player(
    PGconn* connection,
    const char* provider,
    const char* provider_user_id,
    int64_t* out_player_id);

typedef enum DatabaseCreateResult {
    DATABASE_CREATE_ERROR,
    DATABASE_CREATE_CONFLICT,
    DATABASE_CREATE_CREATED
} DatabaseCreateResult;

DatabaseCreateResult database_create_player(
    PGconn* connection,
    const char* provider,
    const char* provider_user_id,
    int64_t* out_player_id);

typedef enum DatabaseAccessResult {
    DATABASE_ACCESS_ERROR,
    DATABASE_ACCESS_ALLOWED,
    DATABASE_ACCESS_DELETED,
    DATABASE_ACCESS_BANNED
} DatabaseAccessResult;

DatabaseAccessResult database_check_access(
    PGconn* connection,
    int64_t player_id);

typedef enum DatabaseSessionResult {
    DATABASE_SESSION_ERROR,
    DATABASE_SESSION_DELETED,
    DATABASE_SESSION_BANNED,
    DATABASE_SESSION_CREATED
} DatabaseSessionResult;

// token_hash contains SESSION_HASH_SIZE bytes.
// Expiry output is set only after successful commit
DatabaseSessionResult database_create_session(
    PGconn* connection,
    int64_t player_id,
    const unsigned char* token_hash,
    int lifetime_seconds,
    int64_t* out_expires_at_unix);

// Close with PQfinish
PGconn* database_connect(void);

typedef enum DatabaseValidationResult {
    DATABASE_VALIDATION_ERROR,
    DATABASE_VALIDATION_REJECTED,
    DATABASE_VALIDATION_VALID
} DatabaseValidationResult;

// Call outside a transaction. Outputs are written only on VALID
DatabaseValidationResult database_validate_session(
    PGconn* connection,
    const unsigned char* token_hash,
    int64_t* out_player_id,
    int64_t* out_expires_at_unix);

bool database_revoke_session(
    PGconn* connection,
    const unsigned char* token_hash);

#endif
