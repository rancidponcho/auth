# Auth

Requires a C11 compiler, CMake 3.22.1 or newer, and Ninja.

From the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/auth
```

On Windows, run `build/auth.exe`. For a release build, use
`-DCMAKE_BUILD_TYPE=Release`.
