#include "auth.h"
#include "provider.h"

#include <curl/curl.h>

static CURLM *http = NULL;

bool auth_init(void)
{
    // selected provider
    if (!provider_init()) {
        return false;
    }

    // libcurl 
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        provider_shutdown();
        return false;
    }

    // curl multi handle: allows for advancing an HTTP request each
    // frame while returning control to the app
    http = curl_multi_init();
    if (!http) {
        curl_global_cleanup();
        provider_shutdown();
        return false;
    }

    return true;
}

void auth_shutdown(void)
{
    if (http) {
        curl_multi_cleanup(http);
        http = NULL;
    }

    curl_global_cleanup();
    provider_shutdown();
}

bool auth_request(void)
{
    return provider_request();
}

void auth_update(void)
{
    provider_update();
}

AuthStatus auth_status(void)
{
    return provider_status();
}
