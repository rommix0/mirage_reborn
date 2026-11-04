# Ensoniq Mirage Emulator — VST3 Plugin Design

Date: 2026-11-03
Status: approved by the owner (2026-11-03)

Extends `2026-10-18-mirage-emulator-design.md` (the main spec: §1 intent, §3
anti-goals, §6.1 architecture, §6.9 machine state) and
`2026-10-22-audio-input-sampling-design.md` (§9, the jack in a plugin). The
threading model it builds on is in `docs/app.md` ("Threading model").

The owner asked to skip the questions (2026-11-03). Everything below that is not
quoted from the owner's earlier decisions is **my call**, marked so, and open to
change at review.

## 1. Intent

The same emulated Mirage as `Mirage.exe`, as a VST3 instrument in a DAW. You put
it on an instrument track, open a disk, play it from the track's MIDI, record and
bounce it, and the DAW project brings it back exactly as you left it: the OS in
RAM, the sounds and edits, and the disk with any changes the OS wrote to it.

It is a **second wrapper around the same engine** (main spec §1, §6.1). The engine
and `mirage_host` stay JUCE-free; the plugin is a new JUCE target next to the app.

### What makes the plugin different from the standalone (owner, 2026-10-18)

The owner wants two things in the VST only. The standalone stays as it is.

1. **The boot filter-calibration sound is muted.** This is the boot ROM's `$F571`
   routine self-oscillating the 8 filters for about 1.1 s, at about 6.25 to 7.36 s
   of emulated time after power-on (`docs/hardware/boot-rom.md` §13.4).
2. **Floppy access is instant.** There is no waiting for the realistic WD1772
   step, rotation and byte timing when the OS loads.

Both have to keep the faithful-emulation rule (main spec §3): no OS-specific
code, no OS patching, no writing OS RAM or parameters. §3 below explains how.

## 2. Goals, non-goals, anti-goals

### Goals

1. **A VST3 instrument** (`Mirage.vst3`, Windows x64) that loads in any VST3 host:
   stereo output, MIDI in and out, and an optional input for the emulated input jack.
2. **The replica panel as the editor.** It is the same `PanelComponent` as the app,
   with a small menu bar above it and the status strip below it.
3. **Disks inside the plugin.** Open, New Blank, Save, Save As, Eject and Import WAV
   work as in the app, through `DiskSession`.
4. **Exact recall.** The DAW project stores the full machine snapshot, including
   the in-memory disk. When the project reopens, the plugin carries on from that
   state without booting again.
5. **Sample-accurate MIDI from the host.** Each event goes into the machine at its
   frame in the block, with no extra block of latency (the app adds one).
6. **The DAW's audio as the input jack.** The OS samples from it as in Plan 4.
7. **Offline bounce** renders the same machine as real-time play.
8. **Many instances**, each a separate Mirage.
9. **Calibration muted and disk access fast** (§3), in the plugin only.

### Non-goals (this plan)

- **Other formats.** No AU, AAX, CLAP or VST2, and no macOS or Linux build. The
  JUCE target can add formats later.
- **No standalone build of the plugin.** `Mirage.exe` is the standalone.
- **No MIDI clock from the DAW's tempo.** VST3 carries no MIDI clock messages, so
  the OS's sequencer cannot sync to the DAW's tempo. Making clock bytes from the
  host tempo is possible later.
- **No automation of panel keys or OS parameters.** OS parameters belong to the
  OS (main spec §3), and the plugin cannot know them.
- **No plugin presets or programs.** Sounds live on disks, as on the Mirage.

### Anti-goals (a failure even if it works)

- Anything that only works for one OS, such as detecting an OS's boot phase from
  its RAM, timing or display.
- Changing the standalone's behaviour. The app keeps realistic timing and audible
  calibration (`docs/app.md`, decisions).
- Changing the emulated timeline. The plugin runs exactly the same cycles, in the
  same order, as the standalone would. Only how emulated time maps to wall-clock
  time and what reaches the speakers may differ (§3).
- Silently losing the user's work. This covers a project's saved machine that
  cannot be restored (§5.3) and unsaved disk changes (§6).
- Putting ROM bytes in a DAW project, in the repository or in the plugin binary.

## 3. The two VST-only behaviours, and why they stay faithful

The rule both mechanisms follow (my call): **they act on the output and on the
emulation's speed, never on the machine's state.** Whether they engage is decided
from hardware signals that are the same under every OS: the drive motor line, and
the CPU's program counter inside the 4 KB boot ROM, which is a chip on the main
board. A snapshot taken with them on can be loaded into the standalone, and the
reverse, and runs identically.

