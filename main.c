#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include <curl/curl.h>
#include <civetweb.h>
#include <jansson.h>

#include "database.h"
#include "session.h"

typedef struct {
    char body[8192];
    size_t body_size;
} SteamResponse;

static bool bytes_to_hex(const unsigned char* bytes, size_t byte_count, char* hex, size_t hex_capacity) {
    static const char hex_digits[] = "0123456789abcdef";
    
    if (!bytes || !hex || hex_capacity == 0 || byte_count > (hex_capacity - 1) / 2) {
        return false;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        hex[i * 2] = hex_digits[bytes[i] >> 4];
        hex[i * 2 + 1] = hex_digits[bytes[i] & 0x0f];
    }

    hex[byte_count * 2] = '\0';
    return true;
}

// Copies the steam response to our GET request. Rename? copy_steam_response?
static size_t receive_steam_response(char* data, size_t item_size, size_t item_count, void* user_data)
{
    SteamResponse* response = user_data;
    size_t received_bytes = item_size * item_count;
    size_t available_bytes = sizeof(response->body) - response->body_size - 1;

    // room for terminator
    if (received_bytes > available_bytes) {
        return 0;
    }

    memcpy(response->body + response->body_size, data, received_bytes);
    response->body_size += received_bytes;
    response->body[response->body_size] = '\0';

    return received_bytes;
}

// Verifies service connection to steam 
static bool request_steam_verification(const char* ticket_hex, SteamResponse* response)
{
    const char* api_key = getenv("STEAM_WEB_API_KEY");
    if (!api_key || !api_key[0] || strpbrk(api_key, "\r\n")) {
        fprintf(stderr, "STEAM_WEB_API_KEY is missing or invalid\n");
        return false;
    }

    char request_url[6144];
    char key_header[256];

    int url_length = snprintf(request_url, sizeof(request_url),
        "https://api.steampowered.com/"
        "ISteamUserAuth/AuthenticateUserTicket/v1/"
        "?appid=480&identity=auth&ticket=%s",
        ticket_hex);

    int header_length = snprintf(key_header, sizeof(key_header), "x-webapi-key: %s", api_key);

    // Reject format errors or truncated strings
    if (url_length < 0 ||
        (size_t)url_length >= sizeof(request_url) ||
        header_length < 0 ||
        (size_t)header_length >= sizeof(key_header)) {
        fprintf(stderr, "Could not format Steam request\n");
        return false;
    }

    CURL* request = curl_easy_init();
    if (!request) {
        return false;
    }

    struct curl_slist* headers = curl_slist_append(NULL, key_header);
    if (!headers) {
        curl_easy_cleanup(request);
        return false;
    }

    response->body_size = 0;
    response->body[0] = '\0';

    if (curl_easy_setopt(request, CURLOPT_URL, request_url) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 3L) != CURLE_OK || // 3 second timeout for client_connection
        curl_easy_setopt(request, CURLOPT_TIMEOUT, 5L) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receive_steam_response) != CURLE_OK || // callback
        curl_easy_setopt(request, CURLOPT_WRITEDATA, (void*)response) != CURLE_OK) {
        fprintf(stderr, "Could not configure Steam request\n");
        curl_easy_cleanup(request);
        curl_slist_free_all(headers);
        return false;
    }

    // Send it
    CURLcode curl_result = curl_easy_perform(request);
    long http_status = 0;

    if (curl_result == CURLE_OK) {
        curl_result = curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &http_status);
    }

    curl_easy_cleanup(request);
    curl_slist_free_all(headers);

    if (curl_result != CURLE_OK) {
        fprintf(stderr, "Steam request failed: %s\n", curl_easy_strerror(curl_result));
        return false;
    }

    if (http_status != 200) {
        fprintf(stderr, "Steam returned HTTP %ld\n", http_status);
        return false;
    }

    return true;
}


