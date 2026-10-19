# Ensoniq Mirage Emulator — Design

Date: 2026-10-18
Status: draft for approval (builder check done)

## 1. Intent

Build a Windows program that behaves like a real Ensoniq Mirage DSK-8: you
open an original Mirage disk image, the emulated machine boots whatever
operating system is on that disk, and you play it from a MIDI keyboard. Its
sound and behaviour should match the hardware.

The owner wants the Mirage's character (factory library, editing, the
sound of its 8-bit samples) on a modern computer. They don't own a real Mirage, so fidelity is judged by
ear and against recordings.

The standalone program comes first because it's the easiest way to confirm
the port works. A VST3 plugin wrapping the same engine comes later.

### Why emulate the hardware instead of rewriting an OS

A Mirage disk carries its own OS (Ensoniq OS 3.2, MASOS, SoundProcess,
third-party variants such as `resmos32`). The hardware boots whatever OS is on
the disk. A native rewrite of one OS's behaviour would only ever behave like
that one OS. So we emulate the **hardware**: CPU, memory, floppy controller,
sound chip, I/O chips. The disk's own OS then runs unmodified. Loading,
saving, editing and the sequencer all come from the running OS. The one
deliberate exception is WAV import (§6.7), which replaces live sampling: it
writes converted audio straight into wave RAM, which is hardware, and touches
nothing that belongs to the OS.

Consequence: reverse engineering targets the hardware: the 4 KB boot ROM,
the memory map, I/O chips and how the OS drives them. Disassembling OSes is a
means, not the deliverable. *(Builder-check answer, my call)* Any evidence
counts when working out hardware behaviour:
- the boot ROM;
- disassembly of any OS in the tree;
- MAME's driver;
- published DSK-8 schematics or other outside documentation.

The rule is only that the result models the **hardware**. Where real Mirages
differed between hardware revisions (for example the external AD7574
converter on early units versus the converter inside the rev. D DOC), a
"hardware revision" setting is allowed. It must describe a real unit, never a
per-OS tweak.

## 2. Goals (version 1)

1. **Boots real disks.** At start-up the emulated Mirage powers on with an
   empty drive and waits for a disk, as the hardware does. *File → Open* a
   `.img` then boots from it, with the LED showing what the hardware shows.
2. **Plays like a Mirage over MIDI.** Notes, velocity, pitch bend, mod wheel,
   sustain, and lower/upper keyboard halves, all handled by the emulated OS
   through the emulated MIDI port.
3. **Edit, save, reload.** The panel changes parameters, the OS saves sounds
   to the inserted disk, *File → Save/Save As* writes the `.img`, and
   reopening shows the edit intact.
4. **Samples from WAV files.** *File → Import WAV…* converts a WAV file to
   Mirage 8-bit samples, at a sample rate and between start and end points
   the user chooses, and writes it into wave RAM, where the OS plays and saves
   it like a sample it recorded itself. *(Owner decision; replaces recording
   from audio input.)*
5. **Any OS.** Disks with other OSes (SoundProcess, MASOS, OS 3.2, resmos32)
   boot and work.
6. **Formatting and blank disks.** *File → New Blank Disk* inserts an
   unformatted disk, and the OS's own format function (or the `fmt2` disk in
   `EnsoniqOS/`) formats it, as on hardware. *(Owner decision.)*
7. **EDM support.** Added after `.img` loading is confirmed working (see §6.6).

### Acceptance (how the owner will judge it)

With a MIDI keyboard:
1. Open `Factory Library/IMG/SND-A01.img` → it boots → load a sound from the
   panel → play it. It sounds like a Mirage by ear.
2. Edit a parameter → save the sound to disk → *Save As* a new `.img` → reopen
   → the edit persists.
3. Import a short WAV into the lower half → play it across the keyboard →
   save the sound to disk with the OS → reopen the image → it plays the same.
4. Open `Soundprocess/Soundprocess OS/Soundprocess OS.img` (and the
   `EnsoniqOS/` images) → each boots and plays.
5. *File → New Blank Disk*, boot `EnsoniqOS/fmt2` (or use an OS's format
   command) to format it, save a sound to it, *Save As* → reopen → it loads.
6. After 1–5 pass: open an `.edm` from `MetascapeMetaSounds/` and repeat 1.

