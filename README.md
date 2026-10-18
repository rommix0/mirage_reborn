# Mirage emulator engine

Headless C++20 emulator engine for the Ensoniq Mirage. See `docs/` for the
design spec and implementation plans.

Run from the folder that contains `emulator/`:

```
cmake -S emulator -B emulator/build -G "Visual Studio 16 2019" -A x64
cmake --build emulator/build --config Debug
ctest --test-dir emulator/build -C Debug --output-on-failure
```

## MIRAGE_ASSET_ROOT

The ROM (`Ensoniq Mirage EPROM Firmware/mirage.bin`) and disk images are
user-owned and are never copied into this repo. Tests locate them through the
CMake cache variable `MIRAGE_ASSET_ROOT`, which defaults to the parent of
`emulator/`. Override it with `-DMIRAGE_ASSET_ROOT=<path>`. Tests that need an
asset skip when it is missing.
