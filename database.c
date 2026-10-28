#include "database.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "session.h"

/*
 * What is sounds like:
 *
 * Queries PostgreSQL for the provider_user_it, putting the internal ID
 * into out_player_id and returning success via a DatabaseLookupResult
 * enum.
 */
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
    PGresult* result =
        PQexecParams(connection, query, 2, NULL, parameters, NULL, NULL, 0);

    // check for fail or no match
    if (!result) {
        fprintf(stderr, "Player lookup failed: %s", PQerrorMessage(connection));
        PQclear(result);
        return DATABASE_LOOKUP_ERROR;
    }

    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Player lookup failed: %s",
            PQresultErrorMessage(result));
        PQclear(result);
        return DATABASE_LOOKUP_ERROR;
    }

    int row_count = PQntuples(result);

    if (row_count == 0) {
        PQclear(result);
        return DATABASE_LOOKUP_NOT_FOUND;
    }

    if (row_count != 1 ||
        PQnfields(result) != 1 ||
        PQgetisnull(result, 0, 0)) {
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
        fprintf(
            stderr,
            "Database connection failed: %s",
            PQerrorMessage(connection));
        PQfinish(connection);
        return NULL;
    }

    return connection;
}

// Player data is stored like so:
//                                     Table "auth.accounts"
//   Column   |           Type           | Collation | Nullable |           Default
//------------+--------------------------+-----------+----------+------------------------------
// player_id  | bigint                   |           | not null | generated always as identity
// created_at | timestamp with time zone |           | not null | CURRENT_TIMESTAMP
// deleted_at | timestamp with time zone |           |          |
DatabaseCreateResult database_create_player(
    PGconn* connection,
    const char* provider,
    const char* provider_user_id,
    int64_t* out_player_id)
{
    if (!connection ||
        !provider ||
        !provider_user_id ||
        !out_player_id ||
        !provider[0] ||
        !provider_user_id[0]) {
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
        fprintf(
            stderr,
            "Could not begin transaction: %s\n",
            PQerrorMessage(connection));
        goto cleanup;
    }

    PQclear(result);
    result = NULL;

    account_result = PQexec(
        connection,
        "INSERT INTO auth.accounts DEFAULT VALUES "
        "RETURNING player_id;");

    if (!account_result ||
        PQresultStatus(account_result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Account insert failed: %s\n",
            PQerrorMessage(connection));
        goto rollback;
    }

    if (PQntuples(account_result) != 1 ||
        PQnfields(account_result) != 1 ||
        PQgetisnull(account_result, 0, 0)) {
        fprintf(stderr, "Account insert returned an unexpected result\n");
        goto rollback;
    }

    // Read player ID generated by PostgreSQL
    const char* player_id_text = PQgetvalue(account_result, 0, 0);
    char* end = NULL;

    errno = 0;
    intmax_t player_id = strtoimax(player_id_text, &end, 10);

    if (errno == ERANGE ||
        end == player_id_text ||
        *end != '\0' ||
        player_id < INT64_MIN ||
        player_id > INT64_MAX) {
        fprintf(stderr, "Account insert returned an invalid player ID\n");
        goto rollback;
    }

    const char* query =
        "INSERT INTO auth.identities "
        "(player_id, provider, provider_user_id) "
        "VALUES ($1, $2, $3) "
        "ON CONFLICT (provider, provider_user_id) DO NOTHING "
        "RETURNING player_id;";

    const char* parameters[] = {player_id_text, provider, provider_user_id};

    result =
        PQexecParams(connection, query, 3, NULL, parameters, NULL, NULL, 0);

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Identity insert failed: %s\n",
            PQerrorMessage(connection));
        goto rollback;
    }

    // another request already registered this identity
    if (PQntuples(result) == 0) {
        outcome = DATABASE_CREATE_CONFLICT;
        goto rollback;
    }

    if (PQntuples(result) != 1 ||
        PQnfields(result) != 1 ||
        PQgetisnull(result, 0, 0)) {
        fprintf(stderr, "Identity insert returned an unexpected result\n");
        goto rollback;
    }

    // commit successful creation
    PQclear(result);
    result = PQexec(connection, "COMMIT");

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(
            stderr,
            "Could not commit account creation: %s\n",
            PQerrorMessage(connection));
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
        fprintf(
            stderr,
            "Could not roll back account creation: %s\n",
            PQerrorMessage(connection));
        outcome = DATABASE_CREATE_ERROR;
    }

cleanup:
    PQclear(result);
    PQclear(account_result);
    return outcome;
}

