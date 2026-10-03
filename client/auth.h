/*
 * Client authentication
 *
 * Include this header to request login data from a platform. Steam 
 * provides a ticket atm. The auth server checks that data before 
 * logging the user in.
 *
 * CMake selects one provider implementation for the build. That source 
 * file defines the functions and includes provider.h, of which's functions
 * are called from here. Provider SDK  headers, callbacks, and internal 
 * state stay in that provider's respective file.
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
bool auth_request(void);
void auth_update(void);

AuthStatus auth_status(void);

#ifdef __cplusplus
}
#endif

#endif // AUTH_CLIENT_H
