cmake_minimum_required(VERSION 3.22.1...4.3)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()

project(auth LANGUAGES C)

add_executable(auth main.c)
set_target_properties(auth PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(auth PRIVATE /W4)
else()
    target_compile_options(auth PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS auth RUNTIME DESTINATION bin COMPONENT auth)
