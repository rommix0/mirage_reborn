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

## Rendering a note to a WAV

```
mirage_cli run --disk "../Factory Library/IMG/SND-A04.img" --note 48:100@40:3 --seconds 45 --wav a04.wav
```

The OS needs about 35-37 s of emulated time after power-on to load the disk's
first sound in the current floppy-controller model; a note fired earlier (for
example `--note 60:100@3:2`) plays nothing. Start notes at 40 s or later (this
example's note at 40-43 s peaks near 0.29). Boot timing is in
`docs/hardware/boot-rom.md` section 13.5.
