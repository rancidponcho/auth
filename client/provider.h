#ifndef AUTH_PROVIDER_H
#define AUTH_PROVIDER_H

#include "auth.h"

#ifdef __cplusplus
extern "C" {
#endif

bool provider_init(void);
bool provider_request(void);
void provider_update(void);
AuthStatus provider_status(void);
const unsigned char *provider_ticket_data(int *out_size);
void provider_cancel(void);
void provider_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // AUTH_PROVIDER_H