// Check bans/deletions using internal player_id
DatabaseAccessResult database_check_access(
    PGconn* connection,
    int64_t player_id)
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
            "AND banned_at <= statement_timestamp() "
            "AND (banned_until IS NULL OR banned_until > statement_timestamp())"
        ") AS banned "
        "FROM auth.accounts "
        "WHERE player_id = $1;";

    PGresult* result =
        PQexecParams(connection, query, 1, NULL, parameters, NULL, NULL, 0);

    // Check query success
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Account access query failed: %s",
            PQerrorMessage(connection));
        PQclear(result);
        return DATABASE_ACCESS_ERROR;
    }

    if (PQntuples(result) != 1 || PQnfields(result) != 2 ||
        PQgetisnull(result, 0, 0) || PQgetisnull(result, 0, 1)) {
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

// Sessions store player_id, hash, and expiration date of the session.
DatabaseSessionResult database_create_session(
    PGconn* connection,
    int64_t player_id,
    const unsigned char* token_hash,
    int lifetime_seconds,
    int64_t* out_expires_at_unix)
{
    if (!connection ||
        player_id <= 0 ||
        !token_hash ||
        lifetime_seconds <= 0 ||
        !out_expires_at_unix) {
        fprintf(stderr, "Invalid session creation arguments\n");
        return DATABASE_SESSION_ERROR;
    }

    if (PQtransactionStatus(connection) != PQTRANS_IDLE) {
        fprintf(stderr, "Session creation requires an idle connection \n");
        return DATABASE_SESSION_ERROR;
    }

    char player_id_text[21];
    char lifetime_text[21];

    snprintf(player_id_text, sizeof(player_id_text), "%" PRId64, player_id);
    snprintf(lifetime_text, sizeof(lifetime_text), "%d", lifetime_seconds);

    DatabaseSessionResult outcome = DATABASE_SESSION_ERROR;

    // Disallow this transaction from reading uncommitted entries
    PGresult* result =
        PQexec(connection, "BEGIN ISOLATION LEVEL READ COMMITTED");

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(
            stderr,
            "Could not begin session transaction: %s",
            PQerrorMessage(connection));
        goto rollback;
    }

    PQclear(result);

    const char* account_parameters[] = {player_id_text};
    // find player row by ID
    result = PQexecParams(
        connection,
        "SELECT player_id FROM auth.accounts "
        "WHERE player_id = $1::bigint FOR UPDATE;", // FOR UPDATE -> lock row until commit
        1,
        NULL,
        account_parameters,
        NULL,
        NULL,
        0);

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Could not lock account: %s",
            PQerrorMessage(connection));
        goto rollback;
    }

    if (PQntuples(result) != 1 ||
        PQnfields(result) != 1 ||
        PQgetisnull(result, 0, 0)) {
        fprintf(stderr, "Account lock query returned an unexpected result\n");
        goto rollback;
    }

    PQclear(result);
    result = NULL;

    // Check if ban/deleted account
    DatabaseAccessResult access_result =
        database_check_access(connection, player_id);

    if (access_result != DATABASE_ACCESS_ALLOWED) {
        if (access_result == DATABASE_ACCESS_DELETED) {
            outcome = DATABASE_SESSION_DELETED;
        } else if (access_result == DATABASE_ACCESS_BANNED) {
            outcome = DATABASE_SESSION_BANNED;
        }

        goto rollback;
    }

    // this is how new session entries for the player are made
    const char* query =
        "INSERT INTO auth.sessions "
        "(player_id, token_hash, created_at, expires_at) "
        "VALUES ($1::bigint, $2::bytea, statement_timestamp(), "
        "statement_timestamp() + $3::integer * INTERVAL '1 second')"    // Session duration
        "RETURNING floor(extract(epoch FROM expires_at))::bigint;";     // Unix timestep, seconds rounded

    // Place token hash into database at location $2
    const char* parameters[] = {
        player_id_text,
        (const char*)token_hash,
        lifetime_text
    };

    const int parameter_lengths[] = {0, SESSION_HASH_SIZE, 0};
    const int parameter_formats[] = {0, 1, 0};
    result = PQexecParams(
        connection,
        query,
        3,
        NULL,
        parameters,
        parameter_lengths,
        parameter_formats,
        0);

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Session insert failed: %s",
            PQerrorMessage(connection));
        goto rollback;
    }

    if (PQntuples(result) != 1 ||
        PQnfields(result) != 1 ||
        PQgetisnull(result, 0, 0)) {
        fprintf(stderr, "Session insert returned an unexpected result\n");
        goto rollback;
    }

    // Read expiry before freeing the result that owns its text
    const char* expiry_text = PQgetvalue(result, 0, 0);
    char* end = NULL;

    errno = 0;
    intmax_t expiry_unix = strtoimax(expiry_text, &end, 10);

    if (errno == ERANGE ||
        end == expiry_text ||
        *end != '\0' ||
        expiry_unix <= 0 ||
        expiry_unix > INT64_MAX) {
        fprintf(stderr, "Session insert returned an invalid expiry\n");
        goto rollback;
    }

    PQclear(result);
    result = PQexec(connection, "COMMIT");

    if (!result ||
        PQresultStatus(result) != PGRES_COMMAND_OK ||
        strcmp(PQcmdStatus(result), "COMMIT") != 0) {
        fprintf(
            stderr,
            "Could not commit session creation: %s",
            PQerrorMessage(connection));
        goto rollback;
    }

    // Publish
    *out_expires_at_unix = (int64_t)expiry_unix;
    outcome = DATABASE_SESSION_CREATED;
    goto cleanup;

