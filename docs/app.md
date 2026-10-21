# The standalone app

`Mirage.exe` is a JUCE 8 window around the same engine the CLI uses: a replica
of the Mirage front panel, a disk drive you load `.img` files into, MIDI in and
out, and an audio output. The machine is emulated at realistic speed, so a
disk load takes as long as it did on the hardware (no turbo, owner decision of
2026-10-18).

## Build and run

From the folder that contains `emulator/`:

```
cmake -S emulator -B emulator/build -G "Visual Studio 16 2019" -A x64
cmake --build emulator/build --config Release --target MirageApp --parallel
emulator/build/app/MirageApp_artefacts/Release/Mirage.exe
```

- The first configure fetches JUCE 8.0.4 from git and the first build takes
  several minutes. `-DMIRAGE_BUILD_APP=OFF` skips the app and the fetch.
- **Use Release for live play.** The Debug build is unoptimised, so it uses far
  more CPU for the same audio and is more likely to glitch. The Debug exe is
  under `.../Debug/Mirage.exe`.
- From Git Bash use `--parallel`, not `-- /m`.

### Boot ROM

On start the app looks for the 4096-byte boot ROM in this order:

1. the path saved in the settings (from the last successful start),
2. `<MIRAGE_ASSET_ROOT>/Ensoniq Mirage EPROM Firmware/mirage.bin` (the default
   asset root is the parent of `emulator/`),
3. a file chooser.

A file that is not exactly 4096 bytes is refused. A ROM whose SHA-1 is not
`ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d` is accepted, but a warning shows the
two hashes: the machine may not behave like the real one. The ROM and disk
images are yours and are never copied into this repository.

### Command line

`Mirage.exe --panel-png out.png` renders the front panel at 1200 px wide with a
fixed sample state ("2." "1" on the display, a disk in, drive LED on) and
exits with code 0. It needs neither an audio device nor the ROM. A missing file
argument exits with 2, a failed write with 1. The ctest `app_panel_png_smoke`
(registered when the app is built) runs it and checks for exit code 0 and a
file that starts with the PNG signature.

## The window

The window is the replica panel (display, keypad, drive bezel with the LED and
the disk's file name, volume slider) above a one-line status strip.
It resizes and letterboxes to keep the panel's aspect ratio.

- **Mouse:** a left click on a panel button presses that key for as long as the
  button is held (right and middle clicks do nothing).
- **Volume slider:** a host-side output gain, 0 to 1, default 1. It is applied
  after the emulated output (a linear ramp across each block, so no zipper
  noise) and is saved in the settings. It does not change anything inside the
  machine.
- **Title:** `Mirage — SND-A01.img`, `... (modified)` when the disk has unsaved
  changes, `Mirage — No disk` with an empty drive.
- **Status strip:** left, the disk's file name (`(modified)` when it has unsaved
  writes) or "No disk"; middle, the audio device, its rate and the audio CPU
  load; right, a warning only when something is wrong: `illegal ops: N` (the
  CPU met an undefined opcode) and `MIDI dropped: N` (MIDI bytes lost, see
  below). The strip refreshes at 10 Hz and reads the audio thread's published
  status, so "(modified)" can lag a block or two behind the first write.

## Menus and shortcuts

| Menu | Item | Shortcut |
|---|---|---|
| File | Open Disk Image... | Ctrl+O |
| File | New Blank Disk (an unformatted disk named "Untitled (blank)") | Ctrl+N |
| File | Save Disk Image | Ctrl+S |
| File | Save Disk Image As... | Ctrl+Shift+S |
| File | Eject Disk | Ctrl+E |
| File | Exit | |
| Machine | Reset (power-cycles the Mirage; the disk and unsaved changes stay) | Ctrl+R |
| MIDI | MIDI input: None / one device (radio list) | |
| MIDI | MIDI output: None / one device (radio list) | |
| MIDI | Panic | |
| Audio | Audio Settings... (output device, rate, buffer size) | |

- **Open** accepts any file of exactly 450,560 bytes whatever its extension
  (never `.hfe`). The Windows file dialog shows one filter, `*.img;*`.
- **Save** writes the disk to the file it came from; **Save As** asks for a new
  file (`.img` by default). A disk with no file name goes straight to Save As.
- **New Blank Disk** inserts a disk with every track unformatted and no file
  (the title and strip say "Untitled (blank)", the panel label "Untitled"). The
  emulated OS has to format it: boot the `EnsoniqOS/fmt2` disk, swap the blank
  disk in and run the format (acceptance-5 checklist below). Save on an untitled
  disk goes to Save As.
