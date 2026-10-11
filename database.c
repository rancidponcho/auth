#include "database.h"

#include <stdio.h>
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
    PGconn* connection = PQconnectdb("connect_timeout=5");
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

