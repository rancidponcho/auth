#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civetweb.h>

// Checks HTTP availability, not Steam or database health.
static int health_handler(struct mg_connection *connection, void *user_data)
{
    (void)user_data;

    // Borrowed request info; valid for this callback.
    const struct mg_request_info *request_info = mg_get_request_info(connection);
   
    // Route matching can include longer paths; handle only the exact path.
    if (strcmp(request_info->local_uri, "/health") != 0) {
        return 0;
    }

    if (strcmp(request_info->request_method, "GET") != 0) {
        mg_printf(connection,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        // The response is already sent; return its status for CivetWeb's log.
        return 405;
    }

    // Content-Length counts the three body bytes in "ok\n".
    mg_printf(connection,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n\r\n"
        "ok\n");
    return 200;
}

static int steam_login_handler(struct mg_connection *connection, void *user_data)
{
    (void)user_data;

    const struct mg_request_info* request_info = mg_get_request_info(connection);

    if (strcmp(request_info->local_uri, "/auth/steam") != 0) {
        return 0;
    }

    if (strcmp(request_info->request_method, "POST") != 0) {
        mg_printf(connection,
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
        mg_send_http_error(connection, 411, "Content-Length is required");
        return 411;
    }

    if (content_length == 0) {
        mg_send_http_error(connection, 400, "Ticket body is empty");
        return 400;
    }

    if (content_length > (long long)sizeof(ticket_data)) {
        mg_send_http_error(connection, 413, "Ticket body is too large");
        return 413;
    }

    size_t expected_bytes = (size_t)content_length;
    size_t received_bytes = 0;
    // A read can return only part of the body.
    while (received_bytes < expected_bytes) {
        int bytes_read = mg_read(connection, ticket_data + received_bytes, expected_bytes - received_bytes);

        if (bytes_read <= 0) {
            mg_send_http_error(connection, 400, "Ticket body is incomplete");
            return 400;
        }

        received_bytes += (size_t)bytes_read;
    }

    printf("Steam ticket body received (%zu bytes)\n", received_bytes);

    // Receipt alone isn't authentication; Steam verification is still missing.
    mg_printf(connection,
        "HTTP/1.1 501 Not Implemented\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    return 501;
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

    // No optional features
    (void)mg_init_library(0);

    struct mg_context *server = mg_start(NULL, NULL, options);
    if(!server) {
        fprintf(stderr, "Could not start auth server\n");
        mg_exit_library();
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
    return EXIT_SUCCESS;
}