rollback:
    PQclear(result);
    result = PQexec(connection, "ROLLBACK");

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(
            stderr,
            "Could not roll back session creation: %s",
            PQerrorMessage(connection));
        outcome = DATABASE_SESSION_ERROR;
    }

cleanup:
    PQclear(result);
    return outcome;
}

DatabaseValidationResult database_validate_session(
    PGconn* connection,
    const unsigned char* token_hash,
    int64_t* out_player_id,
    int64_t* out_expires_at_unix)
{
    if (!connection || !token_hash || !out_player_id || !out_expires_at_unix) {
        return DATABASE_VALIDATION_ERROR;
    }

    if (PQtransactionStatus(connection) != PQTRANS_IDLE) {
        fprintf(stderr, "Session validation requires an idle connection\n");
        return DATABASE_VALIDATION_ERROR;
    }

    //
    const char* query =
        "SELECT s.player_id, "
        "floor(extract(epoch FROM s.expires_at))::bigint "
        "FROM auth.sessions AS s "
        "JOIN auth.accounts AS a ON a.player_id = s.player_id "
        "WHERE s.token_hash = $1::bytea "
        "AND s.revoked_at IS NULL "
        "AND s.expires_at > statement_timestamp() "
        "AND a.deleted_at IS NULL "
        "AND NOT EXISTS ("
            "SELECT 1 FROM auth.bans AS b "
            "WHERE b.player_id = s.player_id "
            "AND b.revoked_at IS NULL "
            "AND b.banned_at <= statement_timestamp() "
            "AND (b.banned_until IS NULL "
                "OR b.banned_until > statement_timestamp())"
        ");";

    const char* parameters[] = {(const char*)token_hash};
    const int parameter_lengths[] = {SESSION_HASH_SIZE};
    const int parameter_formats[] = {1}; // Binary hash, not a string

    DatabaseValidationResult outcome = DATABASE_VALIDATION_ERROR;

    PGresult* result = PQexecParams(
        connection,
        query,
        1,
        NULL,
        parameters,
        parameter_lengths,
        parameter_formats,
        0);

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(
            stderr,
            "Session validation failed: %s",
            PQerrorMessage(connection));
        goto cleanup;
    }

    if (PQntuples(result) == 0) {
        outcome = DATABASE_VALIDATION_REJECTED;
        goto cleanup;
    }

    if (PQntuples(result) != 1 || PQnfields(result) != 2 ||
        PQgetisnull(result, 0, 0) || PQgetisnull(result, 0, 1)) {
        fprintf(stderr, "Session validation returned an unexpected result\n");
        goto cleanup;
    }

    // read the returned numbers
    // Column 0 = player id; column 1 = expiry
    int64_t values[2];

    for (int column = 0; column < 2; ++column) {
        const char* value_text = PQgetvalue(result, 0, column);
        char* end = NULL;

        errno = 0;
        intmax_t value = strtoimax(value_text, &end, 10);

        if (errno == ERANGE || end == value_text ||
            *end != '\0' || value <= 0 || value > INT64_MAX) {
            fprintf(stderr, "Session validation returned an invalid number\n");
            goto cleanup;
        }

        values[column] = (int64_t)value;
    }

    *out_player_id = values[0];
    *out_expires_at_unix = values[1];
    outcome = DATABASE_VALIDATION_VALID;

cleanup:
    PQclear(result);
    return outcome;
}

bool database_revoke_session(PGconn *connection, const unsigned char *token_hash)
{
    if (!connection || !token_hash) {
        return false;
    }

    if (PQtransactionStatus(connection) != PQTRANS_IDLE) {
        fprintf(stderr, "Session revocation requires an idle connection\n");
        return false;
    }

    const char* query =
        "UPDATE auth.sessions " // find matching session
        "SET revoked_at = statement_timestamp() " // record when databse started command
        "Where token_hash = $1::bytea "
        "AND revoked_at IS NULL;"; // leave already-revoked alone

    const char* parameters[] = {(const char*)token_hash};
    const int parameter_lengths[] = {SESSION_HASH_SIZE};
    const int parameter_formats[] = {1};

    PGresult* result = PQexecParams(
        connection,
        query,
        1,
        NULL,
        parameters,
        parameter_lengths,
        parameter_formats,
        0);

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(
            stderr,
            "Session revocation failed: %s",
            PQerrorMessage(connection));
        PQclear(result);
        return false;
    }

    PQclear(result);
    return true;
}
