#include "auth.h"
#include "provider.h"

#include <stdio.h>
#include <stdlib.h>

#include <curl/curl.h>

static AuthStatus login_status = AUTH_IDLE;

static CURLM *http_manager = NULL;
static CURL* login_request = NULL;
static struct curl_slist* upload_headers = NULL;
static bool upload_attempted = false;

// Discard response bytes, report the entire chunk as consumed.
static size_t discard_response_body(char* data, size_t item_size, size_t item_count, void* user_data)
{
    (void)data;
    (void)user_data;
    return item_size * item_count;
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
    struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/octet-stream");
    if(!headers) {
        curl_easy_cleanup(request);
        return false;
    }

    // Set the URL, timeout, and response callback, then queue the request.
    if (curl_easy_setopt(request, CURLOPT_URL, login_url) != CURLE_OK ||    
        curl_easy_setopt(request, CURLOPT_TIMEOUT, 10L) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||    // require httpS
        curl_easy_setopt(request, CURLOPT_POSTFIELDSIZE, (long)ticket_size) != CURLE_OK ||
        curl_easy_setopt(request, CURLOPT_POSTFIELDS, (const char*)ticket_bytes) != CURLE_OK || // Selects POST
        curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, discard_response_body) != CURLE_OK ||
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
    if (login_status == AUTH_PENDING || login_status == AUTH_READY) {
        return false;
    }

    if (!http_manager || !provider_request()) {
        login_status = AUTH_FAILED;
        return false;
    }

    upload_attempted = false;
    login_status = AUTH_PENDING;
    return true;
}

void auth_update(void)
{
    // Process provider callbacks without waiting for a ticket.
    provider_update();    

    if (login_status != AUTH_PENDING) {
        return;
    }

    if (provider_status() == AUTH_FAILED) {
        login_status = AUTH_FAILED;
        login_request_cleanup();
        return;
    }

    if (!upload_attempted && provider_status() == AUTH_READY) {
        upload_attempted = true;

        if (!start_ticket_upload()) {
            login_status = AUTH_FAILED;
            fprintf(stderr, "Could not start ticket upload\n");
        }
    }

    // No active HTTP request.
    if (!login_request) {
        return;
    }

    // Advance the existing request; active_request_count receives the unfinished count.
    int active_request_count = 0;
    CURLMcode update_result = curl_multi_perform(http_manager, &active_request_count);
    if (update_result != CURLM_OK) {
        // Stop processing after a multi-handle error.
        login_status = AUTH_FAILED;
        fprintf(stderr, "HTTP update failed: %s\n", curl_multi_strerror(update_result));
        login_request_cleanup();
        return;
    }

    // Number of completion notices remaining after each read.
    int messages_remaining = 0;
    CURLMsg* message;

    // Read local completion notices until the queue is empty.
    while ((message = curl_multi_info_read(http_manager, &messages_remaining))) {
        // DONE includes failed requests.
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        // HTTP status is separate from the libcurl result.
        long http_status = 0;
        CURLcode curl_result = message->data.result;

        if (curl_result == CURLE_OK) {
            // Read the stored HTTP status; curl_result now holds lookup success.
            curl_result = curl_easy_getinfo(login_request, CURLINFO_RESPONSE_CODE, &http_status);
        }

        if (curl_result != CURLE_OK) {
            login_status = AUTH_FAILED;
            fprintf(stderr, "Ticket upload failed: %s\n", curl_easy_strerror(curl_result));
        } else if (http_status != 200) {
            login_status = AUTH_FAILED;
            fprintf(stderr, "Ticket upload returned HTTP %ld\n", http_status);
        } else {
            login_status = AUTH_FAILED;
            puts("Ticket uploaded, but login verification is not implemented");
        }

        // Cleanup invalidates the notice and clears the active request.
        login_request_cleanup();
    }
}

AuthStatus auth_status(void)
{
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
    login_status = AUTH_IDLE;
}
