/*
 * Client authentication
 *
 * Include this header to request login data from a platform. Steam 
 * provides a ticket, other providers supply their own login data.
 * The auth server checks that data before logging the user in.
 *
 * CMake selects one provider implementation for the build. That source 
 * file includes this header and defines the functions declared below.
 * Provider SDK headers, callbacks, and internal state stay in that file
 *
 * Requests take some time to finish. Successfully starting a request
 * doesn't mean the login data has arrived yet, or that our server has 
 * accepted it.
 */

#ifndef AUTH_CLIENT_H
#define AUTH_CLIENT_H

#include <stdbool.h>

typedef enum AuthStatus {
    AUTH_IDLE,
    AUTH_PENDING,
    AUTH_READY,
    AUTH_FAILED
} AuthStatus;

#ifdef __cplusplus
extern "C" {
#endif

bool auth_init(void);
void auth_shutdown(void);

// returns true on started request
bool auth_request(void);
void auth_update(void);

AuthStatus auth_status(void);

const unsigned char *auth_ticket_data(int *size);

#ifdef __cplusplus
}
#endif

#endif // AUTH_CLIENT_H
