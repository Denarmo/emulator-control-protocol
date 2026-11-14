# Emulator Control Protocol

An open protocol that lets a game frontend start a game in a standalone emulator and control it
while it runs: pause, the frontend's menu over the game, save states, speed, video and audio
settings, screenshots and performance, on Windows, Linux, macOS and Android.

- [SPEC.md](SPEC.md): the protocol, version 1.
- [GUIDE.md](GUIDE.md): how to add ECP to an emulator.
- `schema/`: JSON Schemas (draft 2020-12) for the descriptor, every message and every result, and
  [the system ids](schema/systems.md).
- `sdk/c/`: the engine-side library in portable C11 (`ecp.h`, `ecp.c`).
- `sdk/android/`: the Android library (Kotlin, minSdk 26): `EcpEngine` for engines,
  `EcpFrontend`/`EcpSession` for frontends.
- `sdk/cpp-frontend/`: a header-only C++17 frontend client.
- `examples/test-engine/`: a fake emulator for testing frontends, for desktop and Android.
- `conformance/ecp-probe.py`: a test frontend that checks an engine (Python 3, standard library).

## Building

```sh
# test engine (desktop): any C11 compiler with CMake
cmake -S examples/test-engine -B build/test-engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test-engine

# check it
python conformance/ecp-probe.py --transport both --menu-timeout 5 -- build/test-engine/test-engine --menu-after 1

# C++ frontend example
c++ -std=c++17 -O2 sdk/cpp-frontend/example.cpp -o build/cpp-example   # add -lws2_32 on MinGW, -pthread on Linux

# Android library and test engine APK (examples/test-engine/android/build/outputs/apk/release)
cd sdk/android && ./gradlew assembleRelease testDebugUnitTest
```

MIT licensed. First used by [EmuDEN](https://github.com/Denarmo/emuden); not tied to it.