## 3. Non-goals and anti-goals

**Not in version 1:**
- The VST3 plugin. It comes later as a second wrapper around the same engine.
- macOS/Linux builds. The toolkit allows them later.
- A circuit-exact model of the CEM3328 filters.
- **Recording from the computer's audio input.** *(Owner decision.)* WAV
  import replaces it. The OS's own sampling functions still run, but the
  converter input reads as silence, as on a Mirage with nothing plugged in.

**Never:**
- **HFE support.** HFE files are low-level flux dumps. The owner has ruled
  this out permanently.
- **Hard-coding any one OS's behaviour.** Nothing in the engine may assume a
  particular OS: no patching OS code, no writing OS parameters or OS RAM, no
  shortcuts that skip the OS (such as synthesising notes outside the emulated
  chips). Anything that only works with one OS is a failure, even if it sounds
  right. WAV import (§6.7) writes wave RAM only, which every OS plays from, so
  it stays within this rule.
- **Silently overwriting the user's disk images** (see §6.5).

## 4. Constraints

- **Platform:** Windows 10 first.
- **Real time:** it must play live from a MIDI keyboard with low enough
  latency to perform. Emulation has to keep up comfortably on an ordinary
  desktop CPU.
- **Inputs available in the tree** (all owned by the user, not to be
  redistributed with the program):
  - `Ensoniq Mirage EPROM Firmware/mirage.bin`: 4 KB 6809 boot ROM, mapped at
    `$F000–$FFFF`. Vectors: RESET `$FC7F`, NMI `$F0B0`, IRQ `$8008`,
    FIRQ `$800B` (IRQ/FIRQ point into RAM, so the OS installs its handlers
    there).
  - Disk images: `Factory Library/IMG/*.img` (30 factory disks),
    `EnsoniqOS/` (os32, masos, fmt2, pianos, rhodes, resmos32), `Soundprocess/`,
    `Minotaur Studios/`, `Sound Cells/` (`.img`), and `.edm` libraries
    (`MetascapeMetaSounds/`, `MUGcollection/`, `KmuseLibrary/`).
  - `datasheets/v4_13_EnsoniqDOC.pdf`: DOC 5503 register spec (text
    extracts cleanly). `datasheets/[www.buchty.net]…5503DOC….html` covers
    errata, the AM/sync clarification, and notes that Mirage looping is
    IRQ-driven. `datasheets/CEM3328.pdf`: filter datasheet (scanned).
  - `Manuals/mirfmt.txt` and the R-Massive layout docs cover the disk layout.
    `Manuals/wd1772.txt` covers the floppy controller. The parameter cards and
    Advanced Sampler's Guide cover parameter meanings for listening tests.
- **Known gaps:**
  - The "service manual" has no schematics; it only covers swapping modules.
  - No manual shows the front panel's button layout.
  - Memory map, I/O addresses, clocking and panel/keypad wiring must come from
    disassembling the boot ROM and from MAME's Mirage driver, which is used as
    a reference.

## 5. Decided by the owner

- **Approach:** hardware emulation that runs the disk's own OS (§1).
- **First deliverable:** a standalone Windows program with MIDI input. The VST3
  comes later.
- **Filter:** liberties allowed. A good-sounding 4-pole resonant low-pass in
  the CEM3328's spirit: soft-saturating resonance that can self-oscillate, and
  the passband volume drop as resonance rises. It's driven by the per-voice
  cutoff and resonance values the OS writes.
- **Window:** a replica of the hardware front panel (design "A"). That means
  the dark case, ensoniq/Mirage branding, red two-digit LED, a drive-activity
  light, and every hardware button in its real position with its real label.
  Menus sit above it: *File*, *Machine*, *MIDI* (input and output devices —
  output added as my call, since the OS's sequencer, MIDI thru and SysEx
  dumps send MIDI), *Audio* (output device).
- **Disk files:** chosen through standard open/save file dialogs. Loading
  and saving sounds otherwise behave exactly as the OS does them.
- **Formats:** `.img` first; `.edm` after `.img` is confirmed; never `.hfe`.
- **Samples:** loaded from WAV files, not recorded. The user chooses the
  conversion sample rate and the start and end points, because a real Mirage
  has limited sample memory. Recording from audio input is left out.
