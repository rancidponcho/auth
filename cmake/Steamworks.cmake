include_guard(GLOBAL)

set(STEAM_APP_ID "480" CACHE STRING "Steam AppID")
option(STEAM_DEVELOPMENT "Write steam_appid.txt for local runs" OFF)

set(STEAMWORKS_SDK_ROOT "" CACHE PATH "Steamworks SDK folder containing public/steam")
if(NOT EXISTS "${STEAMWORKS_SDK_ROOT}/public/steam/steam_api.h")
    message(FATAL_ERROR "Set STEAMWORKS_SDK_ROOT to your Steamworks sdk folder.")
endif()

set(steam_bin "${STEAMWORKS_SDK_ROOT}/redistributable_bin")
set(steam_implib "")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" steam_arch)
if(CMAKE_C_COMPILER_ARCHITECTURE_ID)
    string(TOLOWER "${CMAKE_C_COMPILER_ARCHITECTURE_ID}" steam_arch)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT steam_arch MATCHES "^(x86_64|amd64|x64|x86|i[3-6]86)$")
        message(FATAL_ERROR "Steamworks SDK 1.65 has no Windows library for ${steam_arch}.")
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(steam_library "${steam_bin}/win64/steam_api64.dll")
        set(steam_implib "${steam_bin}/win64/steam_api64.lib")
    else()
        set(steam_library "${steam_bin}/steam_api.dll")
        set(steam_implib "${steam_bin}/steam_api.lib")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(steam_arch MATCHES "^(x86_64|amd64|x64|x86|i[3-6]86)$")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(steam_library "${steam_bin}/linux64/libsteam_api.so")
        else()
            set(steam_library "${steam_bin}/linux32/libsteam_api.so")
        endif()
    elseif(steam_arch MATCHES "^(aarch64|arm64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(steam_library "${steam_bin}/linuxarm64/libsteam_api.so")
    else()
        message(FATAL_ERROR "Steamworks SDK 1.65 has no Linux library for ${steam_arch}.")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(steam_library "${steam_bin}/osx/libsteam_api.dylib")
else()
    message(FATAL_ERROR "Steamworks is not configured for ${CMAKE_SYSTEM_NAME}.")
endif()

set(steam_files "${steam_library}")
if(steam_implib)
    list(APPEND steam_files "${steam_implib}")
endif()
foreach(steam_file IN LISTS steam_files)
    if(NOT EXISTS "${steam_file}")
        message(FATAL_ERROR "Missing Steamworks library: ${steam_file}")
    endif()
endforeach()

# A name for Valve's existing library and the headers needed to use it.
add_library(Steamworks::SteamAPI SHARED IMPORTED GLOBAL)
set_target_properties(Steamworks::SteamAPI PROPERTIES
    IMPORTED_LOCATION "${steam_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${STEAMWORKS_SDK_ROOT}/public"
)
if(steam_implib)
    set_target_properties(Steamworks::SteamAPI PROPERTIES
        IMPORTED_IMPLIB "${steam_implib}"
    )
endif()

function(auth_stage_runtime target)
    set(steam_appid_file "${PROJECT_BINARY_DIR}/steam_appid.txt")

    if(STEAM_DEVELOPMENT)
        file(WRITE "${steam_appid_file}" "${STEAM_APP_ID}\n")
    else()
        file(REMOVE "${steam_appid_file}")
    endif()

    # On macOS TARGET_FILE_DIR is inside the app's Contents/MacOS folder.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Steamworks::SteamAPI>"
            "$<TARGET_FILE_DIR:${target}>"
        VERBATIM
    )

    # Linux searches beside the executable in both build and install outputs.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN"
        )
    endif()

    # Installing a macOS app already copies the library inside its bundle.
    get_target_property(steam_bundle ${target} MACOSX_BUNDLE)
    if(NOT steam_bundle)
        install(FILES "$<TARGET_FILE:Steamworks::SteamAPI>"
            DESTINATION bin COMPONENT client)
    endif()
endfunction()
