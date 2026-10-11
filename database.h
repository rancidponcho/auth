#ifndef AUTH_DATABASE_H
#define AUTH_DATABASE_H

#include <stdint.h>

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

// Close with PQfinish
PGconn* database_connect(void);

#endif
