## Auth

Requires a C11 compiler, CMake 3.22.1 or newer, and Ninja.

From the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/auth
```

On Windows, run `build/auth.exe`. For a release build, use
`-DCMAKE_BUILD_TYPE=Release`.

## Client

'client/auth.h' provides client interface.

For Steam, add 'client/steam.cpp' to your app's build. It
requires a C++11 compiler and the Steamworks SDK. Add the 
SDK's 'public/' directory to the include path and link its
Steamworks library.

The server build does not compile the client files.

After 'auth_init()' succeeds, call 'auth_request()' to start 
a ticket request. Call 'auth_update()' each frame to process
replies, and 'auth_shutdown()' when exiting.

The Steam implementation currently prints the request result
Server verification not implemented.
