/*
 * Steam
 *
 * Provides the client authentication implementation for Steamworks.
 *
 */

#include "provider.h"

#include <steam/steam_api.h>
#include <stdio.h>
#include <string.h>

static AuthStatus status = AUTH_IDLE;
static HAuthTicket ticket = k_HAuthTicketInvalid;
static unsigned char ticket_data[GetTicketForWebApiResponse_t::k_nCubTicketMaxLength];
static int ticket_size = 0;


bool provider_init(void)
{
    SteamErrMsg error = {0};
    
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK) {
        fprintf(stderr, "Steam init failed: %s\n", error);
        return false;
    }

    SteamAPI_ManualDispatch_Init();
    return true;
}

void provider_shutdown(void)
{
    if (ticket != k_HAuthTicketInvalid) {
        SteamUser()->CancelAuthTicket(ticket);
        ticket = k_HAuthTicketInvalid;
    }

    ticket_size = 0;

    status = AUTH_IDLE;

    SteamAPI_Shutdown();
}

/*
 * Request a ticket from Steam.
 */
bool provider_request(void)
{
    ISteamUser *user = SteamUser();

    if (ticket != k_HAuthTicketInvalid) {
        return false;
    }

    if (!user) {
        status = AUTH_FAILED;
        return false;
    }

    ticket_size = 0;
    ticket = user->GetAuthTicketForWebApi("auth");

    if (ticket == k_HAuthTicketInvalid) {
        status = AUTH_FAILED;
        return false;
    }
    
    status = AUTH_PENDING;
    return true;
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
        status = AUTH_FAILED;
    } else if (response->m_cubTicket <= 0 || response->m_cubTicket > (int)sizeof(ticket_data)) {
        fprintf(stderr, "Stream ticket size is invalid\n");
        status = AUTH_FAILED;
    } else {
        ticket_size = response->m_cubTicket;
        memcpy(ticket_data, response->m_rgubTicket, (size_t)ticket_size);
        printf("Steam ticket received (%d bytes)\n", ticket_size);
        status = AUTH_READY;
        return;
    }

    SteamUser()->CancelAuthTicket(ticket);
    ticket = k_HAuthTicketInvalid;
    ticket_size = 0;
}

// Process Steam callbacks and update ticket status
void provider_update(void)
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

AuthStatus provider_status(void)
{
    return status;
}

/*
 * Provides a read-only pointer to the ticket data
 */
const unsigned char *provider_ticket_data(int *size)
{
    if (!size) {
        return NULL;
    }

    *size = 0;

    if (status != AUTH_READY) {
        return NULL;
    }

    *size = ticket_size;
    return ticket_data;
}
