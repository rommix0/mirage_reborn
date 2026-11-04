# The VST3 plugin

`Mirage.vst3` is the same emulated Mirage as `Mirage.exe`, as a VST3 instrument
for a DAW (Windows x64). You put it on an instrument track, open a disk, play it
from the track's MIDI, record and bounce it, and the DAW project brings it back
exactly as you left it: the OS in RAM, your sounds and edits, and the disk with
any changes the OS wrote to it. It is a second wrapper around the same engine;
the design is in `docs/specs/2026-11-03-vst3-plugin-design.md`.

Two things differ from the standalone, on purpose (owner, 2026-10-18): the boot
calibration tones are muted, and floppy access is near-instant. Both are
explained under *The two plugin-only behaviours*. Everything else (the panel,
the disks, the OS, the sound) is the same machine. This guide assumes you have
read `docs/app.md` for the panel, the menus and the checklists' key presses.

## Build and install

From the folder that contains `emulator/`:

```
cmake -S emulator -B emulator/build -G "Visual Studio 16 2019" -A x64
cmake --build emulator/build --config Release --target MiragePlugin_VST3 --parallel
```

The plugin is built whenever the app is (`-DMIRAGE_BUILD_APP=ON`, the default).
In Git Bash use `--parallel`, not `-- /m`. **Use Release for live play**; the
Debug build is unoptimised and glitches.

The build leaves the plugin at
`emulator/build/plugin/MiragePlugin_artefacts/Release/VST3/Mirage.vst3`
(a folder; the Debug build is under `.../Debug/VST3/`). The build does not copy
it anywhere. Either:

- copy `Mirage.vst3` to `C:\Program Files\Common Files\VST3` (needs an
  administrator), or
- add the folder that holds it to your DAW's VST3 search paths and rescan.

The plugin shows up as **Mirage** (company "Mirage Emulator", version 0.3.0),
category Instrument / Sampler. There is no standalone build of it: `Mirage.exe`
is the standalone.

## The boot ROM

The plugin needs the 4096-byte boot ROM before it can start a machine. It
looks, in this order, and uses the first 4096-byte file it finds:

1. the environment variable `MIRAGE_PLUGIN_ROM` (a full path). This is for tests
   and development only; leave it unset;
2. the ROM path saved in the plugin's settings (set by *Choose ROM*);
3. `mirage.bin` in the folder that holds `Mirage.vst3`;
4. the app's saved ROM path (read from `MirageEmulator.settings`, never written);
5. the build's `MIRAGE_ROM`, the path the plugin was built with (a development
   convenience; it is only a path, not the ROM);
6. otherwise the editor shows "No Mirage boot ROM was found. The plugin is
   silent until you choose one (a 4096-byte file)." with a **Choose the Mirage
   boot ROM** button. The plugin is silent until you choose one. Choosing a
   file that is not exactly 4096 bytes is refused.

The ROM is read once per DAW process and shared by every Mirage in it. A ROM
whose SHA-1 is not `ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d` is accepted, but
then the calibration is not muted and the strip says "calibration not muted
(unknown ROM)".

**The ROM and your disk images are never copied** into the repository, a DAW
project or the plugin: the project stores only the ROM's SHA-1.

## Buses, parameters and the editor

| Item | What it is |
|---|---|
| Output | "Output", stereo (mono is accepted). The Mirage is mono, so both channels carry the same signal. |
| Input Jack | A sidechain input, mono or stereo, **off by default**. Off means nothing is plugged into the jack. On means plugged in; the channels are averaged, then multiplied by Input Gain. Everything in `docs/app.md` ("Audio input and sampling") then applies. |
| Volume | Parameter, 0 to 1, default 1: the panel's slider. A host-side output gain, smoothed over 20 ms per block. Can be automated. |
| Input Gain | Parameter, 0 to 4, default 1: the strip's Input slider. Can be automated. |
| MIDI | In and out. Program changes, pitch bend, aftertouch and CCs reach the machine as MIDI. |
| Latency / tail | Reports 0 latency and an infinite tail (the machine can sound with no input, for example from its sequencer). |

Nothing else is a parameter: panel keys and OS parameters belong to the OS and
are not automatable. There are no plugin presets; sounds live on disks.

**The editor** is the replica panel (the same artwork as the app) with a small
menu bar above it and the status strip below it. It resizes and keeps the
panel's aspect ratio (at least 500 px wide); its size is saved with the project.
It can be closed and reopened at any time: the machine and its disk live in the
plugin, not the editor. Panel keyboard shortcuts work while the editor has
keyboard focus, as in the app (`docs/app.md`, "Panel keyboard shortcuts"); Ctrl
shortcuts are best effort, because DAWs keep many keys for themselves. Use the
menus if a shortcut does not arrive.