- **A disk that is not fully formatted cannot be saved.** The save is refused
  with "This disk is not fully formatted (N of 80 tracks). Format it with the OS
  before saving." (while fmt2 is still formatting, or after a failed format).
  The disk stays modified and no file, not even a `.tmp`, is written.
- **Open, New Blank Disk, Eject and Exit** (also the close button) ask "Save
  changes to X before ...?" when the disk has unsaved changes (X is "the new
  disk" for an untitled one): Save, Don't Save or Cancel. A save must succeed
  before the action goes on.
- **Save, Save As, Eject** are greyed out with no disk; no disk action runs
  while a save is in flight.
- **Panic** discards all MIDI input not yet played and sends All Notes Off
  (CC 123) on all 16 channels. If the OS ignores CC 123 that is faithful
  behaviour.
- **MIDI input:** one device at a time. The chosen devices are saved when you
  pick them, and a saved device that is missing at launch stays saved.

### Panel keyboard shortcuts

| Key | Panel key |
|---|---|
| `0`-`9` | keypad 0-9 |
| Up arrow | On / Yes |
| Down arrow | Off / No |
| Enter | Enter |
| Esc | Cancel |

Each physical key press is one panel press, released when the key comes up
(auto-repeat is ignored). The shortcuts are ignored while a dialog or file
chooser is open, Ctrl and Alt combinations go to the menus, and every held key
is released when the window loses focus or a dialog opens.

## Settings

JUCE `ApplicationProperties` with application and folder name `MirageEmulator`,
so the file is `%APPDATA%\MirageEmulator\MirageEmulator.settings` (XML). It holds
the ROM path, the last disk folder, the audio device state, the saved MIDI
input and output, the volume and the window position and size. Delete it to
reset the app.

## Threading model

The audio thread owns the `Machine`. Everything else talks to it through
`mirage::RealtimeHost` (`host/include/mirage/realtime_host.h`), which is
JUCE-free:

| Thread | Does | Talks to the audio thread through |
|---|---|---|
| Audio | `RealtimeHost::process()` per device block: drains the queues, runs the machine, publishes status | n/a |
| UI (message) | Menus, panel clicks, disk open/save/eject (`DiskSession`) | the UI queue: `postKey`, `postInsert`, `postEject`, `postReset`, `postPanic`, `postCopyDisk`, `postMarkSaved` |
| MIDI input (one device) | `postMidi` with a timestamp | the MIDI queue |
| "MIDI out" | `popMidiOut`, parses the stream, sends to the chosen output device | the MIDI-out queue |

Queues (all lock-free single-producer, single-consumer):

| Queue | Capacity | Notes |
|---|---|---|
| MIDI in | 1024 commands of up to 12 bytes | `process()` takes at most 256 per block. A longer message (SysEx) is several commands with the same timestamp, all or nothing. |
| MIDI out | 8192 bytes | Each byte carries the machine's cycle stamp; a full queue drops the byte. |
| UI commands | 256 | A full queue makes the `post...` call return false and leave its argument untouched. |
| Returned disks | 64 | Up to 4 more wait inside the host if this fills. |

- After `prepare()` the audio thread never allocates, frees, locks or does
  I/O. Disks cross as `unique_ptr`s that change hands only on the audio thread,
  and **every disk leaving the machine goes back to the UI thread to be
  freed there** (an ejected disk, an insert that never went in, a save copy).
  An allocation-counting test enforces this.
- **MIDI timing:** an event is placed in the block by its timestamp
  (`frames - round((now - t) * rate)`, clamped and non-decreasing). This adds up
  to one block of latency and no jitter.
- **Dropped MIDI** (`MIDI dropped` in the strip) counts bytes rejected by a
  full queue, overflowing the machine's input ring or the MIDI-out queue, and
  MIDI discarded while the audio rate is invalid (a device change in progress;
  stale notes are not replayed). Panic's flush is deliberate and not counted.
- **Disk swap:** inserting while a disk is in ejects the old disk at once and
  inserts the new one 0.5 s of emulated time later, so the OS sees the
  disk-present line drop.
- **Minimum key hold:** a panel key stays down at least 100 ms of emulated time
  (the OS scans one keypad column per main-loop pass). The release is applied
  at the end of a block segment, so with large audio blocks the hold can run
  longer than 100 ms.
