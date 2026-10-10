#include "database.h"

#include <stdio.h>

PGconn* database_connect(void)
{
    PGconn* connection = PQconnectdb("connect_timeout=5");
    if (!connection) {
        fprintf(stderr, "Could not allocate database connection\n");
        return NULL;
    }

    if (PQstatus(connection) != CONNECTION_OK) {
        fprintf(stderr, "Database connection failed: %s", PQerrorMessage(connection));
        PQfinish(connection);
        return NULL;
    }

    return connection;
}
