// Steamworks ticket provider.

#include "provider.h"

#include <stdio.h>
#include <string.h>
#include <chrono>
#include <steam/steam_api.h>

static AuthStatus ticket_status = AUTH_IDLE;

static std::chrono::steady_clock::time_point ticket_deadline;

static HAuthTicket ticket_handle = k_HAuthTicketInvalid;
static unsigned char ticket_data[GetTicketForWebApiResponse_t::k_nCubTicketMaxLength];
static int ticket_size = 0;

// Cancel failed requests so they can be retried, otherwise copy ticket
static void on_auth_ticket(const GetTicketForWebApiResponse_t *response)
{
    if (ticket_handle == k_HAuthTicketInvalid || response->m_hAuthTicket != ticket_handle) {
        return;
    }

    if (response->m_eResult != k_EResultOK) {
        fprintf(stderr, "Steam ticket failed: %d\n", (int)response->m_eResult);
        ticket_status = AUTH_FAILED;
    } else if (response->m_cubTicket <= 0 || response->m_cubTicket > (int)sizeof(ticket_data)) {
        fprintf(stderr, "Stream ticket size is invalid\n");
        ticket_status = AUTH_FAILED;
    } else { // SUCCESS
        ticket_size = response->m_cubTicket;
        memcpy(ticket_data, response->m_rgubTicket, (size_t)ticket_size);
        printf("Steam ticket received (%d bytes)\n", ticket_size);
        ticket_status = AUTH_READY;
        return;
    }
    // FAIL
    SteamUser()->CancelAuthTicket(ticket_handle);
    ticket_handle = k_HAuthTicketInvalid;
    ticket_size = 0;
}

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

// Request a Steam Web API ticket.
bool provider_request(void)
{
    ISteamUser *user = SteamUser();

    if (ticket_handle != k_HAuthTicketInvalid) {
        return false;
    }

    if (!user) {
        ticket_status = AUTH_FAILED;
        return false;
    }

    ticket_size = 0;
    ticket_handle = user->GetAuthTicketForWebApi("auth");

    if (ticket_handle == k_HAuthTicketInvalid) {
        ticket_status = AUTH_FAILED;
        return false;
    }

    ticket_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    
    ticket_status = AUTH_PENDING;
    return true;
}

// Process Steam callbacks and update ticket status
void provider_update(void)
{
    // timeout check
    if (ticket_status == AUTH_PENDING && std::chrono::steady_clock::now() >= ticket_deadline) {
        ISteamUser* user = SteamUser();

        if (user && ticket_handle != k_HAuthTicketInvalid) {
            user->CancelAuthTicket(ticket_handle);
        }

        ticket_handle = k_HAuthTicketInvalid;
        ticket_size = 0;
        ticket_status = AUTH_FAILED;

        fprintf(stderr, "Steam ticket request timed out\n");
    }
    
    // Get Steam connection handle and run periodic processing
    HSteamPipe pipe = SteamAPI_GetHSteamPipe();
    SteamAPI_ManualDispatch_RunFrame(pipe);

    // Get one notification at a time
    CallbackMsg_t callback;
    while (SteamAPI_ManualDispatch_GetNextCallback(pipe, &callback)) {
        if (callback.m_iCallback == GetTicketForWebApiResponse_t::k_iCallback &&    // check notification type
            callback.m_cubParam == (int)sizeof(GetTicketForWebApiResponse_t)) {     // ensure data size is as expected
            on_auth_ticket((const GetTicketForWebApiResponse_t *)callback.m_pubParam); // copy ticket data
        }

        SteamAPI_ManualDispatch_FreeLastCallback(pipe); // release before retrieving another
    }
}

AuthStatus provider_status(void)
{
    return ticket_status;
}

// Borrow the ticket bytes; returns NULL until ready.
const unsigned char *provider_ticket_data(int *out_size)
{
    if (!out_size) {
        return NULL;
    }

    *out_size = 0;

    if (ticket_status != AUTH_READY) {
        return NULL;
    }

    *out_size = ticket_size;
    return ticket_data;
}

void provider_shutdown(void)
{
    if (ticket_handle != k_HAuthTicketInvalid) {
        SteamUser()->CancelAuthTicket(ticket_handle);
        ticket_handle = k_HAuthTicketInvalid;
    }

    ticket_size = 0;

    ticket_status = AUTH_IDLE;

    SteamAPI_Shutdown();
}