| Menu | Item | Shortcut |
|---|---|---|
| File | Open Disk Image... | Ctrl+O |
| File | New Blank Disk | Ctrl+N |
| File | Save Disk Image | Ctrl+S |
| File | Save Disk Image As... | Ctrl+Shift+S |
| File | Eject Disk | Ctrl+E |
| File | Import WAV... | Ctrl+I |
| Machine | Reset (power-cycles the Mirage; the disk and unsaved changes stay) | Ctrl+R |
| Machine | Panic (flushes MIDI not yet played; All Notes Off, CC 123, on all 16 channels) | |

There is no MIDI, Audio or Exit menu: devices belong to the DAW. All dialogs are
asynchronous (the DAW stays usable behind them). *Import WAV* is greyed out
while the drive LED is on and while the DAW is not running audio.

**Status strip.** Left: the disk's file name, with "(modified)" when it differs
from its file, or "No disk". Right: the Input slider and meter, and a warning
only when something is wrong: the outcome of the last WAV import (for 15 s),
`illegal ops: N`, `MIDI dropped: N`, "calibration not muted (unknown ROM)", a
project that could not be restored (below) and a failed save. `MIDI dropped`
counts bytes of MIDI lost on the way in or out (as in the app), plus one for
each MIDI-out message the plugin could not pass on (a SysEx over 65,535 bytes).

## The machine runs free

The Mirage runs whenever the DAW calls `processBlock`, whether or not the
transport plays, as a hardware Mirage in a rack would. **Transport, tempo and
play position are ignored.** If the DAW stops calling `processBlock` (a
suspended plugin, the audio engine off), emulated time stops with it. A freshly
inserted Mirage therefore starts booting at once and keeps running while the
song is stopped.

## Disks in a plugin

They work as in the app (`docs/app.md`, "Menus and shortcuts"): Open does not
reset (use *Machine → Reset* to boot a new disk); the unsaved-changes prompt
comes before Open, New Blank Disk and Eject; Save writes the format the file was
read in; a disk that is not fully formatted cannot be saved.

Differences:

- **Closing the project or removing the plugin does not ask to save.** The disk,
  with everything the OS wrote to it, is inside the project, so nothing is lost.
- A disk is "(modified)" when it differs from its **file**, not from the
  project. After the project reopens the strip still says "(modified)" until
  you *Save* or *Save Disk Image As...*.
- Nothing is ever written to your image unless you choose Save or Save As.
- Two instances can open the same image file. Each has its own copy in memory,
  and **the last Save wins**. This is documented, not prevented.

## State: the DAW project

When the DAW saves, the plugin stores one compressed block per instance
(`MirageState`, version 1): the full machine snapshot (CPU, RAM, wave RAM, every
chip, the audio path, the jack, the MIDI input queue, the panel and the
inserted disk with its modified flag), which file the disk came from and its
format, the two parameters and the editor size. Raw, a snapshot is about
610 KB; the project stores it compressed. The snapshot format is the app's own (version 5), so nothing
about it differs from a standalone snapshot.

- **Taking it** pauses no audio. If the DAW is not running audio, the plugin
  takes it directly.
- **Reopening** builds a new machine from the snapshot and carries on from that
  exact cycle: **no boot**, no calibration, the same display and sound.
- **Held notes.** A note held when the project was saved is still held after it
  reopens, because the DAW's note-off went to the old instance. Use *Machine →
  Panic*. The plugin sends nothing on its own, so the restore stays exact.
- **A project whose Mirage cannot be restored.** This happens with no ROM yet,
  with a ROM of a different SHA-1 from the project's, or with a project saved by a
  newer version. The plugin keeps the saved block untouched and returns it
  unchanged when the DAW saves again, so saving never destroys it. It outputs
  silence, and the editor says why (for example that the project was saved with
  boot ROM `<sha>` and you should choose that ROM). Choosing the right ROM
  restores it. *Machine → Reset*, *File → Open Disk Image...* and *File → New
  Blank Disk* first ask "Discard the Mirage saved in this project?" before they
  start a fresh machine.

## The two plugin-only behaviours, and why they stay faithful

