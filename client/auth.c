/*
 * auth_request() asks the provider code for a login ticket. Currently, that
 * means asking Steam for a ticket for the signed in account. The ticket
 * arrives later. auth_request() returns without waiting for the response.
 *
 * The client should call auth_update() each frame. Once the ticket arrives, it
 * starts and HTTPS request to send the ticket's bytes to the auth server.
 * Further calls let libcurl send the request and receive a response.
 *
 * The server validates the ticket with Steam and creates a session. Its
 * response contains a secret token and the time it will expire. Token and
 * expiry are saved to the client as session_token_hex[] and
 * session_expires_at_unix, and are checked inside auth_update().
 *
 * Before expiry, auth_update() will request another Steam ticket and repeat
 * the login process. On a retryable failure, it will schedule a ticket
 * request 2 seconds from now. Then 5. Then 10. On the last failure, it will
 * cancel all requests and stop trying.
 */
#include "auth.h"
#include "provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <curl/curl.h>
#include <jansson.h>

typedef struct {
    char body[4096];
    size_t body_size;
} LoginResponse;

static char session_token_hex[65];
static int64_t session_expires_at_unix = 0;
static int64_t next_login_attempt_at_unix = 0;
static size_t login_retry_count = 0;
static LoginResponse login_response;
static AuthStatus login_status = AUTH_IDLE;
static CURLM* http_manager = NULL;
static CURL* login_request = NULL;
static struct curl_slist* upload_headers = NULL;
static bool upload_attempted = false;

/*
 * Simple. Check how many times we've attempted reauthentication and try again.
 * Sets next_login_attempt_at_unix to 2, 5, and 10 seconds from now, depending
 * on how many times client has tried. This variable is used in auth_update
 * to let the client know when to reauthenticate.
 */
static void schedule_login_retry(void)
{
    static const int retry_delays[] = {2, 5, 10}; // waiting periods
    const size_t retry_limit = sizeof(retry_delays) / sizeof(retry_delays[0]);

    next_login_attempt_at_unix = 0;

    if (login_retry_count >= retry_limit) {
        fprintf(stderr, "Login retry limit reached\n");
        return;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        fprintf(stderr, "Could not read current time\n");
        return;
    }

    int delay_seconds = retry_delays[login_retry_count];
    next_login_attempt_at_unix = (int64_t)now + delay_seconds;
    ++login_retry_count;

    fprintf(stderr, "Retrying login in %d seconds\n", delay_seconds);
}

/*
 * Callback for back for http request. Captures raw response data from server
 *
 * libcurl calls this whenever another piece of the response body arrives.
 * user_data points to the LoginResponse buffer assigned to the request.
 * Each piece is appended to that bugger, leaving room for the final '\0'.
 *
 * The return value tells curl how many bytes were accpeted. A piece that
 * won't fit returns zero, causing curl to abort the request. JSON parsing
 * happens later, after the complete response arrives
 */
static size_t receive_login_response(
    char* data,
    size_t item_size,
    size_t item_count,
    void* user_data)
{
    LoginResponse* response = user_data;
    size_t received_bytes = item_size * item_count;
    size_t available_bytes = sizeof(response->body) - response->body_size - 1;
    if (received_bytes > available_bytes) {
        return 0;
    }

    memcpy(response->body + response->body_size, data, received_bytes);
    response->body_size += received_bytes;
    response->body[response->body_size] = '\0';

    return received_bytes;
}

/*
 * The complete response body is parsed as JSON. Valid replies contain a
 * 64 character lowercase hex token and an ineger Unix expiry time later than
 * the client's current time.
 *
 * Valid replies replace the saved session. Renewal is scheduled halfway
 * through its remaining lifetime, and the retry count returns to zero.
 * A rejected reply leaves the previous session unchanged.
 *
 * The token is copied before the JSON document containing it is released.
 */
