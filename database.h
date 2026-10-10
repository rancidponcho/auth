#ifndef AUTH_DATABASE_H
#define AUTH_DATABASE_H

#include <libpq-fe.h>

// Close with PQfinish
PGconn* database_connect(void);

#endif
