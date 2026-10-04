#include "auth.h"
#include "provider.h"

#include <curl/curl.h> // libcurl sends HTTP requests from this client.

/*
 * For the health check, one HTTP exchange means sending GET /health and
 * receiving the server's status, headers, and body. libcurl calls the whole
 * operation a "transfer", including connecting and setting up HTTPS.
 *
 * CURLM is libcurl's object for tracking the requests added to it and
 * advancing their network work when asked. http points to that object.
 * A pointer used to refer to a library object is often called a "handle".
 * Pass it to libcurl functions; libcurl keeps its internal fields private.
 *
 * This file-level variable stays available between function calls. static
 * makes its name private to this file. NULL means no object exists yet.
 */
static CURLM *http = NULL;

// TEST
#include <stdio.h>  // fprintf, stderr, and puts for diagnostic output.
#include <stdlib.h> // getenv reads settings from the program's environment.

/*
 * CURL describes an individual request: its URL, timeout, how to handle
 * received data, and its progress. health points to the health request.
 *
 * "easy" is libcurl's name for the interface that creates and configures
 * these individual objects. The "multi" interface processes them. Both
 * names appear here because a multi handle works with easy handles.
 *
 * TEST marks the temporary health experiment. Some processing code outside
 * these markers still refers to health_cleanup and these includes. Reusing
 * it for ticket uploads will mean adapting those references together.
 */
static CURL* health = NULL;

/*
 * libcurl calls this function when response-body bytes arrive while
 * curl_multi_perform is processing the request. For /health, the body is
 * "ok\n". HTTP headers and the status code aren't passed here.
 *
 * data points to the current chunk of bytes. The body can arrive in several
 * chunks, and a chunk isn't guaranteed to end with a C string's '\0'.
 * size * count is the chunk's byte count; libcurl sets size to 1 here.
 * user_data is an optional pointer supplied through CURLOPT_WRITEDATA.
 * This callback doesn't need any extra data, so that option isn't set.
 *
 * For this test, discard the body and check the HTTP status later. Returning
 * the byte count tells libcurl the entire chunk was handled. A smaller
 * return value would make libcurl fail the request with a write error.
 * This return value goes to libcurl inside the client; it sends no reply
 * to the server.
 */
static size_t health_body(char* data, size_t size, size_t count, void* user_data)
{
    // Mark these arguments as intentionally unused. This doesn't change
    // either pointer or free the memory it points to.
    (void)data;
    (void)user_data;
    return size * count;
}

// Used after completion, on a processing error, or if the game closes while
// the request is still pending. There is only one HTTP request in this test.
static void health_cleanup(void)
{
    // No request was started, or an earlier call already cleaned it up.
    if (!health) {
        return;
    }

    // Detach it first so http stops tracking it. This also cancels it if
    // it hasn't finished. Removing a handle doesn't free the request object.
    curl_multi_remove_handle(http, health);
    // Free the request object, then clear the pointer so later code knows
    // there is no health request left to process.
    curl_easy_cleanup(health);
    health = NULL;
}

// END TEST 

bool auth_init(void)
{
    // Initialize the selected login provider, currently Steam. This starts
    // its client integration; auth_request asks it for a ticket separately.
    if (!provider_init()) {
        return false;
    }

    // Prepare libcurl's process-wide state before creating request objects.
    // CURL_GLOBAL_DEFAULT selects its normal initialization features.
    // CURLE_OK means this library call succeeded; it isn't an HTTP status.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        // Steam was already initialized, so undo that work on failure.
        provider_shutdown();
        return false;
    }

    // Create an empty object for tracking and processing HTTP requests.
    // No server is contacted by this call. auth_update will ask libcurl
    // to advance the requests each frame, then return control to the game.
    http = curl_multi_init();
    if (!http) {
        // Creation failed. Release the resources initialized above.
        curl_global_cleanup();
        provider_shutdown();
        return false;
    }

    // TEST 
    // Read the optional test URL from the environment passed to this program.
    // getenv returns NULL when the setting is absent. Its string belongs to
    // the environment, so this code doesn't modify or free it.
    const char* url = getenv("AUTH_HEALTH_URL");

    // && checks url first, so url[0] is only read if the pointer is valid.
    // An absent or empty setting skips the health test entirely.
    if (url && url[0] != '\0') {
        // Create one request object. A new HTTP request defaults to GET.
        // This allocates its state; it doesn't send the request yet.
        CURL* request = curl_easy_init();
        if (!request) {
            auth_shutdown();
            return false;
        }

        /*
         * URL: where to connect and which path to request. An https:// URL
         * selects HTTPS; libcurl handles encryption and certificate checks.
         *
         * TIMEOUT: allow ten seconds for the entire operation, including
         * connecting and receiving the response. 10L has type long, which
         * this option requires. It doesn't make this setup call wait.
         *
         * WRITEFUNCTION: the function that receives response-body bytes.
         * "Write" means writing received data into this application.
         * health_body without () passes a function pointer. It isn't called
         * here; libcurl calls it later when data arrives.
         *
         * Finally, add the configured request to http. Network processing
         * begins when auth_update calls curl_multi_perform.
         *
         * || stops evaluating after the first failure. That keeps a partly
         * configured request from being added. These setopt calls return
         * CURLcode values; add_handle returns a CURLMcode. Their success
         * constants are CURLE_OK and CURLM_OK respectively.
         */
        if (curl_easy_setopt(request, CURLOPT_URL, url) != CURLE_OK ||
            curl_easy_setopt(request, CURLOPT_TIMEOUT, 10L) != CURLE_OK ||
            curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, 
                             health_body) != CURLE_OK ||
            curl_multi_add_handle(http, request) != CURLM_OK) {
            fprintf(stderr, "Could not prepare health request\n");
            // health hasn't been assigned yet. Free the local request here,
            // then release the initialized http, libcurl, and provider state.
            curl_easy_cleanup(request);
            auth_shutdown();
            return false;
        }

        // Copy the pointer, not the request object. request is a local
        // variable; health keeps the object accessible after auth_init
        // returns. Assign it only after http successfully accepts it.
        health = request;
    }

    // END TEST

    // Initialization succeeded. A queued health request can still fail later
    // when auth_update actually tries to connect or receives a response.
    return true;
}