Both act only on the **sound that leaves the plugin** and on **how fast the
machine runs against the clock**. Neither changes anything inside the machine:
the same CPU, RAM, chip and disk state, cycle for cycle, as a machine that does
not use them. No OS code is patched and no OS memory is touched. Both are
decided from hardware signals that every OS shares, so they work the same
under every OS image. A snapshot taken with them on loads in the standalone and
the reverse.

### Calibration tones: muted

The boot ROM's routine at `$F571-$F5E6` (it makes the filters self-oscillate for
about 1.1 s, at about 6.25 to 7.36 s after power-on) is a chip on the board, not
part of an OS, and every OS calls it. While the CPU is inside that routine, the
plugin closes a gate on its output:

- the gate closes over 5 ms, stays closed until the CPU has been outside the
  routine for 20 ms in a row (this bridges the interrupts and the few
  instructions between the 8 calls), then opens over 10 ms;
- it is applied after the DC blocker and before the output resampler, so the
  output matches an unmuted machine again once the resampler (32 scans) has
  passed;
- it is armed only for the known ROM (SHA-1 `ec6ea561...`). With any other
  ROM you hear the calibration, as in the standalone;
- `$F5E7`, the routine the OS calls on every note, is outside the range, so
  notes are never muted.

### Floppy access: near-instant (fast-forward)

While the **drive motor is on** (the drive LED) or the gate is closed, the plugin
runs the machine **faster than real time**, with its output silent. The disk
chip keeps its realistic step, rotation and byte timing; only the waiting is
skipped.

- **Real-time play:** at the start of each audio block, the machine runs in
  1 ms (emulated) chunks until the condition ends or half the block's
  wall-clock duration is used. If it ended, the rest of the block plays
  normally with a 10 ms fade-in; if the budget ran out, the block is silent and
  it continues next block. The machine is therefore about 12 times real time on the
  owner's PC, and less on a slower one. The budget is each instance's own:
  several Mirages loading at once on one DAW audio thread each take up to half
  the block, so together they can overrun it (crackles) until they finish.
- **Empty drive:** the OS's "nd" retry turns the drive motor on about every
  1.6 s, so an idle Mirage with no disk briefly fast-forwards then too.
- **Offline bounce:** no wall-clock budget. The chunks run until the condition
  ends, up to 60 s of emulated time per block. The cap keeps a disk the OS
  retries for ever (an unformatted disk at "ud") from hanging a bounce.
- Input that arrives in a fast-forwarded block (MIDI, panel keys, disk swaps,
  WAV writes) is applied at its start. The key-hold and disk-swap timers run on
  emulated time, so they still hold (100 ms and 0.5 s of emulated time).