// Checks server HTTP availability
static int health_handler(struct mg_connection *client_connection, void *user_data)
{
    (void)user_data;

    // Borrowed request info; valid for this callback.
    const struct mg_request_info *request_info = mg_get_request_info(client_connection);
   
    // Route matching can include longer paths; handle only the exact path.
    if (strcmp(request_info->local_uri, "/health") != 0) {
        return 0;
    }

    if (strcmp(request_info->request_method, "GET") != 0) {
        mg_printf(client_connection,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        // The response is already sent; return its status for CivetWeb's log.
        return 405;
    }

    // Content-Length counts the three body bytes in "ok\n".
    mg_printf(client_connection,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n\r\n"
        "ok\n");
    return 200;
}

/*
 *
 * So this is a longish one
 *
 * Basically, steam sends the client a token. When the client 
 * tries to login to the auth server with said token, it needs to be
 * verified that Steam actually sent out THAT token to THAT user. This
 * function facilitates that verification through a POST request, then 
 * it needs to call the session functions to actually start that 
 * player's session.
 *
 * The whole thing interacts with a database.h to query the PostgreSQL 
 * database whether the player exists. If not, it creates a new 
 * player_id for them, along with other metadata. 
 */
static int steam_login_handler(struct mg_connection *client_connection, void *user_data)
{
    (void)user_data;

    const struct mg_request_info* request_info = mg_get_request_info(client_connection);

    if (strcmp(request_info->local_uri, "/auth/steam") != 0) {
        return 0;
    }

    if (strcmp(request_info->request_method, "POST") != 0) {
        mg_printf(client_connection,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: POST\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        return 405;
    }

    // Steamworks Web API tickets fit in 2560 bytes.
    unsigned char ticket_data[2560];
    long long content_length = request_info->content_length;
    
    if (content_length < 0) {
        mg_send_http_error(client_connection, 411, "Content-Length is required");
        return 411;
    }

    if (content_length == 0) {
        mg_send_http_error(client_connection, 400, "Ticket body is empty");
        return 400;
    }

    if (content_length > (long long)sizeof(ticket_data)) {
        mg_send_http_error(client_connection, 413, "Ticket body is too large");
        return 413;
    }

    size_t expected_bytes = (size_t)content_length;
    size_t received_bytes = 0;
    // A read can return only part of the body.
    while (received_bytes < expected_bytes) {
        int bytes_read = mg_read(client_connection, ticket_data + received_bytes, expected_bytes - received_bytes);

        if (bytes_read <= 0) {
            mg_send_http_error(client_connection, 400, "Ticket body is incomplete");
            return 400;
        }

        received_bytes += (size_t)bytes_read;
    }

    // Steam wants tickets in hex for some reason
    char ticket_hex[sizeof(ticket_data) * 2 + 1];
    bytes_to_hex(ticket_data, received_bytes, ticket_hex, sizeof(ticket_hex));

    printf("Steam ticket body received (%zu bytes, %zu hex characters)\n", received_bytes, strlen(ticket_hex));

    SteamResponse response = {0};

    if (!request_steam_verification(ticket_hex, &response)) {
        mg_send_http_error(client_connection, 502, "Could not query Steam");
        return 502;
    }

    printf("Steam verification response received (%zu bytes)\n", response.body_size);

    // parse steam response
    json_error_t parse_error;
    json_t* root = json_loadb(response.body, response.body_size, JSON_REJECT_DUPLICATES, &parse_error);
    if(!root) {
        fprintf(stderr, "Could not parse Steam JSON: %s\n", parse_error.text);
        mg_send_http_error(client_connection, 502, "Invalid response from Steam");
        return 502;
    }

    // reject error responses
    json_t* steam_reply = json_object_get(root, "response");

    if (!json_is_object(steam_reply) || json_object_get(steam_reply, "error")) {
        fprintf(stderr, "Steam returned an error or unexpected response\n");
        json_decref(root);
        mg_send_http_error(client_connection, 502, "Could not verify with Steam");
        return 502;
    }

    json_t* params = json_object_get(steam_reply, "params");

    // check result and copy steam ID
    const char* result = json_string_value(json_object_get(params, "result"));

    json_t* steam_id_value = json_object_get(params, "steamid");
    const char* steam_id_text = json_string_value(steam_id_value);
    size_t steam_id_length = json_string_length(steam_id_value);

    char steam_id[21];

    if (!json_is_object(params) ||
        !result ||
        strcmp(result, "OK") != 0 ||
        !steam_id_text ||
        steam_id_length == 0 ||
        steam_id_length >= sizeof(steam_id) ||
        strspn(steam_id_text, "0123456789") != steam_id_length) {
        fprintf(stderr, "Steam verification result is missing or invalid\n");
        json_decref(root);
        mg_send_http_error(client_connection, 502, "Unexpected verification result");
        return 502;
    }

    // Keep ID after releasing json data
    memcpy(steam_id, steam_id_text, steam_id_length + 1);
    json_decref(root);

    printf("Steam ticket verified for SteamID %s\n", steam_id);

    // Open database client_connection
    PGconn* database_connection = database_connect();
    if (!database_connection) {
        mg_send_http_error(client_connection, 503, "Database unavailable");
        return 503;
    }
    puts("Connected to PostgreSQL");

    // Find internal ID associated with steam account
    int64_t player_id = 0;
    DatabaseLookupResult lookup_result = database_find_player(
        database_connection, 
        AUTH_PROVIDER_STEAM, 
        steam_id, 
        &player_id);

    if (lookup_result == DATABASE_LOOKUP_ERROR) {
        PQfinish(database_connection);
        mg_send_http_error(client_connection, 503, "Account lookup failed");
        return 503;
    }

    if (lookup_result == DATABASE_LOOKUP_NOT_FOUND) {
        DatabaseCreateResult create_result = database_create_player(
            database_connection, 
            AUTH_PROVIDER_STEAM,
            steam_id,
            &player_id);

        if (create_result == DATABASE_CREATE_ERROR) {
            PQfinish(database_connection);
            mg_send_http_error(client_connection, 503, "Account creation failed");
            return 503;
        }

        if (create_result == DATABASE_CREATE_CONFLICT) {
            // another request created account first
            lookup_result = database_find_player(
                database_connection,
                AUTH_PROVIDER_STEAM,
                steam_id,
                &player_id);

            if (lookup_result != DATABASE_LOOKUP_FOUND) {
                PQfinish(database_connection);
                mg_send_http_error(client_connection, 503, "Could not resolve account");
                return 503;
            }
        } else {
            printf("Created player ID: %" PRId64 "\n", player_id);
        }
    }
    
    // Generate session token and hash
    unsigned char session_token[SESSION_TOKEN_SIZE];
    unsigned char token_hash[SESSION_HASH_SIZE];

    if (!session_generate_token(session_token, token_hash)) {
        PQfinish(database_connection);
        mg_send_http_error(client_connection, 503, "Could not generate session token");
        return 503;
    }

    // Create new session
    const int lifetime_seconds = 60 * 60;
    int64_t expires_at_unix = 0;
    DatabaseSessionResult session_result = database_create_session(
        database_connection, 
        player_id,
        token_hash,
        lifetime_seconds,
        &expires_at_unix);
    PQfinish(database_connection);

    if (session_result == DATABASE_SESSION_DELETED) {
        mg_send_http_error(client_connection, 403, "Account is deleted");
        return 403;
    }

    if (session_result == DATABASE_SESSION_BANNED) {
        mg_send_http_error(client_connection, 403, "Account is banned");
        return 403;
    }

    if (session_result != DATABASE_SESSION_CREATED) {
        mg_send_http_error(client_connection, 503, "Could not create session");
        return 503;
    }

    printf("Resolved player ID: %" PRId64 "\n", player_id);

    // Send response to client
    char session_token_hex[sizeof(session_token) * 2 + 1];
    bytes_to_hex(session_token, sizeof(session_token), session_token_hex, sizeof(session_token_hex));

    char response_body[256];

    snprintf(response_body, sizeof(response_body), 
            "{\"session_token\":\"%s\",\"expires_at\":%" PRId64 "}", 
            session_token_hex, expires_at_unix);

    // Send client session_token and expires_at
    mg_printf(client_connection,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        strlen(response_body),
        response_body);

    return 200;
}

int main(void)
{
    // Setting/value pairs, NULL terminated.
    const char *options[] = {
        // Accept local traffic from the HTTPS proxy.
        "listening_ports", "127.0.0.1:8080",
        "num_threads", "2",
        NULL
    };

    // CURL INIT
    CURLcode curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_result != CURLE_OK) {
        fprintf(stderr, "Could not initialize curl: %s\n", curl_easy_strerror(curl_result));
        return EXIT_FAILURE;
    }

    // No optional features
    (void)mg_init_library(0);

    struct mg_context *server = mg_start(NULL, NULL, options);
    if(!server) {
        fprintf(stderr, "Could not start auth server\n");
        mg_exit_library();
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    // CivetWeb calls these handlers on its worker threads.
    mg_set_request_handler(server, "/health", health_handler, NULL);
    mg_set_request_handler(server, "/auth/steam", steam_login_handler, NULL);

    puts("Auth server listening on http://127.0.0.1:8080");
    puts("Press Enter to stop.");
    // Interactive shutdown for now; closed stdin also ends the server.
    (void)getchar();

    // Wait for workers to finish before freeing library state.
    mg_stop(server);
    mg_exit_library();
    curl_global_cleanup();
    return EXIT_SUCCESS;
}
