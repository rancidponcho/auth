/*
 * auth_status() tracks login attempt
 * AUTH_READY is reserved for a confirmed login from server
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
bool auth_request(void);
void auth_update(void);
AuthStatus auth_status(void);
void auth_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // AUTH_CLIENT_H