static bool parse_login_response(void)
{
    json_t* root = json_loadb(
        login_response.body,
        login_response.body_size,
        JSON_REJECT_DUPLICATES,
        NULL);

    if (!root) {
        return false;
    }

    // Get each field
    json_t* token_value = json_object_get(root, "session_token");
    json_t* expiry_value = json_object_get(root, "expires_at");

    // require 64 char token and int expiry
    if (!json_is_string(token_value) ||
        json_string_length(token_value) != sizeof(session_token_hex) - 1 ||
        !json_is_integer(expiry_value)) {
        json_decref(root);
        return false;
    }

    const char* token_text = json_string_value(token_value);
    int64_t expires_at = (int64_t)json_integer_value(expiry_value);
    time_t now = time(NULL);

    // require lowercase hex and FUTURE expiry
    if (strspn(token_text, "0123456789abcdef") !=
            sizeof(session_token_hex) - 1 ||
        now == (time_t)-1 ||
        expires_at <= (int64_t)now) {
        json_decref(root);
        return false;
    }

    memcpy(session_token_hex, token_text, sizeof(session_token_hex));
    session_expires_at_unix = expires_at;
    int64_t remaining_seconds = expires_at - (int64_t)now;
    next_login_attempt_at_unix = expires_at - remaining_seconds / 2;
    login_retry_count = 0;

    json_decref(root);
    return true;
}

// Removes the active libcurl request, even if it is still pending.
static void login_request_cleanup(void)
{
    if (login_request) {
        curl_multi_remove_handle(http_manager, login_request);
        curl_easy_cleanup(login_request);
        login_request = NULL;
    }

    curl_slist_free_all(upload_headers);
    upload_headers = NULL;
}

/*
 * The provider's ticket becomes the body of an HTTPS POST to AUTH_LOGIN_URL.
 * This is where the request is setup and added to http_manager. auth_update()
 * then runs the connection, send, and receive ops with libcurl.
 *
 * curl reads from the provider's ticket buffer. Those bytes remain valid until
 * the request finishes or is removed during cleanup. The request handle and
 * HTTP headers are saved for that cleanup.
 *
 * Returning true means the request was queued, no that the login succeeded.
 */
static bool start_ticket_upload(void)
{
    // Development login endpoint.
    const char* login_url = getenv("AUTH_LOGIN_URL");

    if (!http_manager || login_request || !login_url || login_url[0] == '\0') {
        return false;
    }

    // Get the ticket data
    int ticket_size = 0;
    const unsigned char* ticket_bytes = provider_ticket_data(&ticket_size);
    if (!ticket_bytes || ticket_size <= 0) {
        return false;
    }

    // New handles default to GET.
    CURL* request = curl_easy_init();
    if (!request) {
        return false;
    }

    // Build HTTP header
    struct curl_slist* headers =
        curl_slist_append(NULL, "Content-Type: application/octet-stream");

    if (!headers) {
        curl_easy_cleanup(request);
        return false;
    }

    // Prevent another login from appending previous response
    login_response.body_size = 0;
    login_response.body[0] = '\0';

    // Set the URL, timeout, and response callback, then queue the request.
    if (curl_easy_setopt(request, CURLOPT_URL, login_url) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_TIMEOUT, 10L) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK || // require httpS
        curl_easy_setopt(request, CURLOPT_POSTFIELDSIZE, (long)ticket_size) !=
            CURLE_OK ||
        curl_easy_setopt(
            request,
            CURLOPT_POSTFIELDS,
            (const char*)ticket_bytes) != CURLE_OK || // Selects POST
        curl_easy_setopt(
            request,
            CURLOPT_WRITEFUNCTION,
            receive_login_response) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_WRITEDATA, (void*)&login_response) !=
            CURLE_OK ||
        curl_multi_add_handle(http_manager, request) != CURLM_OK) {
        curl_easy_cleanup(request);
        curl_slist_free_all(headers);
        return false;
    }

    // Keep the handle for updates and cleanup.
    login_request = request;
    upload_headers = headers;
    return true;
}

bool auth_init(void)
{
    if (!provider_init()) {
        login_status = AUTH_FAILED;
        return false;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        login_status = AUTH_FAILED;
        provider_shutdown();
        return false;
    }

    // request manager; this starts no network activity.
    http_manager = curl_multi_init();
    if (!http_manager) {
        login_status = AUTH_FAILED;
        curl_global_cleanup();
        provider_shutdown();
        return false;
    }

    login_status = AUTH_IDLE;
    return true;
}