- **Source location:** everything lives under `Ensoniq Mirage/emulator/`.
- **Verification:** by ear. There's no real Mirage to compare against.

## 6. Design

### 6.1 Architecture

Two layers:

- **Engine** (plain C++, no GUI or audio toolkit dependency). The emulated
  machine: 6809 CPU, memory map, boot ROM, RAM, wave RAM, DOC 5503, WD1772 +
  disk image, 6522 VIA, 6850 ACIA (MIDI), keypad/display/LED wiring, filter CV
  path, per-voice filters, output mixing, the input converter (wheels; audio
  input reads as silence), and the WAV-to-Mirage sample converter.
  - **Inputs:** MIDI bytes, panel key presses/releases, disk image
    insert/eject, wave RAM writes from WAV import.
  - **Outputs:** stereo/mono audio frames, display segment state, drive LED,
    MIDI out bytes.
- **Front ends:**
  - **Standalone app (version 1):** JUCE. Window, replica panel, menus,
    device selection, file dialogs, audio/MIDI I/O.
  - **Headless test tool (version 1):** command line. Boots a disk image,
    feeds scripted panel presses and MIDI events (including from a `.mid`
    file), and renders audio to WAV, with optional CPU/IO trace logs. Since
    fidelity is judged by ear, this is how listening comparisons and
    regressions are produced reproducibly.
  - **VST3 (later):** JUCE plugin around the same engine.

### 6.2 Hardware to emulate

The facts below are to be confirmed from the boot ROM and MAME. Items marked
(?) are believed but unverified.

- **CPU:** Motorola 6809, cycle-counted. Clocking (?) comes from the 8 MHz
  master clock shared with the DOC.
- **Memory:** boot ROM at `$F000–$FFFF`; program RAM; 128 KB of wave RAM
  shared with the DOC as two 64 KB halves (lower/upper keyboard), selected by
  the DOC's bank-select bit (?).
- **DOC 5503:** bit-exact to the spec. That covers the 24-bit accumulators,
  16-bit frequency, resolution/table-size address formation, page pointers,
  bank select, free-run/one-shot/sync-AM/swap modes, halting on zero bytes,
  the interrupt FIFO via the OIR, the oscillator-enable register, and output
  sample rate = clock / (8 × (enabled oscillators + 2)). The OS sets the
  oscillator count, so the output rate is whatever the running OS chooses.
  Channel-address bits route each oscillator to a voice's filter. The ADC
  register reads the wheels and the (silent) audio input. Early Mirages had
  an external AD7574 ADC instead; emulate the variant that matches this boot
  ROM.
- **WD1772 floppy controller:** enough command coverage and timing for every
  OS in the tree to read and write sectors, using Mirage disk geometry. That's
  80 tracks, single-sided, five 1024-byte sectors (IDs 0–4) plus one 512-byte
  sector (ID 5) per track, 5,632 bytes/track, 450,560 bytes/image.
- **6522 VIA / 6850 ACIA:** timers and interrupts. MIDI runs at 31,250 baud,
  with bytes paced at real MIDI speed into the ACIA.
- **Front panel:** keypad matrix scanning and the two-digit display/LED drive,
  wired so panel buttons and display behave exactly as the OS expects.
- **Filter/VCA control path:** the DAC and sample-and-hold multiplexer the OS
  uses to set each voice's cutoff and resonance (and anything else on that
  path). Values are captured exactly; the filter that consumes them is the
  liberty in §5.
- **Keyboard and wheels:** the physical 61-key keyboard, wheels and pedal are
  not emulated. Note and controller input comes over MIDI only, through the
  ACIA. That's how a real Mirage is played from an external MIDI keyboard.
  The emulated hardware must read as "no keys held, wheels at rest, pedal up"
  so the OS behaves normally. If an OS ignores some MIDI message, that's
  faithful behaviour, not a bug. *(Builder-check answer, my call.)* Mapping
  MIDI onto emulated physical keys and wheels is a possible later addition,
  not v1.

### 6.3 Audio path

The DOC's per-voice outputs are demultiplexed by channel address into 8 voice
channels. Each goes through its filter, then gets mixed and resampled from
the DOC's native rate to the host device rate. Keep the 8-bit/DOC grit: no
interpolation inside the oscillator model.

