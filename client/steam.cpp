/*
 * Steam
 *
 * Provides the client authentication implementation for Steamworks.
 *
 */

#include "auth.h"

#include <steam/steam_api.h>
#include <stdio.h>
#include <string.h>

static HAuthTicket ticket = k_HAuthTicketInvalid;
static unsigned char ticket_data[GetTicketForWebApiResponse_t::k_nCubTicketMaxLength];
static int ticket_size = 0;

bool auth_init(void)
{
    SteamErrMsg error = {0};
    
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK) {
        fprintf(stderr, "Steam init failed: %s\n", error);
        return false;
    }

    SteamAPI_ManualDispatch_Init();
    return true;
}

void auth_shutdown(void)
{
    if (ticket != k_HAuthTicketInvalid) {
        SteamUser()->CancelAuthTicket(ticket);
        ticket = k_HAuthTicketInvalid;
    }

    ticket_size = 0;

    SteamAPI_Shutdown();
}

bool auth_request(void)
{
    ISteamUser *user = SteamUser();

    if (!user || ticket != k_HAuthTicketInvalid) {
        return false;
    }

    ticket_size = 0;
    ticket = user->GetAuthTicketForWebApi("auth");

    return ticket != k_HAuthTicketInvalid;
}

/*
 * Ignore responses for other requests. Successful response with a valid
 * length gets copied. Failed response gets handle canceled and cleared,
 * allowing for another request.
 */
static void on_auth_ticket(const GetTicketForWebApiResponse_t *response)
{
    if (ticket == k_HAuthTicketInvalid || response->m_hAuthTicket != ticket) {
        return;
    }

    if (response->m_eResult != k_EResultOK) {
        fprintf(stderr, "Steam ticket failed: %d\n", (int)response->m_eResult);
    } else if (response->m_cubTicket <= 0 || response->m_cubTicket > (int)sizeof(ticket_data)) {
        fprintf(stderr, "Stream ticket size is invalid\n");
    } else {
        ticket_size = response->m_cubTicket;
        memcpy(ticket_data, response->m_rgubTicket, (size_t)ticket_size);
        printf("Steam ticket received (%d bytes)\n", ticket_size);
        return;
    }

    SteamUser()->CancelAuthTicket(ticket);
    ticket = k_HAuthTicketInvalid;
    ticket_size = 0;
}

void auth_update(void)
{
    HSteamPipe pipe = SteamAPI_GetHSteamPipe();
    SteamAPI_ManualDispatch_RunFrame(pipe);

    CallbackMsg_t callback;
    while (SteamAPI_ManualDispatch_GetNextCallback(pipe, &callback)) {
        if (callback.m_iCallback == GetTicketForWebApiResponse_t::k_iCallback && 
            callback.m_cubParam == (int)sizeof(GetTicketForWebApiResponse_t)) {
            on_auth_ticket((const GetTicketForWebApiResponse_t *)callback.m_pubParam);
        }

        SteamAPI_ManualDispatch_FreeLastCallback(pipe);
    }
}