### 3.1 Muting the calibration: an output gate on the ROM routine

- **What triggers it.** The DOC's per-scan mix is muted while the CPU is executing
  the boot ROM's calibration routine, `$F571-$F5E6`. That range includes `$F5DC`,
  which `$F571` calls, and excludes `$F5E7`, the ROM routine the OS calls on every
  note to set a voice's cutoff and resonance (`docs/hardware/filters.md` §2).
  The disassembly confirms the range holds the whole routine: `$F571`, its wait
  loop `$F5BE` and `$F5DC`. It leaves the range only through `JMP $FC91`, the
  "O.S." error path.
- **Why it is the hardware, not an OS.** The routine is in the boot ROM, not on the
  disk. Every OS in the tree (OS 3.2, MASOS, Soundprocess, resmos32) calls the same
  routine (`boot-rom.md` §13.6-13.7). An OS that never calls it never gets muted,
  and an OS that calls it later (say, to recalibrate) gets the same mute, which is
  the same sound.
- **How it behaves.** The PC is checked once per DOC scan (29.4 kHz). The gate
  closes with a 5 ms ramp the first time the PC is in the range. It stays closed
  until the PC has been **outside the range for 20 ms in a row**, then opens with a
  10 ms ramp. The hold bridges interrupts taken during the routine and the few
  microseconds of OS code between the 8 calls. The calibration ends with cutoff and
  resonance at 0 (`filters.md` §2), so nothing is left ringing when the gate
  opens. The 5 ms, 20 ms and 10 ms are **my call**, to be tuned by ear.
- **Only for the known ROM.** The range is a fact about the ROM with SHA-1
  `ec6ea561…`. With any other ROM (accepted with a warning, as in the app) the gate
  is not armed, and the calibration is heard as in the standalone. The status strip
  says "calibration not muted (unknown ROM)".
- **Where it lives.** It lives in the engine's audio path, because only the engine
  sees each scan: `Machine::setOutputMuteRange(lo, hi)`, which is off by default.
  `RealtimeHost` arms it, and only the plugin asks the host to. The gate's ramp
  and hold are **not saved** in the snapshot. After `loadState` they are set
  from the current PC (closed if it is inside the range). So the snapshot format
  stays at version 5, and a standalone snapshot never carries a mute.

### 3.2 Instant disk: fast-forward while the drive runs

- **What it does.** While the **drive motor is on** (`Machine::driveActive()`, the
  drive LED) or the calibration gate is closed, the host runs the machine **faster
  than real time** with its output silent. The emulation itself is unchanged: the
  WD1772 keeps its realistic step, rotation and byte timing, and the OS sees
  exactly the timing it sees in the standalone. Only wall-clock time is skipped.
- **Why this way** (my call, over the alternatives):
  - *A zero-latency drive* (no step, settle or rotation delays) changes the chip's
    behaviour that OSes time against (index-pulse time-outs, `fdc.md`). Even then,
    data still arrives one byte per 32 µs through the CPU's NMI handler, so a
    66-sector load stays above 2 s. It is less faithful and not instant.
  - *Muting a fixed stretch of time after power-on* is a boot-timing heuristic. It
    is close to the OS-specific detector the app's decisions table rejected.
  - Fast-forward keys on a hardware line, changes nothing inside the machine, and
    is as fast as the CPU allows.
- **How fast.** The engine runs about 24 times real time on one core of the
  owner's PC (a 15 s boot in 0.62 s, Release, measured 2026-11-03).
  - **Real-time playback:** at each block, while the condition holds, the host
    runs the machine in 1 ms (emulated) chunks until the condition ends or it has
    used **half the block's duration** of wall-clock time. That is about 12 times
    real time on this PC, and less on a slower one. The budget is each
    instance's own, so several instances loading at once on one DAW thread can
    overrun the block. With the drive empty, the OS's "nd" retry turns the motor
    on about every 1.6 s, so the host briefly fast-forwards then too. If the
    condition ended, the block is then rendered normally, with a 10 ms fade-in. If
    the budget ran out, the block is silent.
  - **Offline rendering** (`isNonRealtime()`): there is no wall-clock budget. The
    chunks run until the condition ends, up to 60 s of emulated time per block. The
    cap keeps a disk the OS retries for ever, such as an unformatted disk at "ud",
    from hanging the DAW.
  - Expected result (to be measured in the plan): a factory disk is playable
    about 1-2 s after Open in real time instead of about 15 s, a panel *Load*
    takes a fraction of a second, and fmt2's 106 s format takes about 9 s.