Reconstruction and resampling quality and the exact output stage are the
builder's call, within the "sounds like a Mirage" goal.

### 6.4 Timing and real time

Emulate in time slices driven by the audio callback (or a dedicated thread
feeding it). Timestamp incoming MIDI and panel events so they land at the
right emulated time. CPU, DOC, VIA timers and FDC must stay mutually
consistent in emulated time, since the OS relies on DOC interrupts for
looping and on timers for envelopes and LFO.

### 6.5 Disk files

- **Open.** *File → Open Disk Image…* (dialog) **inserts** the image into
  the emulated drive. It does not reset the machine, so the usual Mirage
  workflow works: boot from an OS disk (for example `Soundprocess OS.img`),
  then swap in sound disks. The file is read into memory, and writes from the
  OS go to the in-memory copy. *(Builder-check answer, my call)*
- **Disk changes and reset.** The engine supports changing disks while
  running, including the disk-change and ready signals the WD1772/drive
  expose, and ejecting mid-access behaves as on hardware. There are also
  *File → Eject Disk* and *Machine → Reset* (power-cycle) commands. Reset
  keeps the inserted disk and its unsaved changes.
- **Open dialog filters.** `.img`, plus `.edm` once supported, plus *All
  files*. Many Mirage images have no extension (everything in `EnsoniqOS/`,
  including `resmos32.s`, which is a normal bootable image), so any file of
  exactly 450,560 bytes is accepted whatever its name.
- **Save.** *File → Save Disk Image* writes the in-memory copy back to its
  file. *Save As…* (dialog) writes to a new file.
- **Unsaved changes.** Opening another disk, ejecting or quitting with
  unsaved changes asks first.
- **Blank disks and formatting.** *File → New Blank Disk* inserts an
  unformatted disk. Internally the disk model must represent "unformatted"
  (per track), not just a 450,560-byte buffer. The WD1772 Write Track
  (format) command is emulated: the raw track stream the OS writes is parsed
  into sector IDs and sizes. Tracks formatted in the Mirage layout become
  normal sectors. Anything else is kept as "unformatted / unreadable", so
  reads fail as they would on hardware. A disk can only be saved as `.img`
  once every track is in the Mirage layout; otherwise *Save* explains why.
- **Validation.** Reject files that aren't exactly 450,560 bytes with a clear
  message.

### 6.6 EDM support (after `.img` is confirmed)

The `.edm` files in the tree (Giebler format) are also 450,560 bytes. The
Sound Directory byte sits at the same offset as in `.img` (track 32, sector 5).
So EDM is likely the same raw layout, but this is unconfirmed.

The builder confirms the exact relationship before implementing, by diffing
against `Tools/Ensoniq EDM2IMG…/tools/flopimage_convert.exe` output. If any
header or reordering exists, the builder documents and handles it. Saving an
`.edm` writes back in `.edm` format.

### 6.7 WAV import *(owner decision; replaces sampling from audio input)*

*File → Import WAV…* opens a file dialog, then an import dialog. Clicking
*Import* converts the file and writes it into wave RAM.

- **What the user chooses** *(owner decision)*:
  - **Sample rate** for the conversion. It is limited to the range the Mirage
    itself samples at; the builder confirms the range from the Advanced
    Sampler's Guide, roughly 10–33 kHz.
  - **Start and end points** in the source file. The dialog shows a waveform,
    the selected length in seconds, and how much of the target space that
    length uses at the chosen rate.
