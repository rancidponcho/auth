add_library(auth_client STATIC steam.cpp)

target_include_directories(auth_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_features(auth_client PRIVATE cxx_std_11)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Steamworks.cmake")
target_link_libraries(auth_client PRIVATE Steamworks::SteamAPI)

if(MSVC)
    target_compile_options(auth_client PRIVATE /W4)
else()
    target_compile_options(auth_client PRIVATE -Wall -Wextra -Wpedantic)
endif()
