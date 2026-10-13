#include "database.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

DatabaseLookupResult database_find_player(
    PGconn* connection,
    const char* provider,
    const char* provider_user_id,
    int64_t* out_player_id)
{
    if (!connection || !provider || !provider_user_id || !out_player_id) {
        fprintf(stderr, "Invalid database lookup arguments\n");
        return DATABASE_LOOKUP_ERROR;
    }
    
    // Ask database for the matching player ID
    const char* query =
        "SELECT player_id FROM auth.identities "
        "WHERE provider = $1 AND provider_user_id = $2;";

    const char* parameters[] = {provider, provider_user_id};

    // points to object containing query status
    PGresult* result = PQexecParams(connection, query, 2, NULL, parameters, NULL, NULL, 0);
    
    // check for fail or no match
    if (!result) {
        fprintf(stderr, "Player lookup failed: %s", PQerrorMessage(connection));
        PQclear(result);
        return DATABASE_LOOKUP_ERROR;
    }

    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Player lookup failed: %s", PQresultErrorMessage(result));
        PQclear(result);
        return DATABASE_LOOKUP_ERROR;
    }

    int row_count = PQntuples(result);
    
    if (row_count == 0) {
        PQclear(result);
        return DATABASE_LOOKUP_NOT_FOUND;
    }

    if (row_count != 1 || PQnfields(result) != 1 || PQgetisnull(result, 0, 0)) {
        fprintf(stderr, "Player lookup returned an unexpected result\n");
        PQclear(result);
        return DATABASE_LOOKUP_ERROR;
    }
    
    // get associated internal player id
    const char* player_id_text = PQgetvalue(result, 0, 0);
    char* end = NULL;

    errno = 0;
    intmax_t player_id = strtoimax(player_id_text, &end, 10);

    if (errno == ERANGE ||
        end == player_id_text ||
        *end != '\0' ||
        player_id < INT64_MIN ||
        player_id > INT64_MAX) {
        fprintf(stderr, "Player lookup returned an invalid player ID\n");
        PQclear(result);
        return DATABASE_LOOKUP_ERROR;
    }

    *out_player_id = (int64_t)player_id;

    PQclear(result);
    return DATABASE_LOOKUP_FOUND;
}


PGconn* database_connect(void)
{
    PGconn* connection = PQconnectdb(
        "connect_timeout=5 "
        "options='-c statement_timeout=3s -c lock_timeout=1s'");

    if (!connection) {
        fprintf(stderr, "Could not allocate database connection\n");
        return NULL;
    }

    if (PQstatus(connection) != CONNECTION_OK) {
        fprintf(stderr, "Database connection failed: %s", PQerrorMessage(connection));
        PQfinish(connection);
        return NULL;
    }

    return connection;
}

