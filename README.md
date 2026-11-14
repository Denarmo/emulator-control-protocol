# Emulator Control Protocol

An open protocol that lets a game frontend start a game in a standalone emulator and control it
while it runs: pause, the frontend's menu over the game, save states, speed, video and audio
settings, screenshots and performance, on Windows, Linux, macOS and Android.

- [SPEC.md](SPEC.md): the protocol, version 1.
- `schema/`: JSON schemas for the descriptor and the messages.
- `sdk/`: libraries for engines (C/C++ and Android) and frontends.
- `conformance/`: `ecp-probe`, a test frontend that checks an engine.
- [GUIDE.md](GUIDE.md): how to add ECP to an emulator.

MIT licensed. First used by [EmuDEN](https://github.com/Denarmo/emuden); not tied to it.