bool auth_request(void)
{
    // Reject duplicate requests to not overwrite current
    if (login_status == AUTH_PENDING) {
        return false;
    }

    next_login_attempt_at_unix = 0;

    if (!http_manager || !provider_request()) {
        login_status = AUTH_FAILED;
        return false;
    }

    upload_attempted = false;
    login_status = AUTH_PENDING;
    return true;
}

/*
 * Call this each frame on the client.
 *
 * Checks the saved session, starts scheduled logins, and handles any login
 * attempt already underway.
 *
 * 1. provider_update() checks for provider ticket response or timeout. This
 *    updates the provider's ticket data and status.
 *
 * 2. An expired session loses its saved token and expiry time. A pending
 *    replacement request keeps running. A clock failure also stops that
 *    request and clears the automatic login schedule.
 *
 * 3. A due renewal or retry starts another login through auth_request(),
 *    provided no login attempt is already pending.
 *
 * 4. With no pending attempt, the function returns. Otherwise, a provider
 *    failure ends the attempt. A ready ticket starts one HTTPS upload.
 *    While the ticket is still pending, there is no HTTP request yet.
 *
 * 5. curl_multi_perform() handles connection setup, sending the ticket,
 *    receiving the server's response as those operations become possible.
 *    Received body bytes go to receive_login_reponse.
 *
 * 6. curl_multi_info_read() reports completed requests, including failures.
 *    The result is checked for a curl error, an unsuccessful HTTP status,
 *    or an invalid response body. A valid login response replaces the saved
 *    session, schedules its renewal, and resets the retry count.
 *
 *    Certain network errors and HTTP 502/503/504 schedule delayed retries.
 *    HTTP 401/403 clears the saved session and stops automatic attempts.
 *
 * 7. Completion cleanup releases the HTTP request and cancels its provider
 *    ticket. Cancelling that ticket does not revoke a TinyShip session.
 */