DatabaseCreateResult database_create_player(
    PGconn* connection,
    const char* provider,
    const char* provider_user_id,
    int64_t* out_player_id)
{
    if (!connection || !provider || !provider_user_id || !out_player_id || !provider[0] || !provider_user_id[0]) {
        fprintf(stderr, "Invalid account creation arguments\n");
        return DATABASE_CREATE_ERROR;
    }

    // starts transaction 
    if (PQtransactionStatus(connection) != PQTRANS_IDLE) {
        fprintf(stderr, "Account creation requires an idle connection\n");
        return DATABASE_CREATE_ERROR;
    }

    DatabaseCreateResult outcome = DATABASE_CREATE_ERROR;
    PGresult* account_result = NULL;
    PGresult* result = PQexec(connection, "BEGIN");

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Could not begin transaction: %s\n", PQerrorMessage(connection));
        goto cleanup;
    }

    PQclear(result);
    result = NULL;

    account_result = PQexec(connection,
        "INSERT INTO auth.accounts DEFAULT VALUES "
        "RETURNING player_id;");

    if (!account_result || PQresultStatus(account_result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Account insert failed: %s\n", PQerrorMessage(connection));
        goto rollback;
    }

    if (PQntuples(account_result) != 1 || PQnfields(account_result) != 1 || PQgetisnull(account_result, 0, 0)) {
        fprintf(stderr, "Account insert returned an unexpected result\n");
        goto rollback;
    }

    // Read generated player ID
    const char* player_id_text = PQgetvalue(account_result, 0, 0);
    char* end = NULL;

    errno = 0;
    intmax_t player_id = strtoimax(player_id_text, &end, 10);

    if (errno == ERANGE ||
        end == player_id_text || *end != '\0' || player_id < INT64_MIN || player_id > INT64_MAX) {
        fprintf(stderr, "Account insert returned an invalid player ID\n");
        goto rollback;
    }

    const char* query =
        "INSERT INTO auth.identities "
        "(player_id, provider, provider_user_id) "
        "VALUES ($1, $2, $3) "
        "ON CONFLICT (provider, provider_user_id) DO NOTHING "
        "RETURNING player_id;";

    const char* parameters[] = {
        player_id_text,
        provider,
        provider_user_id
    };

    result = PQexecParams(connection, query, 3, NULL, parameters, NULL, NULL, 0);

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Identity insert failed: %s\n", PQerrorMessage(connection));
        goto rollback;
    }

    // another request already registered this identity
    if (PQntuples(result) == 0) {
        outcome = DATABASE_CREATE_CONFLICT;
        goto rollback;
    }

    if (PQntuples(result) != 1 || PQnfields(result) != 1 || PQgetisnull(result, 0, 0)) {
        fprintf(stderr, "Identity insert returned an unexpected result\n");
        goto rollback;
    }

    // commit successful creation
    PQclear(result);
    result = PQexec(connection, "COMMIT");

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Could not commit account creation: %s\n", PQerrorMessage(connection));
        goto rollback;
    }

    // publish ID only after confirmed commit
    *out_player_id = (int64_t)player_id;
    outcome = DATABASE_CREATE_CREATED;
    goto cleanup;

rollback:
    PQclear(result);
    result = PQexec(connection, "ROLLBACK");

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Could not roll back account creation: %s\n", PQerrorMessage(connection));
        outcome = DATABASE_CREATE_ERROR;
    }

cleanup:
    PQclear(result);
    PQclear(account_result);
    return outcome;
}

DatabaseAccessResult database_check_access(PGconn* connection, int64_t player_id)
{
    if (!connection || !player_id) {
        fprintf(stderr, "Invalid account access check arguments");
        return DATABASE_ACCESS_ERROR;
    }

    char player_id_text[21];
    snprintf(player_id_text, sizeof(player_id_text), "%" PRId64, player_id);

    const char* parameters[] = {player_id_text};

    const char* query =
        "SELECT deleted_at IS NOT NULL AS deleted, "
        "EXISTS ("
            "SELECT 1 FROM auth.bans "
            "WHERE player_id = $1 "
            "AND revoked_at IS NULL "
            "AND banned_at <= CURRENT_TIMESTAMP "
            "AND (banned_until IS NULL OR banned_until > CURRENT_TIMESTAMP)"
        ") AS banned "
        "FROM auth.accounts "
        "WHERE player_id = $1;";

    PGresult* result = PQexecParams(connection, query, 1, NULL, parameters, NULL, NULL, 0);

    // Check query success
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Account access query failed: %s", PQerrorMessage(connection));
        PQclear(result);
        return DATABASE_ACCESS_ERROR;
    }

    if (PQntuples(result) != 1 || PQnfields(result) != 2 || PQgetisnull(result, 0, 0) || PQgetisnull(result, 0, 1)) {
        fprintf(stderr, "Account access query returned an unexpected result\n");
        PQclear(result);
        return DATABASE_ACCESS_ERROR;
    }

    // verify flags
    const char* deleted_text = PQgetvalue(result, 0, 0);
    const char* banned_text = PQgetvalue(result, 0, 1);

    if ((strcmp(deleted_text, "t") != 0 &&
         strcmp(deleted_text, "f") != 0) ||
        (strcmp(banned_text, "t") != 0 &&
         strcmp(banned_text, "f") != 0)) {
        fprintf(stderr, "Account access query returned invalid flags\n");
        PQclear(result);
        return DATABASE_ACCESS_ERROR;
    }

    DatabaseAccessResult access_result;

    if (strcmp(deleted_text, "t") == 0) {
        access_result = DATABASE_ACCESS_DELETED;
    } else if (strcmp(banned_text, "t") == 0) {
        access_result = DATABASE_ACCESS_BANNED;
    } else {
        access_result = DATABASE_ACCESS_ALLOWED;
    }

    PQclear(result);
    return access_result;
}
