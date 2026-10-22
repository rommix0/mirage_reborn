# The standalone app

`Mirage.exe` is a JUCE 8 window around the same engine the CLI uses: a replica
of the Mirage front panel, a disk drive you load `.img` files into, MIDI in and
out, an audio output and an audio input you can sample from. The machine is emulated at realistic speed, so a
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

`Mirage.exe --import-dialog-png in.wav out.png` renders the *Import WAV* dialog for
`in.wav` with its default settings and exits: 0 on success, 1 if the WAV cannot
be read or the PNG written, 2 with an argument missing. It needs neither an
audio device nor the ROM and is there to review the dialog's layout.

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
- **Input slider and meter** (right end of the status strip, not on the panel):
  see *Audio input and sampling* below.
- **Title:** `Mirage — SND-A01.img`, `... (modified)` when the disk has unsaved
  changes, `Mirage — No disk` with an empty drive.
- **Status strip:** left, the disk's file name (`(modified)` when it has unsaved
  writes) or "No disk"; middle, the audio device, its rate and the audio CPU
  load; right, the outcome of the last WAV import for 15 s, then a warning only
  when something is wrong: `illegal ops: N` (the CPU met an undefined opcode)
  and `MIDI dropped: N` (MIDI bytes lost, see below). The strip refreshes at
  10 Hz and reads the audio thread's published status, so "(modified)" can lag
  a block or two behind the first write. The three texts share the strip left
  of the Input controls; a long one pushes the middle text aside.

## Menus and shortcuts

| Menu | Item | Shortcut |
|---|---|---|
| File | Open Disk Image... (a raw image or a Giebler `.edm`) | Ctrl+O |
| File | New Blank Disk (an unformatted disk named "Untitled (blank)") | Ctrl+N |
| File | Save Disk Image (in the format the file was read in) | Ctrl+S |
| File | Save Disk Image As... (a name ending in `.edm` saves Giebler `.edm`, any other a raw image) | Ctrl+Shift+S |
| File | Eject Disk | Ctrl+E |
| File | Import WAV... | Ctrl+I |
| File | Exit | |
| Machine | Reset (power-cycles the Mirage; the disk and unsaved changes stay) | Ctrl+R |
| MIDI | MIDI input: None / one device (radio list) | |
| MIDI | MIDI output: None / one device (radio list) | |
| MIDI | Panic | |
| Audio | Audio Settings... (input and output device, active channels, rate, buffer size) | |

- **Open** accepts any file of exactly 450,560 bytes whatever its extension
  (a raw image, including the 450,560-byte `.edm` files) and any Giebler `.edm`
  file (it starts with "Mirage Disk"), detected by content, never `.hfe`
  (`docs/hardware/fdc.md`, "Disk file formats"). Anything else is refused with
  "*name*: not a Mirage disk image (expected 450,560 bytes or a Giebler .edm
  file)". The Windows file dialog shows one filter, `*.img;*.edm;*`. A Giebler
  file leaves out the sectors that are all zero; they open as zeros.
- **Open does not reset the machine.** It swaps the disk under the running OS, as
  on the hardware (spec 6.5, "Disk changes and reset"): nothing is read from the
  new disk until the OS is booted again. The boot ROM loads an OS only after
  power-on or *Machine → Reset* (Ctrl+R), and when a disk goes into an empty
  drive at the "nd" prompt. Reset keeps the disk and its unsaved changes.
- **Save** writes the disk to the file it came from, in the format it was read
  in: a Giebler `.edm` stays Giebler, a raw `.edm` stays raw. **Save As** asks
  for a new file and writes a Giebler `.edm` for a name ending in `.edm` (any
  case), a raw image for any other name; later Saves keep that format. A disk
  with no file name goes straight to Save As.
- **Save As suggests the disk's own file name**, with `.edm` as the extension
  only when the disk is held in Giebler `.edm` format and `.img` otherwise. So
  Save As on a raw `.edm` such as `mp1.edm` suggests `mp1.img`, and accepting it
  writes a raw image; typing a name that ends in `.edm` writes **Giebler**
  format. Plain Save keeps a raw `.edm` raw. Save As asks before overwriting an
  existing file.
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
- **Import WAV** is greyed out while the drive LED is on (the OS may be loading
  a sound into wave RAM) and while no audio device runs. See *The Import WAV
  dialog* below.
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

## The Import WAV dialog

*File → Import WAV...* (Ctrl+I) asks for a `.wav` file (it starts in the folder
of the last import, or the disk folder), reads it, and opens the import dialog,
a window of its own so the replica panel stays as it is. A file the reader
refuses (not PCM 8/16/24/32-bit or 32-bit float, not mono or stereo) shows its
message instead. The dialog is modal: the panel's keyboard shortcuts are off
and held keys are let go while it is open.

- **Waveform:** the file's mono mix (min/max per pixel column) with a green
  start marker and an orange end marker. Click or drag to move the nearer
  marker. The selection is light blue; any part of it that does not fit before
  the end of the half is drawn in orange.
- **Start / End:** the selection in seconds (also typed in the boxes).
- **Rate:** 10,101-33,333 Hz, the Mirage's own sampling range; *Default* sets
  29,412 Hz. The bytes are converted to play at this rate.
- **Into:** *Lower sound ($00000)* (wave RAM `$00000-$0FFFF`) or *Upper sound
  ($10000)* (`$10000-$1FFFF`), the halves OS 3.2 loads the lower and upper
  sounds into.
- **Start page:** 0-255 (256-byte pages from the start of the half), shown in
  decimal and in hex as the OS shows page numbers.
