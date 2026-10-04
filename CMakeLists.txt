cmake_minimum_required(VERSION 3.22.1...4.3)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()

project(auth LANGUAGES C)

option(AUTH_BUILD_CLIENT "Build the auth client library" OFF)
option(AUTH_BUILD_SERVER "Build the auth server" ${PROJECT_IS_TOP_LEVEL})

if(AUTH_BUILD_CLIENT)
    enable_language(CXX)
    add_subdirectory(client)
endif()

if(AUTH_BUILD_SERVER)
    include(FetchContent)

    set(CIVETWEB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CIVETWEB_ENABLE_SERVER_EXECUTABLE OFF CACHE BOOL "" FORCE)
    set(CIVETWEB_ENABLE_CXX OFF CACHE BOOL "" FORCE)
    set(CIVETWEB_ENABLE_SSL OFF CACHE BOOL "" FORCE)
    set(CIVETWEB_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
    set(CIVETWEB_SERVE_NO_FILES ON CACHE BOOL "" FORCE)

    FetchContent_Declare(civetweb GIT_REPOSITORY https://github.com/civetweb/civetweb.git
                                  GIT_TAG d7ba35bbb649209c66e582d5a0244ba988a15159 # v1.16
    )

    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(civetweb)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    
    add_executable(auth main.c)
    target_link_libraries(auth PRIVATE civetweb-c-library)

    set_target_properties(auth PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(auth PRIVATE /W4)
    else()
        target_compile_options(auth PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    install(TARGETS auth RUNTIME DESTINATION bin COMPONENT auth)
endif()
