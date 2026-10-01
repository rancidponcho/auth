/*
 * Steam
 *
 * Provides the client authentication implementation for Steamworks.
 *
 */

#include "auth.h"

#include <steam/steam_api.h>
#include <stdio.h>

bool auth_init(void)
{
    SteamErrMsg error = {0};
    
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK) {
        fprintf(stderr, "Steam init failed: %s\n", error);
        return false;
    }

    return true;
}

void auth_shutdown(void)
{
    SteamAPI_Shutdown();
}