**Measured** (Release, on the owner's PC, 2026-11-03):

| What | Plugin | Standalone |
|---|---|---|
| Insert, Open `SND-A01` until it plays | 1.76 s | about 15 s |
| Panel *Load Lower* | about 0.24 s | the real load time |
| Worst fast-forwarding block | 5.48 ms used of a 10.67 ms block (512 frames at 48 kHz) | n/a |

The engine runs a 15 s boot in about 0.62 s (about 24 times real time).

**Cost:** if an OS plays a sound while it loads from disk, that sound is skipped.
The OSes in the tree silence their voices while loading anyway.

## Threading

The audio thread owns the `Machine` through `RealtimeHost`, as in the app
(`docs/app.md`, "Threading model"). The DAW's threads take the place of the
device threads.

| Thread | Does |
|---|---|
| DAW audio (`processBlock`) | Runs the block with the host's MIDI events and the jack input, then the fast-forward and the mute, then writes MIDI out. It never allocates, frees, locks or does file I/O. |
| Message thread | The editor, menus, `DiskSession` (open, save, a 10 Hz poll), WAV conversion, starting a machine, the ROM lookup. It talks to the audio thread through the host's queues. |
| Whatever thread the DAW uses for `getStateInformation` / `setStateInformation` | Posts a snapshot request or builds a new machine off the audio thread, then waits for the audio thread to answer. It never waits on the message thread. |

## Decisions made for the plugin

| Decision | Choice | Why / cost if wrong |
|---|---|---|
| Calibration mute trigger | Output gate while the CPU PC is in `$F571-$F5E6`, checked once per DOC scan. Armed only for the known ROM SHA-1. | The routine is in the boot ROM and every OS calls it. Another ROM is heard as in the standalone. |
| Gate timing | Closes over 5 ms, holds 20 ms after the PC leaves the range, opens over 10 ms; after the DC blocker, before the output resampler. | The hold bridges interrupts and OS code between the 8 calls. Retune by ear. The gate is fully closed 5 ms after the first entry, but the output only goes silent about 13 ms after it (a delay in the audio path that was measured and not explained). |
| Fast-forward trigger | Drive motor on, or gate closed, at a block's start. | Hardware signals only. |
| Fast-forward pacing | 1 ms chunks; real time: until the trigger ends or half a block's duration, per instance; offline: until the trigger ends, up to 60 s per block. | About 12 times real time, less on a slower PC. Cost: sound an OS plays during a load is skipped, and several instances loading at once on one audio thread can overrun the block. |
| Small blocks while loading | One 1 ms chunk always runs per fast-forwarded block. | With very small blocks (a few dozen frames) a fast-forwarding block can use more than real time. Raise the block size while loading if the DAW crackles. |
| Zero-frame blocks | A block of zero frames applies queued input but never fast-forwards. | An offline zero-frame block could otherwise run 60 s of emulated time. A load waits for the next real block. |
| Input during a fast-forward | Applied before the chunks; key releases and swap inserts that fall due are applied between chunks. | Emulated-time timers still hold. |
| Host MIDI in | Each event is queued at its frame in the block, with no added latency (the app adds one block). A 256 KB backlog on the audio thread feeds the machine's 4096-byte MIDI ring when a block segment ends. Overflow is dropped and counted in `MIDI dropped`. At most 8192 events per block are taken; the rest are dropped and counted too. | Sample-accurate; long SysEx dumps from a DAW track survive. |
| MIDI out | After each block, the bytes the OS sent are parsed into messages on the audio thread (SysEx up to 65,535 bytes, the most a JUCE `MidiBuffer` event holds), placed at the frame their cycle maps to, and clamped to the block. A longer SysEx is dropped and counted in `MIDI dropped` (one per SysEx). | No extra thread. Cost: an OS SysEx over 65,535 bytes, such as a long wavesample dump, is lost; the app passes it on. |
| State taking | The state thread posts a buffer sized from the last snapshot plus 64 KB; the audio thread fills it at a block start. With no block for 200 ms (audio stopped) the snapshot is taken directly; while blocks run it waits up to 2 s. | No allocation or pause on the audio thread. A save can take up to 2 s during constant panel-key tapping. |
| Project loading | While no block has ever run (a project loading), state calls do not wait the 200 ms. | Many instances load fast. |
| State restoring | A new machine is built off the audio thread and swapped in at a block start; the old one is freed off the audio thread. The blob stays "pending", and a save returns it, until the swap is confirmed. | Exact recall; a save during the swap returns the right state. |
| A rare pending disk insert | If the project is saved while a disk swap is half done and the audio is stopped, the disk is not in that project (your file is untouched) and it reopens with an empty drive. | Saving never forces the machine's clock forward. |
| Unrestorable state | The blob is kept and returned unchanged; the plugin is silent; Reset/Open/New Blank ask before discarding. | No silent loss of the project's Mirage. |
| State format | `MirageState`, version 1, GZIP-compressed. | Wave RAM and empty sectors compress well. |
| Buses | Stereo output (mono allowed); Input Jack sidechain, mono or stereo, off by default; channels averaged. | Hosts show it as a sidechain. |
| Parameters | Volume and Input Gain only. | As the app's sliders. |
| ROM lookup | As above. Loaded once per process. | The ROM is never in the project. |
| Plugin settings | `%APPDATA%\MirageEmulator\MiragePlugin.settings`: the ROM path, the last disk folder and the last WAV folder. One file shared by all instances, with a lock between processes. | It never touches the app's file. |
| Disk asset root | The build's `MIRAGE_ASSET_ROOT` defaults to `../disks and manuals` (the library folder was renamed). | Tests that needed the old name used to skip silently. |
| Output latency | About 5.4 ms extra output latency after the power-on rate change, in both the app and the plugin. | Pre-existing and not changed here: fixing it would shift the standalone's output. |
| Calibration in `Mirage.exe` | Still heard. The standalone does not use the gate or the fast-forward. | See `docs/app.md`, "Decisions made for the app". |

## Known limitations

- **No MIDI clock from the DAW.** VST3 carries no MIDI clock, so the OS's
  sequencer cannot sync to the DAW's tempo.
- **Held notes after reopening a project.** Use *Machine → Panic*.
- **Sound during a load is skipped** (fast-forward).
- **Small DAW blocks while loading.** One 1 ms chunk always runs per
  fast-forwarded block, so with very small blocks loading can overrun real time.
- **Several instances loading at once** on one DAW audio thread can overrun
  the block: each has its own half-block budget.
- **Two instances on one image:** the last Save wins.
- **A host that withholds program changes from VST3 instruments** hides them from
  the Mirage. That is the host's limit.
- **A SysEx dump from the Mirage's OS longer than 65,535 bytes** (for example
  a wavesample dump) cannot leave the plugin: a JUCE `MidiBuffer` event holds no
  more. It is dropped and counted in `MIDI dropped`. Make such dumps from
  `Mirage.exe`, which sends them whole.