- **Saving:** the audio thread copies the disk into a buffer the UI supplied
  (`postCopyDisk`); the UI writes the file atomically (temp file, then rename)
  and sends `postMarkSaved` with the copy's write count. The dirty flag clears
  only if the OS wrote nothing in between. If the copy has not come back
  after 2 s (no audio device, a failed device change) the save fails with a
  message and can be retried.

## Decisions made for the app

| Decision | Choice | Why / cost if wrong |
|---|---|---|
| Output headroom | Keep `kOutputGain` 0.5 and add a soft clip in `render()`: linear up to ±0.75, then `0.75 + 0.25·tanh((|x|−0.75)/0.25)`, never past ±1.0 | One voice peaks at about 0.26, so normal play is untouched. Chords can't hard-clip the device. CLI WAVs match the app. Easy to retune by ear (`machine.h` comment). |
| Boot calibration tones (filter self-oscillation, about 1 s, right after the ROM hands off to the OS, about 4-5 s after inserting the disk) | **Not muted** | This is what the emulated hardware emits. Muting would need a boot-phase detector, which is an OS-specific heuristic. Flag it at the acceptance listen. |
| MIDI input timing | Events placed in the block by timestamp, MidiMessageCollector-style, clamped and non-decreasing | Adds up to one block of latency but no jitter. |
| Minimum key hold | A panel key stays down at least 100 ms of emulated time | A fast mouse click could fall between the OS's keypad scans. |
| Disk swap | Eject now; insert 0.5 s of emulated time later | The OS sees PB6 drop. |
| Saving | Audio thread copies the disk; the UI writes atomically, then sends "mark saved" with the write count | Concurrent OS writes are never lost from the dirty flag. |
| Atomic `DiskImage::saveFile` | Moved here from Plan 3 because Save UI ships now | n/a |
| MIDI panic | Flush the input queue and send CC 123 on all 16 channels | If an OS ignores CC 123, that's faithful behaviour. |
| JUCE licence | JUCE 8 (AGPLv3 or JUCE's free Starter licence). The engine and host stay JUCE-free, so VST3 and licence choices stay open. | Noted in the README. |
| Faithful emulation | The host only presses panel keys, queues MIDI bytes and inserts or ejects disks | No OS-specific code paths, no OS patching. |

## Acceptance-1 checklist (by ear, on your machine)

**Status: passed.** The owner ran this checklist and accepted it by ear
(2026-10-19).

Boot timing on the emulated clock (`docs/hardware/boot-rom.md` §13.6): the ROM
hands the OS off at about 4.1 s, the OS main loop starts at about 5.2 s, the
lower sound finishes loading at about 12.6 s and the display settles by about
13-15 s.

1. Start `Mirage.exe` (the Release build). The display blinks "nd" after about
   0.5 s (no disk).
2. *MIDI → input:* your keyboard. *Audio → Audio Settings:* your output.
3. *File → Open* `Factory Library/IMG/SND-A01.img`. The drive LED lights and
   the display shows the boot. You hear the calibration tones for about 1 s
   (expected and not muted), starting about 4-5 s after you insert the disk,
   right after the ROM hands off to the OS.
4. Wait for the display to settle on "2." "1" (about 15 s). The OS has
   auto-loaded the lower sound.
5. Play. Then load another sound from the panel (*Load Upper/Lower*, a digit,
   Enter, as the OS 3.2 manual describes) and play it.

Re-run it after changes to the audio path or the app.

## Acceptance-2 checklist (edit, save in the OS, Save As, reopen)

**Status: not yet run by the owner.** `tests/test_acceptance.cpp` runs the same
panel keys headlessly (the test `acceptance 2: an edited parameter survives OS
save, Save As and reopen`) and passes. The display strings below are what that
run shows; the keys are from the OS 3.2 manual (*Selecting and changing a
parameter*, *Saving a sound on a formatted diskette*), and each key is clicked on
the panel (or typed with its panel keyboard shortcut).

1. *File → Open Disk Image...* `Factory Library/IMG/SND-A01.img` and wait for the
   display to settle on "2." "1" (about 15 s).
2. Show the value of parameter 36 (Filter Fc) of the lower sound: **Param**, **3**,
   **6** (the display shows "3." "6"), **Value**. The display shows "2" "8." (value
   28; the right-hand dot means a value is shown).
3. Press **Up** three times. The display shows "3" "1." (value 31).
4. Save the lower sound to place 1 on the disk with the OS: **Param**, **1**, **1**
   (display "S" "L", save lower), **1** (display "S" "1"), **Enter**. The display
   goes blank and the drive LED lights for a few seconds; when the LED goes out
   the display shows "1." "1" (parameter 11). The disk now has unsaved changes in
   the app (the title and status strip show "(modified)"): the OS wrote to the
   emulated floppy, not to your file.
5. *File → Save Disk Image As...* and give it a new name, for example
   `SND-A01-edited.img`. (The original `SND-A01.img` is untouched.)
6. *File → Open Disk Image...* the new file. The OS boots from it as in
   acceptance 1 and settles on "2." "1".
7. Press **Param**, **3**, **6**, **Value**. The display shows "3" "1." (value 31):
   the edit came back from the file. Opening `SND-A01.img` again and repeating
   this step shows "2" "8.".

Re-run it after changes to the disk path (FDC write, `DiskSession`) or the file
dialogs.

## Acceptance-5 checklist (format a blank disk with fmt2)

**Status: not yet run by the owner.** `tests/test_acceptance.cpp` runs the same
panel keys headlessly (`acceptance 5: fmt2 formats a blank disk that then boots`
and `acceptance 5: a sound saved to the formatted disk loads back`) and passes.
Formatting is emulated at real speed (no turbo): on the emulated clock fmt2 takes
**about 106 s from Enter to "Fc"** (all 80 tracks about 33 s, then OS 3.2 is
written and every track is read back), so expect a bit under two minutes of
waiting with the drive LED on.

1. *File → Open Disk Image...* `EnsoniqOS/fmt2`. Wait for the display to show
   "F" "d" (fmt2 is asking for a blank disk).
2. *File → New Blank Disk* (Ctrl+N). The title says "Untitled (blank)". The
   Mirage notices the disk change. (If fmt2 had unsaved changes the app would
   ask first; it has none.)
3. Press **1**, then **Enter** (format; OS 3.2 goes onto the disk). The drive
   LED lights and the head steps through the tracks.
4. While it runs, try *File → Save Disk Image As...* and pick a name: the save is
   refused with "This disk is not fully formatted (N of 80 tracks). Format it
   with the OS before saving." and no file appears. Retry after step 5.
5. Wait for the display to show "F" "c" (the display's lower-case c) after about
   106 s.
6. *File → Save Disk Image As...* `Formatted.img`. It saves (450,560 bytes).
7. *File → Open Disk Image...* `Formatted.img`. fmt2 wrote OS 3.2 onto it, so it
   boots like acceptance 1 (about 15 s) and settles on "2." "1".
8. Save a sound to it: boot `Factory Library/IMG/SND-A01.img`, wait for "2." "1",
   *File → Open Disk Image...* `Formatted.img` (the Mirage sees the new disk),
   press **Param**, **1**, **1**, **1**, **Enter** (save lower sound to place 1;
   wait for the drive LED to go out), then *File → Save Disk Image As...*
   `Formatted-with-sound.img`.
9. Boot `SND-A01.img` again, open `Formatted-with-sound.img`, press **Load
   Lower**, **1**, **Enter** and play: the sound plays. (The test also changes
   the lower sound's Filter Fc before saving and sees the new value return.)

Re-run it after changes to the FDC (Write Track, Read Address) or to
`DiskSession`.

## WAV import: wave RAM halves and the pitch hint

The import writes converted bytes straight into the 128 KB sample RAM
(`Machine::writeWaveRam`, wave RAM only; the engine API is shared by the dialog and
`mirage_cli --import`). Task 6 adds the dialog; the headless route is below.

**Halves.** The dialog's *Lower* half is wave RAM `$00000-$0FFFF` and *Upper* is
`$10000-$1FFFF`. Measured under OS 3.2 (SND-A01 booted, then `Load Lower` and `Load Upper`
from the panel over a marker): **Load Lower rewrites exactly `$00000-$0FFFF` and Load
Upper `$10000-$1FFFF`**, and the keys 36-71 play wave tables below `$10000` while keys
72-96 play from `$10000` up (DOC size register bit 6 selects the bank). So the lower
keyboard half is the lower wave RAM half, with the factory split. SND-A01's boot loads
both halves (sectors 26-91 and 91-155), so neither half is empty after boot; the
import overwrites what it covers.

**CLI.** `mirage_cli run --disk D --import "FILE@T[,rate=R][,start=S][,end=E][,half=lower|upper][,page=P][,normalize=0|1]"`
(repeatable) converts FILE and writes it at T seconds of emulated time. The file name cannot
contain a comma. Defaults: rate 29412 Hz (10101-33333 allowed), start 0, end the end of the file,
lower half, page 0 (a 256-byte page, 0-255, decimal), normalize 1. It prints
`import: FILE N bytes at $XXXXX, last page P[, truncated]`, where P is decimal; the OS
shows page numbers in hexadecimal (a wavesample end of 40 decimal is `28`). If the drive
motor is on at T (the OS is loading) nothing is written and it prints
`import: FILE refused, the drive is busy`; the run goes on. A file that cannot be read,
or settings the import refuses, print the message and exit 1 before the run starts.
SND-A01 is busy until about 14.6 s, so import at 15 s or later:

```
mirage_cli run --disk "../Factory Library/IMG/SND-A01.img" --import "sine.wav@15,rate=29412" --note 48:100@16:2 --seconds 19 --wav out.wav
```

**Pitch hint** (Task 6 shows it as "plays at original pitch around C3"):

    key ~= k0 + 12 * log2(rate / r0),   k0 = 47.3,   r0 = 29412 Hz

`wav_import.h` has `kPitchHintKey` (k0), `kPitchHintRate` (r0) and
`originalPitchKey(rate)`, the rounded MIDI note number (C4 = 60, so 47 = B2, 48 = C3).
Rates outside the keyboard (keys 36-96, C2-C7) give a key off its ends; at the limits
10101 Hz gives 29 (F1) and 33333 Hz gives 49 (C#3).

*How it was measured* (`acceptance 3: an imported sine plays at a pitch set by the key`,
OS 3.2, SND-A01 as booted, velocity 100, wheels at rest): a 2 s 440 Hz sine is imported at
rate r into the lower half, page 0. For each of keys 41-56 (one wavesample, from page
`$48` on, tuned a semitone per key) the note is played and its fundamental `f` measured
from the zero crossings of the output between 31 ms and 125 ms. The sine plays at its
original 440 Hz when `f = 440`, so each key says the original-pitch key is
`key - 12 log2(f / 440)`; the mean over the 16 keys is k0, and the keys agree to 0.07
semitone. At r = 29412 Hz this gives **47.33** (kept as 47.3); at r = 22050 Hz it gives
42.34, against 42.31 from the formula. Playback is at the fixed DOC scan rate
(8 MHz / (8 x 34) = 29,411.76 Hz), so one byte per scan plays a rate-29412 sample at
its recorded speed, and the output frequency is `440 * FC / 511` (FC is the oscillator
frequency register).

*What the number depends on.* It is that one wavesample's tuning. SND-A01's nine
wavesamples are each tuned so that the sample plays at its recorded speed (FC about
`$1FF`) at their own root key: about 36 (keys 36-37), 38.5 (38-40), 47.3 (41-56), 57
(57), 58.9-60 (58-60), 62 (61-63), 68 (64-71), 74 (72-76), 79 (77-81) and 93 (82-96).
The musician's manual says a single-wavesample sound set up by the OS plays at key #17
(MIDI 52, E3) on the lower half and key #46 (MIDI 81, A5) on the upper half, so "around
B2/E3" is the lower-half range to expect; relative tuning (parameters 67, 68) moves it.
The hint is a starting point, not a promise for every sound.

## Known limitations

- Acceptance has to be judged by ear by the owner; automated checks cannot
  hear the output.
- The file dialogs, MIDI devices, audio device selection, menu shortcuts and
  panel keyboard shortcuts are not exercised by automated tests (the queues,
  the MIDI parser, `DiskSession`, the panel layout and the soft clip are).
- The boot calibration tones are deliberately not muted (see the decisions).
- The status strip's "(modified)" can lag by a block or two.
- A SysEx message longer than about 4 KB (for example a wavesample dump from a
  librarian) is truncated: the emulated MIDI input ring holds 4096 bytes and
  JUCE delivers a whole SysEx message at once. `RealtimeHost::postMidi` also
  rejects any single message over 12 KB (1024 queue commands of 12 bytes).
  Plan 3 adds a paced feeder.
- Windows only so far; the Windows file chooser shows a single filter,
  `*.img;*`.
- No audio input. WAV import has the engine call and `mirage_cli --import`; the dialog is Plan 3 Task 6.