void auth_update(void)
{
    // 1.
    // Process provider callbacks without waiting for a ticket. Just Steam
    // implemented atm
    provider_update();

    // 2.
    // Check session expiration
    if (session_expires_at_unix != 0) {
        time_t now = time(NULL);

        if (now == (time_t)-1 || (int64_t)now >= session_expires_at_unix) {
            memset(session_token_hex, 0, sizeof(session_token_hex));
            session_expires_at_unix = 0;

            if (now == (time_t)-1) { // specifically clock failure
                login_request_cleanup();
                provider_cancel();
                login_status = AUTH_FAILED;
                fprintf(stderr, "Could not read current time\n");
                next_login_attempt_at_unix = 0;
                return;
            }

            if (login_status == AUTH_READY) {
                login_status = AUTH_IDLE;
            }

            puts("Session expired");
        }
    }

    // 3.
    // Check for due renewal and request authentication if so
    if (login_status != AUTH_PENDING && next_login_attempt_at_unix != 0) {
        time_t now = time(NULL);

        if (now == (time_t)-1) {
            next_login_attempt_at_unix = 0;
            login_status = AUTH_FAILED;
            fprintf(stderr, "Could not read current time\n");
            return;
        }

        if ((int64_t)now >= next_login_attempt_at_unix) {
            if (!auth_request()) {
                fprintf(stderr, "Could not start session renewal\n");
            }
        }
    }

    // 4.
    if (login_status != AUTH_PENDING) {
        return;
    }

    if (provider_status() == AUTH_FAILED) {
        login_status = AUTH_FAILED;
        login_request_cleanup();
        provider_cancel();
        return;
    }

    // Tue provider stays ready across ticks. upload_attempted prevents
    // repeated uploads
    if (!upload_attempted && provider_status() == AUTH_READY) {
        upload_attempted = true;

        if (!start_ticket_upload()) {
            login_status = AUTH_FAILED;
            fprintf(stderr, "Could not start ticket upload\n");
            login_request_cleanup();
            provider_cancel();
            return;
        }
    }

    // No active HTTP request.
    if (!login_request) {
        return;
    }

    // 5.
    // Advance the existing request. active_request_count receives the
    // unfinished count
    int active_request_count = 0;
    CURLMcode update_result =
        curl_multi_perform(http_manager, &active_request_count);
    if (update_result != CURLM_OK) {
        // Stop processing after a multi-handle error.
        login_status = AUTH_FAILED;
        fprintf(
            stderr,
            "HTTP update failed: %s\n",
            curl_multi_strerror(update_result));
        login_request_cleanup();
        provider_cancel();
        return;
    }

    // 6.
    // Number of completion notices remaining after each read.
    int messages_remaining = 0;
    CURLMsg* message;
    // Read local completion notices until the queue is empty.
    while (
        (message = curl_multi_info_read(http_manager, &messages_remaining))) {
        // DONE includes failed requests.
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        // HTTP status is separate from the libcurl result.
        long http_status = 0;
        CURLcode curl_result = message->data.result;

        if (curl_result == CURLE_OK) {
            // Read stored HTTP status; curl_result now holds lookup success
            curl_result = curl_easy_getinfo(
                login_request,
                CURLINFO_RESPONSE_CODE,
                &http_status);
        }

        if (curl_result != CURLE_OK) {
            login_status = AUTH_FAILED;
            fprintf(
                stderr,
                "Ticket upload failed: %s\n",
                curl_easy_strerror(curl_result));

            if (curl_result == CURLE_COULDNT_RESOLVE_PROXY ||
                curl_result == CURLE_COULDNT_RESOLVE_HOST ||
                curl_result == CURLE_COULDNT_CONNECT ||
                curl_result == CURLE_OPERATION_TIMEDOUT ||
                curl_result == CURLE_SEND_ERROR ||
                curl_result == CURLE_RECV_ERROR ||
                curl_result == CURLE_GOT_NOTHING ||
                curl_result == CURLE_PARTIAL_FILE) {
                schedule_login_retry();
            }
        } else if (http_status != 200) {
            login_status = AUTH_FAILED;

            if (http_status == 401 || http_status == 403) {
                memset(session_token_hex, 0, sizeof(session_token_hex));
                session_expires_at_unix = 0;
                next_login_attempt_at_unix = 0;
            } else if (http_status == 502 ||
                       http_status == 503 ||
                       http_status == 504) {
                schedule_login_retry();
            }

            fprintf(stderr, "Ticket upload returned HTTP %ld\n", http_status);
        } else if (!parse_login_response()) {
            login_status = AUTH_FAILED;
            puts("Invalid login response");
        } else {
            login_status = AUTH_READY;
            puts("Login complete");
        }

        // 7.
        // Cleanup invalidates the notice and clears the active request.
        login_request_cleanup();
        provider_cancel();
    }
}

/*
 * Unexpired saved sessions report AUTH_READY, even while a replacement login
 * is pending or after a renewal attempt has failed.
 *
 * Without a usable session, result is AUTH_PENDING, AUTH_FAILED, or AUTH_IDLE.
 * An earlier successful login reports AUTH_IDLE once its session is gone or
 * expired. A clock read failure reports AUTH_FAILED.
 *
 * This function only reports status. The stored state is unchanged.
 */
AuthStatus auth_status(void)
{
    if (session_expires_at_unix != 0) {
        time_t now = time(NULL);

        if (now == (time_t)-1) {
            return AUTH_FAILED;
        }

        if ((int64_t)now < session_expires_at_unix) {
            return AUTH_READY;
        }
    }

    if (login_status == AUTH_READY) {
        return AUTH_IDLE;
    }

    return login_status;
}

void auth_shutdown(void)
{
    // Free the request before its manager.
    login_request_cleanup();

    if (http_manager) {
        curl_multi_cleanup(http_manager);
        http_manager = NULL;
    }

    curl_global_cleanup();
    provider_shutdown();
    memset(session_token_hex, 0, sizeof(session_token_hex));
    session_expires_at_unix = 0;
    next_login_attempt_at_unix = 0;
    login_retry_count = 0;
    login_status = AUTH_IDLE;
}
