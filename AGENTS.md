# AGENTS.md

C++23 command-line tool for talking to a DJI 4G cellular module over USB (vendor `0x2ca3`, product `0x4006`, see `src/at.cppm:23`). Built with C++ modules, vcpkg manifest mode, and CMake >= 4.0.

## Build

Requires `VCPKG_ROOT` (toolchain comes from `$VCPKG_ROOT` per `CMakePresets.json`). On macOS use Homebrew LLVM, not AppleClang — C++23 modules need it (repo is configured with `/opt/homebrew/opt/llvm/bin/clang++`).

```sh
cmake --preset default          # configures into build/ (Ninja)
cmake --build build             # or --parallel
ctest --test-dir build --output-on-failure
```

CI (`/.github/workflows/ci.yml`) is the canonical flow: `cmake --preset default -DCMAKE_BUILD_TYPE=Debug`, `cmake --build build --parallel`, `ctest --test-dir build --output-on-failure`, with `-Wall -Wextra -Wpedantic -Werror` set globally. Locally `-Werror` is only on the test target.

## Tests

Boost.UT, single binary `build/DJIC_tests` with one test suite per area; CTest runs it 4x passing a suite name (`"AT parsing"`, `"PDU codec"`, `"SMS logic"`, `"Modem and Asterisk state"`). Focused run: `ctest --test-dir build -R DJIC_at_tests`. Suites live in `tests/*.cppm` and register via `register_*_tests()` in `tests/main.cpp`.

## C++ module layout (file name != module name)

| File | Exported module |
|------|-----------------|
| `src/at.cppm` | `usb` (USB + AT channel, DJI VID/PID) |
| `src/pdu.cppm` | `smscodec` (PDU encoding) |
| `src/modem.cppm` | `modem` |
| `src/sms.cppm` | `sms` |
| `src/esim.cppm` | `esim` |
| `src/asterisk.cppm` | `asterisk` (Boost.Beast WS client) |

Searching for module `usb` or `smscodec` must land on `at.cppm` / `pdu.cppm`. New modules use `FILE_SET CXX_MODULES` and must be added to **both** `target_sources` blocks in `CMakeLists.txt` (the `DJIC` binary and the `DJIC_tests` target).

## Gotchas

- The main app (`src/main.cpp`) needs real DJI hardware + libusb (`PkgConfig::LIBUSB`) — you cannot run it on a normal machine. Iterate on tests instead.
- `clice.toml` enables `clang-tidy` for the `clice` C++ LSP; run tidy through clice rather than standalone.
- No README exists; treat `CMakeLists.txt` and CI as the source of truth.
