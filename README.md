# AM32 CLI Configurator

This project is being refactored from the old Qt Widgets configurator into a pure command-line tool.

The current CLI writes a 48-byte AM32 EEPROM settings/config binary to one ESC through Betaflight/MSP four-way passthrough.

## Usage

```powershell
am32-cli --port COM4 --index 0 --config .\am32_v3_config.bin
```

To force motor direction to reverse before writing:

```powershell
am32-cli --port COM4 --index 0 --config .\am32_v3_config.bin --reverse
```

Arguments:

- `--port <port>`: Windows serial port, for example `COM4`.
- `--index <0-3>`: ESC/motor index behind the flight-controller passthrough.
- `--config <path>`: AM32 EEPROM config binary. The first 48 bytes are written.
- `--reverse`: sets EEPROM byte `17` to `1` before writing. Without this flag, byte `17` is left as it exists in the config file.
- `--help`: prints usage and exit codes.

All logs, warnings, and errors are written to standard output.

## Exit codes

| Code | Meaning |
| ---: | --- |
| `0` | Success |
| `1` | Invalid command-line arguments |
| `2` | Config file cannot be read or is invalid |
| `3` | Serial port open/configuration failed |
| `4` | Serial read/write failure |
| `5` | MSP/four-way passthrough setup failed |
| `6` | ESC connect failed |
| `7` | EEPROM write failed |
| `8` | Four-way protocol/CRC error |

## Build

Requirements:

- Windows
- CMake 3.20 or newer
- A C++17 compiler, such as Visual Studio Build Tools or MinGW

Configure and build:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

With Visual Studio generators, the executable is usually created at:

```text
build\Release\am32-cli.exe
```

With single-config generators such as Ninja or MinGW Makefiles, it is usually created at:

```text
build\am32-cli.exe
```

## Implementation notes

- The new CLI entrypoint is `main.cpp`.
- The CMake build intentionally excludes the old Qt Widgets files: `widget.cpp`, `widget.h`, and `widget.ui`.
- Serial I/O uses the Windows API directly, so the built executable does not require Qt runtime DLLs.
- The CLI opens the port at `115200 8N1`, sends MSP commands to start four-way passthrough, connects to the requested ESC index, detects the EEPROM address from the ESC MCU id, and writes the 48-byte config.
- The old Qt GUI source files are still present as migration reference material but are not part of the CLI build.
- The final MSP flight-controller reset is sent as a cleanup command. No response is required because the port may reset immediately after the command is accepted.

## Troubleshooting

If MSP passthrough starts successfully but ESC connect fails with a decoded bad ACK, for example:

```text
ESC connect returned bad ACK: marker=0x2E, command=connect, length=4, ACK 0x0F
```

then the flight controller is responding and four-way passthrough is active, but the interface rejected the selected ESC. Check:

- The ESC is powered.
- The selected `--index` matches the motor output. Try `0`, `1`, `2`, and `3`.
- Betaflight/Cleanflight is not armed and motor outputs are idle.
- The ESC signal wire is connected to that output.

It is acceptable for the MSP setup commands to show `RX <none>` if the later four-way connect succeeds. Some flight-controller firmware enters passthrough without returning a visible MSP response.