- **What you hear meanwhile:** silence. The OSes in the tree silence the voices
  while they load anyway. If an OS kept playing during a load, that sound would
  be skipped. This is the known cost.
- **Input during a fast-forward.** MIDI, panel keys, disk swaps and wave writes
  that arrive in a fast-forwarded block are applied at the block's start, before
  the chunks run. The key-hold and disk-swap timers run on emulated cycles, so
  they still hold (100 ms and 0.5 s of emulated time). The jack holds its last
  value (the documented behaviour of `runCycles` outside `render`).
- **Where it lives:** `RealtimeHost::setFastDisk(FastDisk::{Off, RealTime,
  Offline})`, which defaults to `Off`. The app never calls it. The clock is
  `std::chrono::steady_clock`, which needs no JUCE.
  *Implementation note:* the code has `HostOptions{.fastDisk}` (off by default),
  given to the `RealtimeHost` constructor with `.muteCalibration`, and
  `RealtimeHost::setOffline(bool)` for the offline cap, which the plugin calls
  each block with `isNonRealtime()`. The app passes no options.

## 4. The plugin

### 4.1 Identity and buses (my calls)

| Item | Choice |
|---|---|
| Format | VST3 only, Windows x64, built by `juce_add_plugin` in a new `plugin/` folder |
| Name | "Mirage", company "Mirage Emulator" (as the app), version 0.3.0 |
| Category | Instrument, Sampler; `IS_SYNTH`, `NEEDS_MIDI_INPUT`, `NEEDS_MIDI_OUTPUT` |
| Output bus | "Output", stereo (mono allowed). The Mirage is mono, so both channels get the same signal. |
| Input bus | "Input Jack": an auxiliary (sidechain) bus, mono or stereo, **off by default**. Off means nothing is plugged into the jack. On means plugged in, with the channels averaged as in the app. Hosts show it as a sidechain input. |
| Parameters | **Volume** (0..1, default 1), the panel slider. **Input Gain** (0..4, default 1, the app's skew with 1 in the middle), the strip slider. Both can be automated and both are saved with the state. Nothing else. |
| Latency reported | 0. The engine's own delays (about 1 ms output resampler, 1.2 ms jack) are part of the instrument, as the MIDI wire is. |
| Tail | Infinite. The machine runs and can make sound with no input, for example its sequencer. |

### 4.2 Running free

The emulated Mirage runs whenever the DAW calls `processBlock`, whether or not the
transport plays, as a hardware Mirage on a studio rack would. Transport, tempo and
play position are ignored. If the DAW stops calling `processBlock` (suspended
plugin, audio engine off), emulated time stops with it.

### 4.3 MIDI

- **In.** The block's `MidiBuffer` goes straight to a new
  `RealtimeHost::process(in, out, frames, rate, events)`. Each event carries its
  frame offset, and its bytes are queued into the machine at that frame. There is
  no queue between threads and no added latency. The existing timestamped
  `postMidi` path stays for the app.
- **Long SysEx** (a wavesample dump through a DAW track) can exceed the machine's
  4096-byte input ring. The host keeps a preallocated backlog of 256 KB on the
  audio thread, owned by `RealtimeHost`, and tops the ring up at each segment end,
  as `MidiInFeeder` does for the app. Anything beyond the backlog is dropped and
  counted in `MIDI dropped`.
- **Out.** The bytes the OS transmits (MIDI thru, SysEx dumps, the sequencer) are
  parsed into messages on the audio thread after the block and written to the
  block's output `MidiBuffer`. Each message goes at the frame its cycle stamp maps
  to, clamped to the block. Parsing must not allocate (a preallocated SysEx buffer).
  *Implementation note:* a juce::MidiBuffer event holds at most 65,535 bytes, so
  a longer SysEx (such as a wavesample dump) cannot leave the plugin; it is
  dropped and counted in `MIDI dropped`. Such dumps are made from the app.
- **Panic** (Machine menu) does what it does in the app: it flushes pending input
  and sends CC 123 on all 16 channels.
- Program change, pitch bend, aftertouch and CCs reach the plugin as MIDI through
  JUCE's VST3 wrapper. If a host withholds program changes from VST3 instruments,
  that is the host's limit, noted in the guide.

### 4.4 The input jack

The Input Jack bus is the `in` buffer of `RealtimeHost::process` (sampling spec
§9). The bus inactive, or no input channels, means `nullptr` (unplugged). Active
means the average of its channels, times Input Gain. Everything from Plan 4 then
applies unchanged: the mic and line paths, the OS's level detect and threshold,
and hearing the input through filter 8 while the OS samples. The status strip
keeps the Input slider and meter.

### 4.5 The editor

- The app's **`PanelComponent`** unchanged (the artwork is finished), with the
  status strip below it. The strip shows the disk name and "(modified)", the
  input slider and meter, and warnings: illegal ops, MIDI dropped, an unknown ROM.
- A **menu bar inside the editor** (`juce::MenuBarComponent`) replaces the window
  menus:
  - *File*: Open Disk Image..., New Blank Disk, Save Disk Image, Save Disk Image
    As..., Eject Disk, Import WAV...
  - *Machine*: Reset, Panic.

  There is no MIDI or Audio menu, because devices are the DAW's.
- **Resizable**, keeping the panel's aspect ratio. The size is saved with the state.
- **Panel keyboard shortcuts** work while the editor has keyboard focus, as in the
  app. Ctrl shortcuts are best effort, because DAWs keep many keys for themselves.
- All dialogs are **asynchronous** (file choosers, alerts and the Import WAV
  dialog), since plugins cannot run modal loops. The app's dialogs already are.
- The editor can close at any time. The `DiskSession`, its 10 Hz `poll()` timer and
  everything else live in the processor, not the editor.

### 4.6 Disks in a plugin

They work as in the app (`docs/app.md`, menus): Open does not reset, the
unsaved-changes prompt comes before Open, New Blank and Eject, Save writes the
format the file was read in, and a disk that is not fully formatted cannot be
saved.

One difference: **closing the project or removing the plugin does not prompt.**
The disk, with the OS's writes, lives in the project (§5), so nothing is lost. A
disk is "modified" when it differs from its **file**, not from the project. The
status strip keeps saying so after the project reopens, until *Save* writes the
file. Nothing is ever written to the user's image unless they choose Save or
Save As (main spec §3, "never silently overwrite").

Two instances can open the same image file. Each has its own copy in memory, and
the last Save wins. This is documented and not prevented (my call).

### 4.7 The boot ROM

The plugin needs the ROM before it can build a `Machine`. It looks in this order:

1. the ROM path in the plugin's settings;
2. `mirage.bin` beside the `.vst3` bundle;
3. the app's saved ROM path (`MirageEmulator.settings`, read only);
4. the build's `MIRAGE_ROM` (development builds);
5. otherwise the editor shows "Choose the Mirage boot ROM" and a file chooser.
   The plugin outputs silence until a ROM is chosen.

The rules are the app's: exactly 4096 bytes, and an unknown SHA-1 is allowed with
a warning. The ROM bytes are loaded once per process and shared by all instances.
They are **never** written into the project. The snapshot carries only the ROM's
SHA-1, as it does today.

Plugin settings go in `%APPDATA%\MirageEmulator\MiragePlugin.settings`: the ROM
path and the last disk folder. This is a separate file from the app's, so the two
never fight over one file.

## 5. Plugin state (the DAW project)

### 5.1 What is saved

`getStateInformation` writes a `juce::ValueTree` ("MirageState", version 1),
GZIP-compressed:

- `machine`: `Machine::saveState()`, the full snapshot (main spec §6.9). That
  covers CPU, RAM, wave RAM, every chip, the audio path, the jack, the MIDI input
  queue, the panel, and **the inserted disk with its dirty flag**. Not its write
  count: a restored disk counts from 0, so a save that copied the disk before a
  restore must not mark the restored one saved (a mark-saved carries the
  machine's generation, which each restore bumps).
- `disk`: what `DiskSession` knows that the machine does not. That is the file
  path, the format (raw or Giebler), and whether the disk is untitled.
- `volume`, `inputGain`: the two parameters.
- `editor`: width and height.

Raw, a snapshot is about 610 KB (the disk 450 KB, the wave RAM 128 KB). Wave RAM
and a disk's empty sectors compress well. Taking it pauses no audio and allocates
nothing on the audio thread (§5.2). The snapshot format stays at version 5 (§3.1).

### 5.2 Taking and restoring it without touching the audio thread's rules

- **Save.** The UI posts a request carrying a buffer reserved off the audio thread
  (sized from a first snapshot, plus margin). At the start of its next block, the
  audio thread serialises into it without allocating, through a new
  `Machine::saveState(std::vector<uint8_t>& reserved)` that only writes within the
  reserved capacity. It hands the buffer back as the disk copies are handed back
  today. If no block runs within 200 ms (the DAW has stopped audio), the
  processor takes the machine directly. That is safe because `process()` claims
  the machine with a non-blocking atomic flag, and a block that finds it claimed
  outputs silence. That only happens while the audio is stopped anyway.
- **Restore.** `setStateInformation` builds a **new** `Machine` off the audio
  thread, from the ROM plus `loadState`, prepares it at the current block size and
  rate, and posts it. The audio thread swaps it in at a block start and sends the
  old one back to be freed off the audio thread, like the disks. Before the first
  `prepareToPlay` (a project loading) it is installed directly.
- **After a restore** the machine carries on from the snapshot cycle by cycle,
  with no boot. Notes that were held when the project was saved are still held,
  because the DAW's note-off went into the old instance. *Panic* clears them. Being
  exact means the plugin does not send anything on its own (my call).

### 5.3 When a saved machine cannot be restored

These cases are: no ROM found yet; a ROM with a different SHA-1 from the
snapshot's; and a snapshot from a newer version.

- The processor **keeps the saved blob untouched** and hands it back from
  `getStateInformation`, so saving the project does not destroy it.
- The editor says why ("This project's Mirage was saved with boot ROM
  `<sha>`; choose that ROM to restore it"). Choosing the right ROM restores it.
- Meanwhile the plugin outputs silence and runs no machine.
- *Machine → Reset* or *File → Open/New Blank* asks "Discard the Mirage saved
  in this project?" before starting a fresh machine.

## 6. Threading

This is the app's model (`docs/app.md`) with the DAW's threads in place of the
device threads:

| Thread | Does |
|---|---|
| DAW audio (`processBlock`) | Owns the `Machine` through `RealtimeHost`. Runs the block with the host's MIDI events and the jack input, then the fast-forward and the mute (§3), then emits MIDI out. It never allocates, frees, locks or does I/O. |
| Message thread | The editor, menus, `DiskSession` (open, save, poll), Import WAV conversion, the state request and restore (§5.2), the ROM lookup. It talks to the audio thread through the existing UI and wave queues and the new state and machine-swap queues. |

The DAW may call `getStateInformation`/`setStateInformation` from a thread other
than the message thread. The processor runs them on whatever thread calls them,
with the same handshakes. They must never wait on the message thread.

## 7. Testing and acceptance

- **Headless tests** (Catch2, as before):
  - the gate is closed for the whole `$F571` stretch and open from the first note
    after boot, under every OS image in acceptance 4;
  - the gate is not armed for an unknown ROM;
  - fast-forward ends in the same machine state, bit for bit, as a plain run to
    the same cycle;
  - a fast-forwarded factory boot is playable within a stated wall-clock time;
  - block MIDI lands at its frame;
  - long SysEx through the backlog;
  - MIDI out lands at its frame;
  - a state taken mid-boot and restored continues identically;
  - restore with the wrong ROM keeps the blob;
  - nothing allocates on the audio thread.
- **A VST3 smoke test** (ctest): a small JUCE console tool loads the built
  `Mirage.vst3` through JUCE's own VST3 hosting. It checks the buses, processes
  blocks offline with a disk and notes, round-trips the state into a second
  instance, and checks the second renders the same samples.
- **The owner's acceptance 9** (by ear, in their DAW), written up in a new
  `docs/plugin.md`:
  - insert the plugin and open SND-A01: it plays within a couple of seconds,
    with no calibration tones;
  - play from a MIDI track;
  - load a sound from the panel: it is near-instant;
  - save the project, close the DAW and reopen: the edit and the disk are back
    with no boot;
  - bounce offline and compare with the real-time take;
  - use two instances with different disks;
  - sample through the Input Jack sidechain.
- The **standalone's** acceptances 1-8 are untouched. Its tests must pass
  unchanged, because the new host features default to off.

## 8. Left to the builder

- How the app and the plugin share UI code (shared source files or a small
  static library), as long as the app's behaviour and look do not change.
- The status strip's layout in the editor, and the menu bar's look.
- Buffer sizes not fixed above, and the backlog's exact mechanics.
- The VST3 smoke test's structure.
- How the 5/20/10 ms gate timings and the 1/2 block budget are tuned, by ear
  and by measurement.
