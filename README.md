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
mirage_cli run --disk "../Factory Library/IMG/SND-A04.img" --note 48:100@16:3 --seconds 20 --wav a04.wav
```

The OS needs about 14.6 s of emulated time after power-on to load the disk's
first sound (the controller model reads about 30 sectors/s from a track; the
lower sound's 133 sectors take about 7.4 s); a note fired earlier (for example
`--note 60:100@3:2`) plays nothing. Start notes at 16 s or later (this
example's note at 16-19 s peaks near 0.28; the file's overall peak of 0.29 is
the boot calibration tones). Boot timing is in `docs/hardware/boot-rom.md`
sections 13.5 and 13.6.

## The standalone app (`app/`)

A JUCE 8 app (replica front panel, menus, audio and MIDI devices, disk open and
save) around the same engine. The full guide, with the menus, shortcuts,
threading model, decisions and the acceptance checklist, is in
[`docs/app.md`](docs/app.md).

Building it fetches JUCE (git, tag 8.0.4) on the first configure, and the first
build takes several minutes. Skip it with `-DMIRAGE_BUILD_APP=OFF`.

```
cmake --build emulator/build --config Release --target MirageApp --parallel
emulator/build/app/MirageApp_artefacts/Release/Mirage.exe
```

**Build Release for live play**; the Debug build is unoptimised and may glitch.
On start it loads the boot ROM from the saved path, then from
`MIRAGE_ASSET_ROOT/Ensoniq Mirage EPROM Firmware/mirage.bin`, and otherwise
asks for it (a 4096-byte file; a ROM with a different SHA-1 is allowed but
warned about). Settings are kept in `%APPDATA%\MirageEmulator\MirageEmulator.settings`.
`Mirage.exe --panel-png out.png` renders the window content to a PNG and exits.

Licence: the app uses JUCE 8 (AGPLv3, or JUCE's free Starter licence). The
engine and `mirage_host` contain no JUCE code, so the licence choice for any
other front end (for example a VST3) stays open.

Panel lettering: the "Mirage" wordmark is drawn from outlines of the Anybody
font and the "DIGITAL SAMPLING KEYBOARD" legend uses the Michroma font, both
under the SIL Open Font License 1.1 (licences in `app/resources/fonts/`). They
stand in for the panel's commercial typefaces and are embedded in the exe.
`tools/fonts/make_wordmark.py` regenerates the wordmark.