- **A very long SysEx dump into the Mirage** (one block segment longer than about
  1.3 s) can let the machine's 4096-byte MIDI buffer run dry between top-ups,
  leaving gaps on the wire. The 256 KB backlog and "MIDI dropped" cover the
  normal case.
- **Project saved mid-disk-swap** reopens with an empty drive (see decisions).
- **A project saved while a key is being tapped** can take up to 2 s to save.
- **The calibration is only muted for the known ROM.**
- **Hidden parameters.** Some DAWs list extra MIDI CC parameters for the plugin;
  those are the DAW's, not the Mirage's.
- **Output latency** is about 5.4 ms higher after power-on, as in the app
  (pre-existing, not changed).
- Windows x64 and VST3 only. No automation of panel keys.
- Everything in the app's known limitations that concerns the machine itself
  (`docs/app.md`) applies here too.

## Acceptance-9 checklist (by ear, in your DAW)

**Status: not yet run by the owner.**

Run it in the Release build, in your DAW, on an instrument track. Times below
are measured on your PC (Release): a factory disk plays 1.76 s after Open, a
panel *Load* takes about 0.24 s. Allow some margin for the DAW. Each numbered
step is a separate item to accept.

1. **Insert.** Add *Mirage* on an instrument track. If the editor asks for the
   boot ROM, click **Choose the Mirage boot ROM** and pick `mirage.bin`. The
   panel appears.
2. **Open a disk, no tones.** *File → Open Disk Image...*
   `Factory Library/IMG/SND-A01.img`. Expect the drive LED to light and the
   display to settle on "2." "1" within about 2 s, and the sound to be
   playable then. You hear **no calibration tones** at any point. (In the
   standalone this takes about 15 s with the tones.)
3. **Play and record.** Arm the track, play the Mirage from your keyboard,
   then record a short MIDI clip (a few notes) and play it back. Expect the
   notes to sound on time.
4. **Load from the panel.** Press **Load Lower**, **2**, **Enter** on the
   panel, as in acceptance 1. Expect the load to be near-instant (about a
   quarter of a second of silence), with no tones, then play the new sound.
5. **Project recall.** Edit parameter 36 as in acceptance 2 (steps 2 to 4):
   **Param**, **3**, **6**, **Value** (display "2" "8."), **Up** three times
   ("3" "1."), then save to place 1 with the OS (**Param**, **1**, **1**, **1**,
   **Enter**) and wait for the LED to go out. The strip says "(modified)".
   Save the DAW project, quit the DAW and reopen the project. Expect the
   Mirage to **play at once** with no boot and no tones; **Param**, **3**, **6**,
   **Value** still shows "3" "1."; and the strip still says "(modified)". Use
   *File → Save Disk Image As...* into a **scratch folder** (never over
   `SND-A01.img`); the "(modified)" goes away.
6. **Bounce.** Bounce (render) the recorded clip offline and listen to the
   result. Expect it to sound the same as the real-time take (small timing
   differences from the DAW's own offline mode are not a fault; a missing or
   very different sound is).
7. **Two instances.** Add a second *Mirage* on another track and open
   `EnsoniqOS/masos` (or one of the other OS disks from acceptance 4), then
   *Machine → Reset* if it does not boot. Expect no calibration tones from
   either, and both to play their own sounds.
8. **Input Jack.** Enable the plugin's sidechain input (the Input Jack) and
   route an audio track to it. Sample as in acceptance 7 (Input slider, meter,
   **Sample Lower**, **Enter**, play the source, then play the sampled sound
   from MIDI). Expect the same result as in the standalone.
9. **The standalone is unchanged.** Run `Mirage.exe`, open `SND-A01.img`.
   Expect the calibration tones for about 1 s (about 4-5 s after Open) and the
   load at real speed (about 15 s).

Re-run it after changes to the gate, the fast-forward, the state code or the
plugin editor. If something sounds wrong, say which step and what you heard;
the 5/20/10 ms gate timings and the half-block budget are the things to retune
by ear.
