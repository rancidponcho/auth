# Auth

Client authentication library and server.

The client requests Steam tickets on Linux, macOS, and Windows through a C
interface. Apple and Google providers aren't implemented. Ticket upload,
server verification, accounts, and sessions aren't implemented either.

Requires CMake 3.22.1 or newer and a C11 compiler. The Steam client also needs a
C++11 compiler and the Steamworks SDK. Examples below use Ninja.

## Server

From this repository's root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/auth
```

On Windows, run `build/auth.exe`. Use `-DCMAKE_BUILD_TYPE=Release` for a release build.

The server is a placeholder. It prints `bloobis knorp` and exits with an
error. It doesn't need the Steam SDK or build the client library.

## Client

Put this repository at `external/auth` in your project. In your project's
root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22.1)
project(MyGame LANGUAGES C CXX)

set(AUTH_BUILD_CLIENT ON)
set(AUTH_BUILD_SERVER OFF)
add_subdirectory(external/auth)

add_executable(my_game main.c)
target_link_libraries(my_game PRIVATE auth_client)
auth_stage_runtime(my_game)
```

`auth_client` provides the headers and links its own dependencies. The game
can stay in C; the Steam implementation uses C++.

Call `auth_stage_runtime` in the CMake directory that defines your executable,
after any macOS bundle settings. It copies Steam's runtime library beside the
executable, or inside the app bundle.

Configure your project with the SDK path. Bash example:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSTEAMWORKS_SDK_ROOT="/path/to/steamworks/sdk" \
  -DSTEAM_DEVELOPMENT=ON -DSTEAM_APP_ID=480
cmake --build build
```

The SDK directory must contain `public/steam/steam_api.h`. It doesn't need
to be on PATH.

Development mode writes `steam_appid.txt` into your project's build directory.
Open Steam and sign in, then launch your app with that directory as the working
directory. AppID 480 is Valve's Spacewar sample. Set `STEAM_DEVELOPMENT=OFF`
for distribution.

## Usage

Include `auth.h`. Call `auth_init()` and check that it succeeds, then call
`auth_request()`. A true return means a new ticket request started.

Call `auth_update()` each frame to process callbacks. Read `auth_status()`
for the current state:

| State | Meaning |
| --- | --- |
| `AUTH_IDLE` | No request started. |
| `AUTH_PENDING` | Waiting for a ticket. |
| `AUTH_READY` | Ticket received. |
| `AUTH_FAILED` | Ticket request failed. |

`AUTH_READY` only means the ticket arrived. Server login isn't implemented.
Call `auth_shutdown()` when done, if initialization succeeded.

## Build options

| Option | Default |
| --- | --- |
| `AUTH_BUILD_CLIENT` | OFF |
| `AUTH_BUILD_SERVER` | ON when built on its own; OFF when included by another project. |

The client and server can be built independently.
