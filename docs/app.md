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
argument exits with 2, a failed write with 1.

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
- **Open, Eject and Exit** (also the close button) ask "Save changes to X before
  ...?" when the disk has unsaved changes: Save, Don't Save or Cancel. A save
  must succeed before the action goes on.
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
- No audio input and no WAV import yet (Plan 3).