void auth_shutdown(void)
{
    // TEST
    // Remove and free the individual request before freeing its manager.
    health_cleanup();
    // END TEST

    if (http) {
        // Free the multi object. It doesn't free individual easy handles,
        // which is why health_cleanup has to run first.
        curl_multi_cleanup(http);
        http = NULL;
    }

    // Release libcurl's process-wide state after its handles are gone.
    curl_global_cleanup();
    provider_shutdown();
}

bool auth_request(void)
{
    // This still only asks the provider for a ticket. It doesn't start the
    // health request or send a ticket to the auth server yet.
    return provider_request();
}

void auth_update(void)
{
    // Give Steam an opportunity to deliver its callbacks. This returns even
    // when the ticket is still pending. The health request below runs
    // independently; it doesn't wait for AUTH_READY.
    provider_update();

    // TEST
    // Skip HTTP processing if the test wasn't enabled or is already over.
    if (!health) {
        return;
    }
    // END TEST

    /*
     * Continue the request from wherever the previous frame left it.
     * libcurl may advance the connection, send request bytes, receive
     * response bytes, or find that nothing is ready yet. It returns control
     * so the game can keep running while the request is unfinished.
     * Calling this each frame continues the same request; it doesn't send
     * a fresh GET /health every frame.
     *
     * &running gives libcurl the address of this int so it can write the
     * number of unfinished requests into it. Here that can be 0 or 1.
     * Completion messages below explain how each request ended.
     */
    int running = 0;
    CURLMcode result = curl_multi_perform(http, &running);
    if (result != CURLM_OK) {
        // This reports a problem advancing the group of requests. Errors
        // for an individual request, such as a timeout, are checked below.
        // Stop this test on a group-level error rather than continuing it.
        fprintf(stderr, "HTTP update failed: %s\n", curl_multi_strerror(result));
        health_cleanup();
        return;
    }

    // remaining receives the number of notices left after reading one.
    // message points to a notice stored by libcurl inside this process.
    int remaining = 0;
    CURLMsg* message;

    /*
     * Read libcurl's completion notices. These describe how requests ended;
     * any response-body bytes were already delivered to health_body.
     * A failure can also produce a notice without any server response.
     *
     * Each call removes one notice from the queue. NULL means none are
     * waiting, so the loop ends immediately. This loop doesn't wait for
     * network data or ask the server for updates.
     *
     * The assignment stores the returned pointer in message, then the while
     * condition checks that pointer. The extra parentheses make this
     * intentional assignment easier to distinguish from a comparison.
     */
    while ((message = curl_multi_info_read(http, &remaining))) {
        // DONE means the operation finished, including if it failed.
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        // status will hold the server's HTTP response code, such as 200.
        // libcurl requires a pointer to long for CURLINFO_RESPONSE_CODE.
        long status = 0;
        // This result belongs to the individual request. CURLE_OK means the
        // exchange completed without a libcurl error. It can still have
        // received HTTP 404 or 500, so check the HTTP status separately.
        CURLcode transfer_result = message->data.result;
// TEST
        if (transfer_result == CURLE_OK) {
            // Read the status already received and stored by libcurl. This
            // sends nothing. &status lets libcurl write into the variable.
            // transfer_result is reused here: after this call it describes
            // whether looking up that stored status succeeded.
            transfer_result = curl_easy_getinfo(health, CURLINFO_RESPONSE_CODE, &status);
        }

        if (transfer_result != CURLE_OK) {
            fprintf(stderr, "Health request failed: %s\n", curl_easy_strerror(transfer_result));
        } else if (status != 200) {
            fprintf(stderr, "Health request returned HTTP %ld\n", status);
        } else {
            puts("Health request succeeded: HTTP 200");
        }

        // Consume the notice's information before cleanup: removing/freeing
        // the handle invalidates the notice pointer. Clearing health also
        // makes the next frame skip HTTP processing for this finished test.
        health_cleanup();
// END TEST
    }
}

AuthStatus auth_status(void)
{
    // This enum still describes the provider's ticket request. It doesn't
    // report the health check or whether a server has accepted the ticket.
    return provider_status();
}
