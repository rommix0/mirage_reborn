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

## Saving the disk from a headless run

`mirage_cli run ... --save-disk out.img` writes the disk in the drive (a
450,560-byte raw image) at the end of the run, for example after the OS has
saved a sound from `--key` presses:

```
mirage_cli run --disk "../Factory Library/IMG/SND-A01.img" --key Param@16 --key K1@17 --key K1@18 --key K1@19 --key Enter@20 --seconds 30 --save-disk edited.img
```

It exits 1 if there is no disk, a track is unformatted or the file cannot be
written. `--disk` also reads a Giebler `.edm` file (detected by content); the
disk file formats are in `docs/hardware/fdc.md`.

## Importing a WAV into wave RAM

```
mirage_cli run --disk "../Factory Library/IMG/SND-A01.img" --import "sine.wav@15,rate=29412,half=lower,page=0" --note 48:100@16:2 --seconds 19 --wav out.wav
```

`--import "FILE@T[,rate=R][,start=S][,end=E][,half=lower|upper][,page=P][,normalize=0|1]"`
converts the file and writes it into wave RAM at T seconds of emulated time. The drive must be idle (SND-A01 loads
until about 14.6 s); the OS plays the imported bytes through whatever wavesample start,
end and tuning its sound already has. See `docs/app.md` ("WAV import") for the halves
and the pitch hint.

## Sampling from the audio input jack

```
mirage_cli run --disk "../Factory Library/IMG/SND-A01.img" --audio-in "sine.wav@15,gain=0.08,loop=1" --key SampleLower@16 --key Enter@19 --note 52:100@23:0.5 --seconds 24 --wav sampled.wav
```

`--audio-in "FILE@T[,gain=G][,loop=0|1]"` plays a WAV file into the emulated input
jack from T seconds of emulated time, at its own rate (the CLI resamples it to `--rate`),
times gain G (default 1) and repeating with `loop=1`. Repeat it to change the input
later; a later entry replaces the earlier from its T. Without `--wav` the run is still
rendered (the output is discarded) so the input is placed exactly. At the end it prints
`audio-in: FILE N frames played` per entry. The example boots SND-A01, feeds a 440 Hz
sine, and the OS samples it into the lower half (Sample Lower, then Enter once the
level-detect display is up, 2.55 s later); the note at 23 s plays it back (key 52 is
about 440 Hz). The input is for tests and listening: the app has no WAV player on the
jack. The mic input (parameter 75 off, the default) is 11 times hotter than line and
clips a full-scale sine, so the example above uses `gain=0.08`. How the OS samples is in
`docs/hardware/filters.md` section 3.3.

## The standalone app (`app/`)

A JUCE 8 app (replica front panel, menus, audio and MIDI devices, disk open and
save, New Blank Disk, Import WAV) around the same engine. It opens raw
450,560-byte images and Giebler `.edm` files (by content) and saves in the
format a disk was read in. The full guide, with the menus, shortcuts, threading
model, decisions, the acceptance 1-6 checklists and the known limitations, is in
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
`Mirage.exe --panel-png out.png` renders the front panel to a PNG and exits;
`Mirage.exe --import-dialog-png in.wav out.png` does the same for the Import WAV
dialog.

Licence: the app uses JUCE 8 (AGPLv3, or JUCE's free Starter licence). The
engine and `mirage_host` contain no JUCE code, so the licence choice for any
other front end (for example a VST3) stays open.

Panel lettering: the "Mirage" wordmark is traced from a photo of a DSK panel
(`tools/fonts/trace_wordmark.py`) and the "DIGITAL SAMPLING KEYBOARD" legend
uses the Michroma font (SIL Open Font License 1.1, licence in
`app/resources/fonts/`), a free stand-in for the panel's Microgramma-style face.
Both are embedded in the exe.