- **Normalise** (on by default): scale the selection so its peak uses all 8 bits.
- **Live text:** the selected length in seconds, the bytes and the percentage of
  the half they fill, the wave RAM range and pages written with the last page
  (decimal and hex), and the pitch hint "Plays at original pitch around B2
  (with the loaded sound's tuning)" (see below; it depends on the sound in
  memory). In orange: "Too long for the half: only the first N s fits ..."
  when the selection is cut off, any problem that refuses the import (Import is
  greyed while there is one), and a note while the drive LED is on or no audio
  device runs (Import is greyed then too).
- **Import** converts the selection on the UI thread (mono mix, low-pass and
  resample, optional normalise, unsigned 8-bit), posts it to the audio thread
  and closes the dialog. When the write comes back the status strip shows
  `Imported N bytes at $XXXXX (pages A-B, hex AA-BB)` (pages within the half),
  or `Not imported: the drive was busy` if the drive started before the write
  was applied (nothing is written then). *Cancel* or Esc closes it.

The OS is not told about the import: its wavesample start, end, loop and
tuning stay as they were, as on a real Mirage after sampling. There is
deliberately no loop setter in the dialog: loop points are OS parameters, and the
app never writes OS parameters (spec §3). Set them on the panel as on a real
Mirage, with the page numbers the dialog shows (in hex, as the OS shows them):
OS 3.2 parameters **60** Wavesample Start, **61** Wavesample End, **62** Loop
Start, **63** Loop End, **64** Loop End Fine Adjust and **65** Loop Switch
(Advanced Sampler's Guide, "Looping"). Select one with **Param** and the two
digits, then **Value** and **Up**/**Down**. The imported
sample is kept only once you save the sound with the OS (and then the disk
image with *Save*); a reset loses it.

## Audio input and sampling

The audio input feeds the emulated Mirage's input jack, and the OS samples from
it with its own routine, as on the hardware (design:
`docs/specs/2026-10-22-audio-input-sampling-design.md`; the analog model and what
the OS does: `docs/hardware/filters.md` §3.3). The app never records anything
itself and writes nothing into wave RAM: it only feeds the jack. *Import WAV*
stays as it is and is a separate way to get a sound in.

- **Choosing the input:** *Audio → Audio Settings...* now lists an input device
  and its channels next to the output ones. Two steps: pick the **input device**
  in the second device list (it starts at none), then tick the input channel your
  source is plugged into under "Active input channels" (no channel is ticked
  when you pick a device). Channels are listed one by one and at most two inputs can be
  active, because the Mirage samples **mono**: with two ticked the app feeds the
  jack their average (so a source on only one of them arrives at half level;
  tick only that one). No input channel ticked, no input device, or an input
  that cannot be opened means nothing is plugged into the jack, and the strip's
  meter says "no input". On Windows the input device is listed separately from
  the output device (for the "Windows Audio" type); an audio interface's own
  driver offers both on one device.
- **First start:** with no saved audio settings the app opens the default
  output and **no input**: nothing is plugged into the jack, no microphone is
  opened (so no Windows microphone prompt or indicator) and the meter says "no
  input", until you choose an input in Audio Settings (spec 6.5). The choice is
  saved with the other audio settings and comes back at the next start.
  A saved input that cannot be opened at start (the interface is unplugged, the
  microphone permission is refused) falls back to the default output with no
  input, and **the saved choice is kept**: it is written back at quit unchanged,
  until you open *Audio Settings...* (from then on the devices as they stand are
  yours). On Windows a refused microphone permission (*Settings → Privacy →
  Microphone*) can also show up as an input that delivers silence; the jack then
  reads as a silent source, which changes nothing in the emulation. If an
  input device is unplugged while the app runs, JUCE reopens the default
  devices; the jack is unplugged for as long as no input channel is active.
  With no output channel active at all (an input-only device) the machine still
  runs; there is just nothing to hear.
- **Input slider** (status strip, 0 to 4, default 1, double-click returns to 1,
  saved in the settings): a gain applied to the input *before* the jack. It
  stands in for the mixer or preamp knob the Advanced Sampler's Guide recommends
  (the Mirage has no input level control of its own). The slider is not linear:
  1 sits in the middle.
- **Meter** (next to the slider): the peak level of the input *after* the Input
  gain, on a dB scale from -60 dB (empty) to 0 dB (full), green, yellow above
  -6 dB, red at 1.0 or more. It is refreshed 10 times a second from the last
  audio block's peak, with a short decay, so it is a guide to the level, not an
  exact peak meter. **1.0 on the meter is the full scale of the line path of the
  emulated jack.** It is the level the Mirage's input sees, and it is the same
  with the Mirage on mic or line.
- **Levels** (what to aim for; the numbers come from the engine's model, see
  `docs/hardware/filters.md` §3.3):
  - *Mic* (OS parameter 75 off, **the default**): the jack has about 11 times
    (+21 dB) the gain of line, plus a compressor. A sine of about 0.08 on the
    meter fills the sample without clipping; it clips from about 0.1 to 0.15, and
    the OS needs about 0.05 to start recording. So for a normal audio-interface
    signal in mic mode, **turn the Input slider down** (a source peaking around
    half of full scale wants about 0.1) until the loud notes peak at about 0.05
    to 0.08 on the meter (-26 to -22 dB, about a third of the way along the
    bar), with the OS's level-detect display showing the middle bar.
  - *Line* (parameter 75 on): no preamp gain and no compressor, and the
    emulated input filter (parameter 74 at its default) passes only about 0.54 of
    it. A source has to reach about **0.7 on the meter** to cross the OS's
    default threshold (parameter 76 = 24); aim for 0.7 to 0.9 and keep it out of
    the red.
  - Too quiet and the OS never starts recording ("n" "S"); too loud and the
    sample clips (the line path is a hard clip, as the guide warns).
- **How the OS samples** (OS 3.2; MASOS is the same with parameter 93 at 0;
  SoundProcess has no sampling):
  1. **Sample Lower** (the black *Lower* key under SAMPLE; *Upper* is the
     black key above it): "S" "L" blinks for about 2.5 s.
  2. **Level detect:** both digits show the same single segment, which follows the
     level: only the **decimal points** = no level; the **bottom bar** = quiet;
     the **middle bar** = above the threshold (parameter 76), what you want on
     the loud parts; the **top bar** = near clipping. (The segments can
     flicker between two when the level sits between them.)
  3. **Enter** starts the wait for a sound: the recording begins at the first
     sample above the threshold. If nothing crosses it within about 1.8 s the
     display blinks "n" "S", the OS gives up and wave RAM is untouched (the guide
     says about 6 s; the OS's own loop is 1.8 s at the emulated clock).
  4. **Recording** lasts about 2.2 s at the default sample time (parameter 73 =
     34 us, 29.4 kHz) and fills the lower half (65,520 bytes; the last 16 are
     zero). **The emulator's display is blank during the recording** (the real
     machine shows the middle bar; here the OS writes the display once and the
     panel model keeps a segment lit for only 35 ms after a write). Keep the
     sound going for the whole 2.2 s.
  5. The end shows a steady "S" "F" (segments `6D 71`), then you play and edit as
     usual.
  Parameter 74 (input filter), 75 (mic or line), 76 (threshold) and 77
  (multisampling) are the OS's: set them on the panel with **Param**, the two
  digits, **Value**, **Up**/**Down**. The input is not heard while you play (the
  OS keeps the jack off the voices in normal play); it is connected to the filters
  at mic gain while the OS boots, until its main loop starts, so keep the source
  quiet during the boot or set the slider to 0.
- **Not done in the app:** a WAV player never feeds the jack (WAV files stay an
  import feature; the CLI's `--audio-in` exists for tests and reproducible
  listening).

## Settings

JUCE `ApplicationProperties` with application and folder name `MirageEmulator`,
so the file is `%APPDATA%\MirageEmulator\MirageEmulator.settings` (XML). It holds
the ROM path, the last disk folder, the audio device state (devices, active input
and output channels, rate, buffer), the saved MIDI input and output, the volume,
the input gain and the window position and size. Delete it to reset the app.

## Threading model

The audio thread owns the `Machine`. Everything else talks to it through
`mirage::RealtimeHost` (`host/include/mirage/realtime_host.h`), which is
JUCE-free:

| Thread | Does | Talks to the audio thread through |
|---|---|---|
| Audio | `RealtimeHost::process()` per device block: averages the active input channels into one mono buffer, drains the queues, runs the machine with that input, publishes status | n/a |
| UI (message) | Menus, panel clicks, disk open/save/eject (`DiskSession`), WAV import, the Input slider (`setInputGain`, an atomic) and meter (`status().inputPeak`) | the UI queue: `postKey`, `postInsert`, `postEject`, `postReset`, `postPanic`, `postCopyDisk`, `postMarkSaved`; the wave queue: `postWaveWrite` |
| MIDI input (one device) | `MidiInFeeder::push` with a timestamp | the feeder, which calls `postMidi` (the only caller) |
| "MIDI out" | `popMidiOut`, parses the stream, sends to the chosen output device; `MidiInFeeder::pump()` once per loop pass, at least every 16 ms (never on the audio thread) | the MIDI-out queue; the MIDI queue, through the feeder |

Queues (all lock-free single-producer, single-consumer):

| Queue | Capacity | Notes |
|---|---|---|
| MIDI in | 1024 commands of up to 12 bytes | `process()` takes at most 256 per block. A longer message (SysEx) is several commands with the same timestamp, all or nothing. The feeder keeps the traffic far below this (see below). |
| MIDI out | 8192 bytes | Each byte carries the machine's cycle stamp; a full queue drops the byte. |
| UI commands | 256 | A full queue makes the `post...` call return false and leave its argument untouched. |
| Returned disks | 64 | Up to 4 more wait inside the host if this fills. |
| Wave writes | 16 | `postWaveWrite` returns false when full (or for a null write or one past the end of wave RAM). |
| Returned wave writes | 16 | While it is full, `process()` leaves the writes waiting in the wave queue. |

- After `prepare()` the audio thread never allocates, frees, locks or does
  I/O. Disks cross as `unique_ptr`s that change hands only on the audio thread,
  and **every disk leaving the machine goes back to the UI thread to be
  freed there** (an ejected disk, an insert that never went in, a save copy).
  Wave writes (WAV import) travel the same way: a `unique_ptr<WaveWrite>` that
  comes back `Written` or `RefusedDriveBusy` through `takeReturnedWave`, polled
  at 10 Hz with the disk returns. An allocation-counting test enforces this.
- **Audio input** arrives in the same device callback as the output, so it runs
  on the audio thread with the rest of `process()`; there is no input thread
  and no queue. The mono buffer is reserved in `audioDeviceAboutToStart`
  (the device's block size, at least 64) and the callback never allocates: a block
  larger than announced is mixed and rendered in pieces of that size, like the
  output. JUCE passes a null pointer for an input channel that is not active
  (skipped); with none active, `process` gets `nullptr` and the jack is unplugged.
  The host copies the input times the gain into its own buffer, so the engine
  never sees the device's memory.
- **Wave writes** are applied at the start of a block, after the UI queue and
  before rendering, so the CPU and the DOC see the new bytes for the whole
  block. One that arrives while the drive is active (`driveActive()`, the motor
  on) is refused and handed back untouched: the UI greys Import out while the
  drive LED is on, and this closes the gap between that check and the write.
- **MIDI timing:** an event is placed in the block by its timestamp
  (`frames - round((now - t) * rate)`, clamped and non-decreasing). This adds up
  to one block of latency and no jitter.
- **MIDI input feeder** (`MidiInFeeder`, `host/include/mirage/midi_in_feeder.h`):
  all MIDI input goes through it, so a long SysEx (a wavesample dump is
  hundreds of KB, delivered by JUCE in one message) is not truncated by the
  machine's 4096-byte input ring. A mutex serialises `push` (MIDI-input
  thread), `pump` (the "MIDI out" thread, once per loop pass: `wait(1)` can
  sleep about 15.6 ms at the default Windows timer resolution) and `clear`
  (UI thread), so `postMidi` has one producer at a time; none of them runs on
  the audio thread. While nothing is backlogged, a message that fits goes
  straight to `postMidi` with its own timestamp, so playing is unchanged. A
  message that would push the bytes in flight past 2048 (`kMaxInFlight`:
  posted and not yet delivered to the ACIA, from the status fields
  `midiBytesTaken`, `midiBytesFlushed` and `midiQueueDepth`), and anything
  arriving behind a backlog, waits in order; `pump` posts it in chunks as the
  machine consumes it (a byte per 320 us), each chunk with its message's
  timestamp. A slow pump costs no throughput: 2048 bytes in flight is about
  650 ms of wire time. The in-flight count can only overstate, never
  understate: `publishStatus` stores the queue depth before the taken and
  flushed counts (release) and `status()` loads them in the opposite order
  (acquire), so a read never pairs a newer taken count with an older depth.
  The backlog is capped at 1 MiB: a message that does not fit is dropped whole
  and counted. Panic clears the backlog first, so no SysEx follows the All
  Notes Off.
- **Dropped MIDI** (`MIDI dropped` in the strip) counts bytes rejected by a
  full queue, overflowing the machine's input ring or the MIDI-out queue, and
  MIDI discarded while the audio rate is invalid (a device change in progress;
  stale notes are not replayed), plus the bytes of messages the feeder dropped
  for a full 1 MiB backlog (the feeder counts messages in `dropped()` and their
  bytes in `droppedBytes()`; the strip shows bytes, like the rest). Panic's
  flush is deliberate and not counted.
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
| Faithful emulation | The host only presses panel keys, queues MIDI bytes, inserts or ejects disks and writes wave RAM for WAV import | No OS-specific code paths, no OS patching. |
| Wave RAM halves | Import targets `$00000-$0FFFF` or `$10000-$1FFFF` of the 128 KB sample RAM; the dialog names them by the OS 3.2 sound that lives there (measured, see below) | Which half is "lower" is an OS convention; the address is hardware. |
| WAV conversion | Mono mix (average), windowed-sinc low-pass and resample (cutoff 0.45 x the lower of the two rates), peak normalise on by default, round to unsigned 8-bit (`$80` = 0), no dither, `$00` written as `$01` | Keeps the Mirage's 8-bit grit; dither would add hiss. Easy to change by ear. |
| Saving `.edm` | *Save* writes the format the file was read in. *Save As* writes Giebler `.edm` for a `.edm` name and a raw image otherwise, and suggests `.edm` only for a disk held in Giebler format (a raw `.edm` is offered as `.img`, so accepting the suggestion never converts it). The Plan 2 `.edm` save refusal is gone. | Spec 6.6 "Saving an `.edm` writes back in `.edm` format"; a raw `.edm` stays raw with plain Save so other tools keep reading it. Cost if wrong: someone who wants a Giebler copy of a raw `.edm` types the `.edm` name; the format is in the name. |
| Device input to mono | Average of the active input channels (JUCE null pointers skipped); the settings dialog lists channels one by one, at most two inputs and two outputs | The Mirage samples mono. A stereo pair would average a one-channel source with silence and halve it. Cost: the output channels are also listed singly (the app copies the output to every active channel). |
| Input units | 1.0 at the jack (the meter's full scale) is the full scale of the emulated line path | Simple; the Input slider sets the real level. With the default OS settings the line path needs about 0.7 and the mic path clips from about 0.1: the slider's job. |
| Host input gain | 0 to 4, default 1, the strip's Input slider (1 in the middle), saved in the settings, applied before the jack | Stands in for the mixer or preamp knob the guide recommends; the Mirage has none. Default 1 means a line-level interface signal clips the default (mic) path until the slider is lowered; the checklists say so. |
| Mic gain | `Machine::kMicGain` = 11 (+21 dB), the schematic's resistor ratio (the plan said 10) | filters.md §3.3. A guess within a few dB of the real preamp; retune by ear. |
| Input at the first start | None: the default output only, no input channel active until the user picks an input in Audio Settings (device, then channel). A saved state with an input keeps it. | Spec 6.5: no input device chosen means nothing plugged into the jack. No microphone is opened without being asked (no permission prompt, boot calibration untouched). Cost: one more step before the first sampling session. |
| A saved device that cannot be opened | JUCE falls back to the default output with no input; the app keeps the saved audio state and writes it back at quit, until the user opens Audio Settings | The saved interface choice survives a launch with the interface unplugged. Cost: opening the dialog while on the fallback makes the fallback the saved choice. |
| Output with no active channel | The callback renders into a scratch buffer reserved with the input buffer | An input-only device must not freeze the machine. |
| Input meter | Post-gain peak of the last block at 10 Hz, dB scale from -60, short decay, "no input" when no channel is active | An aid, not a measurement; the OS's own level-detect display is the reference. |
| Import while the drive is busy | The UI disables Import while the drive LED is on; the audio thread also refuses a write that arrives while `driveActive()` and hands it back | No race between the UI check and the write. |

## Booting a disk in the checklists

**To boot a disk: *File → Open Disk Image...*, then *Machine → Reset* (Ctrl+R).**
Open alone swaps the disk under the running OS, as on the hardware (spec 6.5):
the display and the sound stay as they are and nothing is read from the new disk
until the machine boots again. Reset clears RAM, including wave RAM, and boots
the disk in the drive, unsaved changes and all. Wait about a second after Open
before Reset, so the swap has finished. From a fresh start with an empty drive,
Open boots by itself (acceptance 1), so the Reset can be skipped there; it does no
harm. The checklists below say "Open, then Reset" wherever they need a boot.
`tests/test_acceptance.cpp` checks this flow (`acceptance 2: Open leaves the
running OS alone, Open then Reset boots the new disk`).

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

**Status: passed.** The owner ran this checklist and accepted it by ear
(2026-10-22).  `tests/test_acceptance.cpp` runs the same
panel keys headlessly (the test `acceptance 2: an edited parameter survives OS
save, Save As and reopen`) and passes. The display strings below are what that
run shows; the keys are from the OS 3.2 manual (*Selecting and changing a
parameter*, *Saving a sound on a formatted diskette*), and each key is clicked on
the panel (or typed with its panel keyboard shortcut).

1. *File → Open Disk Image...* `Factory Library/IMG/SND-A01.img`, then *Machine →
   Reset* (see *Booting a disk*), and wait for the display to settle on "2." "1"
   (about 15 s).
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
6. *File → Open Disk Image...* the new file (it is the file already in the drive;
   Open inserts it again), then *Machine → Reset*. The OS boots from it as in
   acceptance 1 and settles on "2." "1". (Without the Reset nothing would change:
   the OS keeps running and the display stays as it was.)
7. Press **Param**, **3**, **6**, **Value**. The display shows "3" "1." (value 31):
   the edit came back from the file, because Reset cleared the OS's RAM. Open
   `SND-A01.img`, Reset, wait for "2." "1" and repeat this step: it shows "2" "8."
   (the factory value).

Re-run it after changes to the disk path (FDC write, `DiskSession`) or the file
dialogs.

## Acceptance-5 checklist (format a blank disk with fmt2)

**Status: passed.** The owner accepted it (2026-10-22).  `tests/test_acceptance.cpp` runs the same
panel keys headlessly (`acceptance 5: fmt2 formats a blank disk that then boots`
and `acceptance 5: a sound saved to the formatted disk loads back`) and passes.
Formatting is emulated at real speed (no turbo): on the emulated clock fmt2 takes
**about 106 s from Enter to "Fc"** (all 80 tracks about 33 s, then OS 3.2 is
written and every track is read back), so expect a bit under two minutes of
waiting with the drive LED on.

1. *File → Open Disk Image...* `EnsoniqOS/fmt2`, then *Machine → Reset*. Wait for
   the display to show "F" "d" (fmt2 is asking for a blank disk).
2. *File → New Blank Disk* (Ctrl+N). The title says "Untitled (blank)". The
   Mirage notices the disk change. (If fmt2 had unsaved changes the app would
   ask first; it has none.)
   **Only insert the blank disk while fmt2 shows "F" "d", and do not Reset after
   it.** A blank disk cannot boot. If the Mirage tries to boot it, the boot ROM
   shows "ud" (unformatted disk) and retries for ever, exactly as the hardware
   does. That happens when the blank disk goes into an empty drive at the "nd"
   prompt, when it replaces fmt2 before fmt2 has finished loading (before "F"
   "d"), or when you Reset with it in the drive. To recover, open fmt2 again,
   Reset, wait for "F" "d", then *New Blank Disk*. (Checked headlessly: with fmt2
   at "F" "d" the display stays "F" "d" after the blank goes in, and 1, Enter
   starts formatting.)
3. Press **1**, then **Enter** (format; OS 3.2 goes onto the disk). The drive
   LED lights and the head steps through the tracks.
4. While it runs, try *File → Save Disk Image As...* and pick a name: the save is
   refused with "This disk is not fully formatted (N of 80 tracks). Format it
   with the OS before saving." and no file appears. Retry after step 5.
5. Wait for the display to show "F" "c" (the display's lower-case c) after about
   106 s.
6. *File → Save Disk Image As...* `Formatted.img`. It saves (450,560 bytes).
7. *File → Open Disk Image...* `Formatted.img`, then *Machine → Reset*. fmt2 wrote
   OS 3.2 onto it, so it boots like acceptance 1 (about 15 s) and settles on "2."
   "1". (Without the Reset fmt2 would still be running.)
8. Save an edited sound to it. *File → Open Disk Image...* `Factory Library/IMG/
   SND-A01.img`, *Machine → Reset*, wait for "2." "1". Press **Param**, **3**,
   **6**, **Value** ("2" "8."), then **Up** three times ("3" "1."). *File → Open
   Disk Image...* `Formatted.img` and do not Reset (the Mirage sees the new disk;
   wait about 2 s). Press **Param**, **1**, **1**, **1**, **Enter** (save lower
   sound to place 1; wait for the drive LED to go out), then *File → Save Disk
   Image As...* `Formatted-with-sound.img`.
9. *File → Open Disk Image...* `SND-A01.img`, *Machine → Reset*, wait for "2."
   "1", and check **Param**, **3**, **6**, **Value** shows "2" "8." again (the
   factory sound is back in RAM). *File → Open Disk Image...*
   `Formatted-with-sound.img` (no Reset; wait about 2 s), then press **Load
   Lower**, **1**, **Enter** (wait for the drive LED to go out). Press **Param**,
   **3**, **6**, **Value**: it shows "3" "1." The edited sound came back from the
   formatted disk, not from `SND-A01.img`. Play it.

Re-run it after changes to the FDC (Write Track, Read Address) or to
`DiskSession`.

## WAV import: wave RAM halves and the pitch hint

The import writes converted bytes straight into the 128 KB sample RAM
(`Machine::writeWaveRam`, wave RAM only; the engine API is shared by the dialog and
`mirage_cli --import`). The dialog is described above; the headless route is below.

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

**Pitch hint** (the dialog shows it as "Plays at original pitch around B2 (with the
loaded sound's tuning)", the note name of `originalPitchKey(rate)`):

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

## Acceptance-3 checklist (import a WAV, play it, save it with the OS, reopen)

**Status: passed.** The owner ran this checklist and accepted it by ear
(2026-10-22).  `tests/test_acceptance.cpp` runs the same
route headlessly (`acceptance 3: an imported sine plays at a pitch set by the key`
and `acceptance 3: the imported sound survives an OS save and reopen`) and
passes. No wavesample start or end edits are needed: SND-A01's lower sound
plays its wavesamples from fixed pages of the lower half (keys 41-56 from page
`$48` on, the others from pages `$00` to `$D8`), so an import that fills the half
is what those keys play.

1. *File → Open Disk Image...* `Factory Library/IMG/SND-A01.img`, then *Machine →
   Reset*, and wait for the display to settle on "2." "1" with the drive LED off
   (about 15 s). *Import WAV* stays greyed while the LED is on.
2. *File → Import WAV...* (Ctrl+I) and pick a WAV of at least 2.3 s, for example
   a sustained note or a voice.
3. In the dialog leave **Rate** at 29412 Hz (*Default*), **Into** at *Lower sound
   ($00000)*, **Start page** at 0 and **Normalise** on. Keep the selection at
   2.3 s or more: the text says "65536 bytes at 29412 Hz, 100.0% of the half",
   pages 0 to 255 (and, for a longer file, the orange "Too long for the half"
   note: the rest is cut off). Click **Import**.
4. The status strip shows `Imported 65536 bytes at $00000 (pages 0-255, hex 00-FF)`.
5. Play across the lower keyboard (MIDI 36-71, C2 to B4). Every key plays the
   import, each from its own wavesample's page. Keys 41-56 play it from about
   0.63 s into the selection (page `$48`) a semitone apart, at its original
   pitch around B2 (MIDI 47). The upper keyboard (72 and up) still plays the
   factory upper sound.
6. Save the lower sound to place 1 with the OS: **Param**, **1**, **1**, **1**,
   **Enter** (as in acceptance 2). Wait for the drive LED to go out. The title
   shows "(modified)".
7. *File → Save Disk Image As...* `SND-A01-import.img` (keep the factory disk
   as it is; *Save* would write the import into `SND-A01.img`).
8. *File → Open Disk Image...* `SND-A01-import.img`, then *Machine → Reset* (it
   clears wave RAM, so the import is gone from RAM), wait for "2." "1" and play
   the lower keyboard again: the import plays as in step 5, now loaded from the
   disk. (Without the Reset the import would still be in RAM from step 2 and
   this step could not fail.)

Re-run it after changes to the WAV reader or converter, the host's wave queue
or the dialog.

## Acceptance-4 checklist (other OS images boot and play)

**Status: passed.** The owner ran this checklist and accepted it by ear
(2026-10-22).  `tests/test_acceptance.cpp` boots each image
headlessly and plays note 60 (`acceptance 4: each OS image boots and plays`) and
passes. Times are on the emulated clock (`docs/hardware/boot-rom.md` §13.7).

For each image: *File → Open Disk Image...*, then *Machine → Reset* (otherwise the
previous image's OS keeps running), wait for the display to settle with the drive
LED off, then play a few notes from your MIDI keyboard.

| Image | The display settles on | Then |
|---|---|---|
| `EnsoniqOS/os32` | "2." "1" (about 15 s) | Play. OS 3.2 auto-loads the lower sound, as SND-A01. |
| `EnsoniqOS/masos` | "2." "1" (about 15 s) | Play. |
| `EnsoniqOS/pianos` | "2." "1" (about 15 s) | Play. |
| `EnsoniqOS/rhodes` | "2." "1" (about 15 s) | Play. |
| `EnsoniqOS/resmos32.s` | "2." "1" (about 15 s) | Play. |
| `Soundprocess/Soundprocess OS/Soundprocess OS.img` | "0" "1" (about 13 s) | Play. "01" is the selected program; the system disk carries its own sounds, so no sound pack is needed, and it answers on every MIDI channel. |

Soundprocess was silent until Plan 3 Task 7: it waits for a DOC timing signal on
VIA PB5 before every DOC write, which the emulator did not provide (§13.7). To
try its other sounds: **SEQ REC** (Soundprocess's *PROG* key, manual section 1)
and two digits 01-32 select a program. Each pack in
`Soundprocess/Soundprocess Sound Packs/IMG/` also boots the OS by itself and
plays the same way. (Loading a bank from a pack with the manual's *LOAD* key, a
bank number 1-3 and *ENTER* is not tried here; the manual's key overlay says
which panel key *LOAD* is.) `EnsoniqOS/fmt2` is not in the table: it shows "F" "d" and
waits for a blank disk (acceptance 5).

Re-run it after changes to the DOC, the VIA wiring or the FDC.

## Acceptance-6 checklist (`.edm` disks, raw and Giebler)

**Status: passed.** The owner ran this checklist and accepted it by ear
(2026-10-22).  `tests/test_acceptance.cpp` boots both
disks headlessly and plays note 60 (`acceptance 6: a Metascape .edm boots and
plays`, `acceptance 6: a Giebler .EDM boots its OS`), and `tests/test_edm_format.cpp`
and `tests/test_disk_session.cpp` cover the format and Save / Save As; all pass.
Times are on the emulated clock.

1. Acceptance 1 with `MetascapeMetaSounds/mp1.edm` (a raw 450,560-byte `.edm`)
   instead of `SND-A01.img`: *File → Open Disk Image...* `mp1.edm`, *Machine →
   Reset*, wait for the display to settle on "2." "1" (about 15 s), then play.
2. *File → Open Disk Image...* `Soundprocess/TritonSoundprocess/SP1.EDM` (a Giebler
   file, 389,633 bytes), then *Machine → Reset*. It carries the Soundprocess OS,
   so follow the Soundprocess row of the acceptance-4 checklist: the display
   settles on "0" "1" (about 13 s), then play; **SEQ REC** and two digits select
   other programs. It should sound like `SoundProcess_Vol1.edm`, the same disk as
   a raw rip. (Without the Reset, OS 3.2 from step 1 would still be running.)
3. With `SP1.EDM` still in the drive, *File → Save Disk Image As...*: the dialog
   suggests `SP1.edm`. Save it as `SP1-copy.edm` (keep the original as it is).
   It is written as a Giebler file: it starts with "Mirage Disk" in a text
   editor and is 389,633 bytes, like `SP1.EDM`, unless the OS wrote to the disk.
4. *File → Open Disk Image...* `SP1-copy.edm`, then *Machine → Reset*. It boots to
   "0" "1" and plays like `SP1.EDM`. *File → Save Disk Image As...*
   `SP1-copy.img` writes a 450,560-byte raw image of the same disk.
5. Raw `.edm` stays raw, and nothing is written into your library. In File
   Explorer copy `MetascapeMetaSounds/mp1.edm` to `mp1-copy.edm` in a scratch
   folder. *File → Open Disk Image...* `mp1-copy.edm`, *Machine → Reset*, wait for
   "2." "1" and play a few notes. Save a sound with the OS (**Param**, **1**,
   **1**, **1**, **Enter**; wait for the drive LED to go out). *File → Save Disk
   Image* (Ctrl+S): `mp1-copy.edm` stays 450,560 bytes and does not start with
   "Mirage Disk" in a text editor. *File → Save Disk Image As...* now suggests
   `mp1-copy.img` (a raw `.edm` is offered as `.img`). Type `mp1-giebler.edm`
   instead: that file starts with "Mirage Disk" and is not 450,560 bytes. Open
   it, Reset, and check it boots to "2." "1" as in step 1.

Re-run it after changes to `DiskImage` or `DiskSession`.

## Acceptance-7 checklist (sample live from the audio input)

**Status: not yet run by the owner.** `tests/test_acceptance.cpp` runs the same
route headlessly against a synthetic 440 Hz sine on the jack (`acceptance 7: OS 3.2
samples a sine from the input jack`): the sampled lower half holds the sine at
the right pitch, it plays on the lower keyboard (an octave apart on keys 52
and 64), and it survives an OS save and a reopen (`acceptance 7: a sampled sound
survives an OS save and reopen`). The real input device, the Input slider and the meter can only be
checked by you. You need a sound source plugged into an audio input of your
computer: an audio interface with an instrument, a microphone or a line-level
player is best. Times are on the emulated clock.

1. *Audio → Audio Settings...* The strip's meter says "no input": the app starts
   with no input chosen. Choose the output you listen on, then pick your audio
   interface in the **input device** list (the second device list). The
   "Active input channels" list appears with nothing ticked: tick **only** the
   channel your source is plugged into. Close the dialog. The strip's meter now
   shows an empty bar (not "no input"). The choice is saved for the next start.
2. Put the Input slider at its default, 1. Keep the source **silent** for the
   next step (the input reaches the Mirage's filters at mic gain while it boots).
3. *File → Open Disk Image...* `Factory Library/IMG/SND-A01.img`, then *Machine →
   Reset*, and wait for the display to settle on "2." "1" with the drive LED
   off (about 15 s).
4. Play a sustained sound into the input (a held note or a steady tone, as loud
   as you will play it). **The Input meter moves with it.** With the Mirage on
   mic (parameter 75 off, the default), lower the Input slider until the loud
   parts peak at about 0.05 to 0.08 on the meter (-26 to -22 dB, about a third
   of the way along the bar). For an interface output peaking around half of full
   scale that is a slider of about 0.1. If the meter goes red, lower the slider
   more; if step 5 then shows no middle bar, raise it a little.
5. Press **Sample Lower** (the black *Lower* key under SAMPLE). The display
   blinks "S" "L" for about 2.5 s. Then both digits show one segment. Keep playing
   and watch it: the decimal points only (no level), the bottom bar (quiet), the
   **middle bar** (above the threshold: what you want), the top bar (near
   clipping; fine for the odd peak, too loud if it stays). If you only ever get
   the dots or the bottom bar, raise the Input slider; if the top bar stays,
   lower it.
6. Stop playing. Press **Enter**, then **start the sound** (or have it already
   ringing). Recording begins at the first loud sample. If nothing is loud
   enough within about 1.8 s the display blinks "n" "S" and gives up with
   nothing changed: go back to step 5.
7. **Keep the sound going for about 2.5 s.** The display is blank while it
   records (about 2.2 s; the emulator does not show the real machine's lit middle
   bar). Then the display settles on a steady "S" "F".
8. Play the **lower keyboard** (MIDI 36-71): it plays what you sampled. The
   headless test sampled a 440 Hz sine and found it played back at about 441.5 Hz
   on **E3** (MIDI note 52, with middle C = C4 = 60 as in the rest of this guide)
   and at 882.5 Hz an octave up on **E4** (MIDI 64): so E3 plays your sound near
   the pitch you sampled, E4 is twice that, and the semitones in between follow
   (the keys around E3, MIDI 46-66, are one wavesample). It is a raw 8-bit
   sample at 29.4 kHz with no loop: expect some hiss and a short sound that ends.
   The upper keyboard (72 and up) still plays the factory upper sound.
9. Save the sound with the OS: **Param**, **1**, **1**, **1**, **Enter** (as in
   acceptance 2). Wait for the drive LED to go out; the title shows "(modified)".
10. *File → Save Disk Image As...* into a **scratch folder** you create for this
    (for example `Mirage scratch` on your Desktop), not a library folder, as
    `SND-A01-sampled.img`. (Never *Save*: that would write your sample into
    `SND-A01.img`.)
11. *File → Open Disk Image...* `SND-A01-sampled.img`, then *Machine → Reset*
    (it clears wave RAM, so the sample is gone from RAM), wait for "2." "1". Press
    **Load Lower**, **1**, **Enter** and wait for the drive LED to go out. Play
    the lower keyboard: **it plays the same sound as in step 8**, now loaded from
    the disk.

If step 5 or 6 does not behave: *Audio → Audio Settings...* again and check the
input channel (the meter says "no input" when none is active). Report what the
display showed at each step.

Re-run it after changes to the input path, the jack model or the app's audio
device code.

## Acceptance-8 checklist (line against mic)

**Status: not yet run by the owner.** Headlessly (`acceptance 7: the mic input
is hotter than the line input`) the same full-scale sine swings the sampled bytes
more through the mic than through line and both come out at the right pitch;
what it sounds like is for you to judge. Parameter 75 is the OS's **Line/Mic**
switch: **off (0) is mic, the default**, on (1) is line. Use the same source
and the same playing for all takes.

1. Do steps 1-3 of acceptance 7 (input chosen, SND-A01 opened, Reset, wait for
   "2." "1").
2. **Line take.** Select line: **Param**, **7**, **5**, **Value**, then **Up**
   (the display shows the value: 0 is mic, 1 is line). Put the Input slider so that the loud
   parts of your sound reach about 0.7 to 0.9 on the meter (-3 dB, yellow, not
   red). Sample Lower as in acceptance 7 steps 5-7. Play a few lower-keyboard
   notes and listen: it is a clean sample at a moderate level, with its natural
   dynamics.
3. **Mic take, same settings.** Select mic: **Param**, **7**, **5**, **Value**,
   **Down** (back to 0). Leave the Input slider where it is and play
   the **same sound at the same loudness**. Sample Lower again (it overwrites the
   lower half). Play the same notes. Expect it to be **much louder** (the mic
   path has about 11 times the gain) and, because the source is far above mic
   level, harshly clipped and squashed.
4. **Mic take at the mic's own level.** Lower the Input slider to about a tenth
   of what it was (the loud parts peaking at about 0.05 to 0.08 on the meter) and
   sample once more. Compared with the line take it is about as loud (the slider
   took back the mic's extra gain), but not clipped, and its quiet parts sit
   closer to its loud parts: it is **compressed** (the mic path has a
   compressor).
5. Optional: with the mic take still in memory, save it with the OS and *Save
   As* into your scratch folder as in acceptance 7 steps 9-11.

Say which take was louder, whether the mic take sounded compressed, and whether
the level guidance in step 4 gave a clean sample.

## Known limitations

- Acceptance has to be judged by ear by the owner; automated checks cannot
  hear the output.
- The file dialogs, the Import WAV dialog, MIDI devices, audio device
  selection, menu shortcuts and panel keyboard shortcuts are not exercised by
  automated tests (the queues,
  the MIDI parser, `DiskSession`, the panel layout and the soft clip are).
- The boot calibration tones are deliberately not muted (see the decisions).
- The status strip's "(modified)" can lag by a block or two.
- Windows only so far; the Windows file chooser shows a single filter,
  `*.img;*.edm;*`.
- **Sampling's analog input stage is a model.** The jack's scale, the mic
  gain (11), the compressor curve and the polarity are model choices, not
  measurements (`docs/hardware/filters.md` §3.3). Levels in the checklists come
  from the model; retune by ear.
- The MASOS external input-filter cartridge (parameter 93 above 0) is not
  modelled. The input is mono (the average of the active channels).
- The emulator's display is blank during the 2.2 s recording (see *Audio input
  and sampling*); the real machine shows the middle bar.
- The input meter's value is the last block's peak, 10 times a second. Audio
  device selection, the Input slider, the meter and sampling from a real input
  device are not exercised by automated tests (the engine, the host's input path
  and the OS's sampling run headlessly against a synthetic jack signal).
- WD1772 Read Track is not modelled (it completes with no data and no error);
  none of the images tested here needs it. Write Track does not time out waiting for a first
  byte (an unanswered start writes zeros), and only the Mirage's own track
  layout (ids 0-5, sizes 3, 3, 3, 3, 3, 2) counts as formatted
  (`docs/hardware/fdc.md`).
- Formatting runs at the real speed (about 106 s for fmt2). There is no turbo.
- The DOC's CA3 line is high in the two refresh slots as an assumption: the
  DOC's pin behaviour there is not documented, and Soundprocess needs it high
  to play (`docs/hardware/boot-rom.md` 13.7). A CPU-written halt with M0 set
  starts nothing (MAME routes it through its halt routine).
- The Import WAV pitch hint is measured on SND-A01's lower sound and moves with
  the loaded sound's tuning. The import does not edit the OS's wavesample
  start, end, loop or tuning, and it is lost on a reset unless the OS saves it.
- The Import WAV dialog closes when *Import* is clicked even if the post to the
  audio thread fails (an error shows; the selection is lost). A hexadecimal
  number typed in the Start page box reads as 0.
- The MIDI feeder's `MIDI dropped` count can include a message that is later
  delivered: if the host queue refuses a direct post (more than 1024 commands
  pending in one block gap, for example the clock lagging behind a dump after
  a Reset), the host counts the bytes as dropped and the message then joins the
  feeder's backlog and goes out.
- A save always writes the file; "(modified)" clears only if the OS has not
  written to the disk since the copy was taken, so a disk that the OS keeps
  writing stays "(modified)".