- **What I chose** *(my call)*:
  - **Target:** the lower or upper half of wave RAM (64 KB each), plus a
    start page within it (256 bytes per page; default page 0). The dialog
    shows the last page the sample occupies, so the user can set the
    wavesample start and end parameters on the panel to match. Several
    imports into one half at different pages make a multisample by hand.
  - **Too long:** a selection that doesn't fit before the end of the half is
    cut off there, and the dialog says so before importing.
  - **Conversion:** mix down to mono, low-pass and resample to the chosen
    rate, optionally normalise the peak (on by default, because 8 bits leave
    no headroom to waste), then quantise to unsigned 8-bit. Byte `$00` is
    written as `$01`, because a zero byte halts the DOC.
  - **Accepted files:** PCM WAV at 8, 16, 24 or 32 bits, 32-bit float, mono
    or stereo, any sample rate. Anything else is refused with a clear
    message.
  - **Only wave RAM is written.** Bytes outside the imported range are left
    as they were. The import never touches OS RAM or OS parameters, so it
    works the same under every OS.
  - **Not while the drive is busy.** Import is disabled while the drive
    light is on, because the OS loads sounds from disk into wave RAM and the
    two writes would trample each other.
  - **Keeping it:** the imported sample lives in wave RAM like a freshly
    recorded one. It is kept only once the user saves the sound to disk with
    the OS, and is lost at reset like any other unsaved sample.
- **What the OS still decides.** The OS doesn't know an import happened. Its
  wavesample start, end, loop and tuning settings stay as they were, as on a
  real Mirage after sampling, and the user sets them from the panel. Which
  key plays the sample at its original pitch depends on the chosen rate and
  the OS's tuning. The builder measures that relationship for OS 3.2 and
  shows it in the dialog as a hint ("plays at original pitch around C4").
- **Headless tool.** The CLI gets an import option (source file, rate, start
  and end, half, page, emulated time), so imports can be tested and listened
  to without the app.

### 6.8 The window

- **Panel.** The replica panel described in §5.
- **Keyboard shortcuts.** Optional, on top of clicking: digits → keypad,
  ↑/↓ → On-Yes/Off-No, Enter, Esc → Cancel. They mustn't steal keys while a
  file dialog is open.
- **Import dialog.** The WAV import dialog (§6.7) is a separate window, so the
  replica panel stays faithful.

### 6.9 Machine state (for the later VST3)

*(Builder-check answer, my call.)* A plugin must come back exactly as it was
when a DAW project reopens: OS in RAM, loaded sounds, edits, and the
in-memory disk. So from the start, every chip model's state is serialisable,
and emulation is deterministic given the same inputs. That covers the 6809,
RAM, DOC, VIA, ACIA, WD1772 (including in-flight operations), filter control
values and timers.

v1 needn't expose save/restore in the UI. The headless tool should be able to
use it, for example snapshotting after boot to speed up tests.

### 6.10 Tooling (builder's call, defaults proposed)

- **Language and build:** C++20, CMake, JUCE for the app (and later the VST3).
- **Engine code is our own.** MAME's Mirage driver and its 5503/6809/WD1772
  code are references for behaviour, not copied source. That keeps the
  licence our choice and the engine free of GPL obligations.
- **Unit tests:** engine pieces are unit-tested headlessly. Examples include
  CPU instruction tests, DOC address formation and modes against the
  datasheet, and disk geometry round-trips.

## 7. Suggested build order

Each step ends in something checkable:

1. Engine skeleton, `.img` reader/writer, headless tool shell.
2. 6809 core, plus boot ROM disassembly and the derived memory map (written up
   in `emulator/docs/`).
3. WD1772, VIA, ACIA and panel/display wiring, until the boot ROM loads an OS
   from `SND-A01.img` and the headless tool logs the expected display.
4. DOC 5503 and audio out. MIDI notes render to WAV headlessly.
5. Filter CV path, filters, output stage.
6. JUCE standalone app: replica panel, menus, devices, shortcuts. Then live
   play by MIDI keyboard (acceptance 1).
7. Save/Save As end to end (acceptance 2), then New Blank Disk and
   formatting (acceptance 5).
8. WAV import: converter and CLI option first, then the import dialog
   (acceptance 3).
9. Other OSes (acceptance 4).
10. EDM (acceptance 6).

Later: VST3 wrapper.

## 8. Left to the builder

- **Resampling and output.** Resampler choice, output stage and gain staging.
- **Filter model specifics.** Within §5.
- **Emulation scheduling.** Granularity and threading model.
- **Panel art.** Exact layout once labels and positions are confirmed from
  external references, plus the import dialog's look.
- **WAV conversion quality.** The resampling filter and the dither (if any)
  used when converting to 8-bit.
- **Diagnostics.** Logging and trace format, and the test-tool command syntax.
- **Pacing the work.** Whether to pause emulation when the window loses focus
  or audio stops.
