#include <civetweb.h> // HTTP server, connections, and request info.
#include <stdio.h>    // printf-style output, puts, and getchar.
#include <stdlib.h>   // EXIT_SUCCESS and EXIT_FAILURE.
#include <string.h>   // strcmp, for comparing strings by their contents.

/*
 * Just a quick "is the server answering HTTP requests?" check.
 * 200 is HTTP's standard "OK" status. Here it only means the server answered
 * this request. Steam, accounts, and database health aren't checked here.
 *
 * CivetWeb calls this function when a request matches the route registered
 * in main. connection points to its connection object. Pass that pointer
 * to CivetWeb to inspect the request or send a response.
 *
 * user_data is an optional pointer to application data, like server settings.
 * It's called void * because the API doesn't know what type is stored there.
 * This handler is registered with NULL, so there's nothing there yet.
 *
 * static keeps this function's name private to this source file. CivetWeb
 * can still receive a pointer to it and call it later.
 */
static int health_handler(struct mg_connection *connection, void *user_data)
{
    // Intentionally unused for now. Casting the expression to void discards
    // its value and avoids an unused-parameter warning. The pointer itself
    // isn't changed or freed.
    (void)user_data;

    /*
     * Ask CivetWeb for the request info belonging to this connection.
     * This returns a pointer to an existing struct; it doesn't convert
     * the connection struct or make a copy of the request.
     *
     * const prevents changes to that struct through this pointer.
     * The pointer variable itself could still be made to point elsewhere.
     * request->local_uri reads the local_uri member through the pointer.
     * CivetWeb owns this info, so don't free it or keep it after the callback.
     */
    const struct mg_request_info *request = mg_get_request_info(connection);
   
    /*
     * strcmp returns 0 when the strings are equal. A negative or positive
     * result means they differ. So != 0 means "these strings don't match".
     *
     * CivetWeb's route matching can also catch longer paths. This handler
     * only accepts /health itself, so leave other paths for CivetWeb.
     * Returning 0 here means "request not handled". It doesn't send
     * an HTTP response, and there isn't an HTTP status code 0.
     */
    if (strcmp(request->local_uri, "/health") != 0) {
        return 0;
    }

    /*
     * Same comparison again: reject anything OTHER than GET.
     * GET asks to retrieve something; POST will be used for ticket uploads.
     *
     * 405 is the standard HTTP "Method Not Allowed" status, and Allow tells
     * the client which method this endpoint accepts. This response has no
     * body, so Content-Length is 0.
     */
    if (strcmp(request->request_method, "GET") != 0) {
        mg_printf(connection,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        // mg_printf sent the response. This return marks the request as handled
        // and gives CivetWeb the matching status code for its access log.
        return 405;
    }

    /*
     * mg_printf writes to this connection, whereas printf writes to stdout.
     * These adjacent C string literals become one string automatically.
     *
     * First comes the HTTP status line, then the headers, then a blank line,
     * then the body. HTTP header lines end with \r\n. The extra \r\n makes
     * that blank line separating headers from the body.
     *
     * text/plain describes the body. "ok\n" is 3 bytes: o, k, and a newline.
     * The C string's terminating \0 isn't sent or included in that length.
     * Connection: close says to close this connection after the response.
     */
    mg_printf(connection,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n\r\n"
        "ok\n");
    // Again, returning 200 alone wouldn't send anything to the client.
    return 200;
}

static int steam_login_handler(struct mg_connection *connection, void *user_data)
{
    (void)user_data;

    // Inspecting recieved request
    const struct mg_request_info* request = mg_get_request_info(connection);

    if (strcmp(request->local_uri, "/auth/steam") != 0) {
        return 0;
    }

    // Accept data submitted using POST
    if (strcmp(request->request_method, "POST") != 0) {
        mg_printf(connection,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: POST\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        return 405;
    }

    // Read and verification will go here //
    mg_printf(connection,
        "HTTP/1.1 501 Not Implemented\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    return 501;
}

int main(void)
{
    /*
     * An array of pointers to strings. Each element is a const char *,
     * so the characters are read-only through that pointer.
     * When passed to mg_start, the array becomes a pointer to its first
     * element. That's why the function receives a const char **.
     *
     * CivetWeb reads these as setting/value pairs. NULL marks the end of
     * the array; it isn't another string or the text "NULL".
     */
    const char *options[] = {
        // Listen on this computer's loopback address, TCP port 8080.
        // A local HTTPS proxy can forward requests here. This address
        // isn't specific to Tailscale; remote clients can't reach it directly.
        "listening_ports", "127.0.0.1:8080",
        // Two worker threads handle requests while main waits below.
        "num_threads", "2",
        NULL
    };

    /*
     * Initialize CivetWeb's process-wide state. 0 requests no optional
     * features, such as TLS. For this call, 0 can also be a successful
     * return value, so don't read it as a true/false success flag.
     * Discard it and check whether starting the server succeeds below.
     */
    (void)mg_init_library(0);

    /*
     * Start listening and start the server's threads inside this process.
     * The first NULL means no extra callback hooks; the second means
     * no server-wide user data. The request handler gets registered below.
     *
     * server points to CivetWeb's server state, or is NULL if startup fails.
     */
    struct mg_context *server = mg_start(NULL, NULL, options);
    // !server is true when server is NULL.
    if(!server) {
        fprintf(stderr, "Could not start auth server\n");
        // Library initialization still needs cleanup if server startup fails.
        mg_exit_library();
        // This is the program's exit status for the OS, not an HTTP status.
        return EXIT_FAILURE;
    }

    /*
     * Register the function to call when a request matches /health.
     * health_handler without () passes a function pointer; it doesn't call
     * the function here. CivetWeb calls it later on a worker thread.
     *
     * The final NULL becomes the handler's user_data argument. This is
     * separate from the server-wide user data argument to mg_start.
     */
    mg_set_request_handler(server, "/health", health_handler, NULL);

    // Eventually provider auth implementations will take the place of
    // health_handler completely. No need for client to request server health
    mg_set_request_handler(server, "/auth/steam", steam_login_handler, NULL);

    puts("Auth server listening on http://127.0.0.1:8080");
    puts("Press Enter to stop.");
    /*
     * getchar reads one character from stdin. In a normal terminal, input
     * arrives after Enter, so this keeps main waiting while the workers
     * answer requests. (void) just discards the value returned by getchar.
     *
     * It returns int so it can represent a character or EOF (end of input).
     * If stdin is already closed, this can finish without anyone typing.
     * This is a temporary interactive stop mechanism. A deployed service
     * should handle shutdown signals instead of waiting for keyboard input.
     */
    (void)getchar();

    // Stop accepting requests and wait for the server's threads to finish.
    // This frees the server context, so don't use server afterward.
    mg_stop(server);
    // Clean up the process-wide library state after the server has stopped.
    mg_exit_library();
    return EXIT_SUCCESS;
}
