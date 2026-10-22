# Sampling from Audio Input — Design

Date: 2026-10-22
Status: approved by the owner (2026-10-22)
Extends: `docs/specs/2026-10-18-mirage-emulator-design.md` (the main spec)

## 1. Intent

The Mirage was a sampler first. On the hardware you press *Sample Upper* or
*Sample Lower*, the OS shows the input level on the LED display, and pressing
*Enter* records from the rear-panel input jack into wave RAM, at the rate set
by parameter 73. The owner wants that workflow in the emulator: plug a sound
into the computer, sample it with the Mirage's own OS, edit it and save it to
disk, as on a real unit.

This reverses an earlier owner decision (main spec §3, "Recording from the
computer's audio input" is a non-goal; §5 "WAV import replaces sampling").
*(Owner decision, 2026-10-22.)* WAV import (§6.7) stays as it is. The two
coexist: import writes wave RAM directly, sampling goes through the OS.

### Why this fits the emulation approach

Sampling needs no OS-specific code. The OS's own sampling routine selects the
A/D input, sets the anti-alias filter, waits for the threshold, reads the DOC's
A/D register at its sample rate and writes the bytes into wave RAM. The
emulator already runs all of that. The only missing piece is hardware: the
analog input stage, which today reads "nothing connected"
(`kLineInputIdle`, `docs/hardware/filters.md` §3). Giving the emulated input
jack a signal makes sampling work under every OS that samples (OS 3.2, MASOS,
SoundProcess) without the engine knowing which one is running.

## 2. Goals

1. **The emulated input jack carries a signal.** In the app it comes from an
   audio input device. In the CLI (for tests and reproducible listening) it
   comes from a WAV file at a given emulated time. WAV files stay an *import*
   feature in the app (main spec §6.7): there is no WAV player feeding the
   jack *(owner decision, 2026-10-22)*.
2. **The OS's sampling works as on hardware.** Level-detect mode on the LED
   display, the sampling threshold (param 76), the sample rate (param 73), the
   input filter (param 74), the line/mic switch (param 75), and the result
   written into wave RAM by the OS, then saved to disk with the OS.
3. **Timing is exact.** The input is placed in emulated time, sample for
   sample, the same way the audio output is. No added jitter; at most one audio
   block of latency.
4. **Deterministic and testable.** Given the same input file and the same
   panel/MIDI events, a headless run produces the same wave RAM bytes every
   time.

### Acceptance (by ear, with the owner's audio interface)

7. Boot `SND-A01.img`. Play a sound into the computer's audio input. Press
   *Sample Lower*, watch the level on the display, press *Enter*, play the
   sound. Play the lower keyboard: it is the sampled sound, at the pitch the
   OS gives it. Save the sound with the OS, *Save As*, reopen and Reset: it
   plays the same.
8. The line/mic switch (param 75) audibly changes the input path: mic adds
   gain and compression, line does not.

## 3. Non-goals and anti-goals

**Not in this design:**
- **The MASOS external input-filter cartridge** (param 93, the optional 7-pole
  filter and its own A/D). Its input reads "nothing connected". MASOS samples
  through the internal path when param 93 is 0, as the guide requires anyway.
- **Monitoring the input through the Mirage's audio output** beyond whatever
  the emulated analog path already mixes (see §6.3). The computer's own
  monitoring is the user's business.
- **Stereo sampling.** The Mirage samples mono. The host mixes or picks a
  channel before the jack.
- **A circuit-exact model of the mic preamp and compressor.** As with the
  filters, liberties are allowed within "sounds and behaves like a Mirage".

**Never (unchanged from the main spec):**
- No OS-specific code paths, no patching OS code, no writing OS RAM or OS
  parameters. The host feeds the input jack; the OS does the sampling.
- Recording never stops the emulation or speeds it up: realistic timing.

## 4. Constraints

- Everything in the main spec §4 and the Plan 2/3 global constraints still
  holds: JUCE only in `app/`; engine and `mirage_host` JUCE-free; after
  `prepare()` the audio thread never allocates, frees, locks or does I/O;
  snapshots exact and versioned.
- **Evidence for the analog path** (main spec §1: any evidence counts):
  - Advanced Sampler's Guide (OCR'd, PDF pp. 22-26): param 73 sample time
    (rate = 1,000,000 / time µs), param 74 *Input Filter Frequency* ("the
    sampling input passes through an internal 4-pole filter... 24 dB per
    octave low-pass... adjustable in semitone steps", Appendix A lists the
    cutoffs), param 75 *Line/Mic*: "two input paths which both use the same
    audio input jack... The Mic level setting (off) increases gain and also
    puts a compressor on the signal... The Line level setting (on)... the
    compressor is not in the circuit... The Mirage does not have a variable
    input level control", param 76 *Sampling Threshold* (level-detect mode
    lights the centre LED bar when the signal crosses it).
  - `docs/hardware/filters.md` §3: the A/D mux on VIA PB3:PB2. OS 3.2's
    sampling set-up (`$9702-$974D`) selects mux 1 (`$14`) or, when `<$19` = 0,
    mux 0 (`$10`), and sets voice channel 7's filter (cutoff from `<$18` plus
    filter 7's calibration offset, resonance from `$96FF`). `<$19` is very
    likely param 75 and `<$18` param 74 (to be confirmed).
  - The existing mux-0 model is "compressed input": the voice mix through the
    compressor, which the boot calibration relies on.

## 5. Decided by the owner

- Sampling from audio input is in scope (2026-10-22), alongside WAV import.
- Skip further questions; the builder makes the remaining calls (marked
  *my call* below) and records them.

## 6. Design

### 6.1 Engine: the input jack

`Machine` gains an audio input, fed per render block:

- `render(float* out, size_t frames, double hostRate)` gains an overload
  `render(const float* in, float* out, size_t frames, double hostRate)`.
  `in` holds `frames` mono samples at `hostRate`, in volts-like units where
  ±1.0 is a hot line level *(my call)*. `nullptr` means nothing is plugged in.
  Because `render` runs exactly `frames / hostRate` seconds of emulated time,
  input frame *k* belongs to the emulated instant `blockStart + k / hostRate`:
  the input is placed sample-exact with no extra latency.
- Inside, the input goes through a second `StreamResampler` (host rate → the
  DOC scan rate, which the OS sets and can change) so the analog chain sees one
  input sample per DOC scan, exactly where the filters already run. Real-time
  rules as for the output resampler (reserved in `prepare()`).
- `runCycles` (no render) sees silence at the jack, as today. The CLI drives
  input through `render`.
- The input resampler's state is saved in snapshots (snapshot version bump),
  so a restored machine fed the same input gives the same bytes.

### 6.2 Engine: the analog input stage (`filters.md` §3)

A model of the two paths the guide describes, both fed from the same jack:

- **Anti-alias filter.** The jack signal enters voice channel 7's filter input
  (the filter the OS sets up for sampling), added to whatever DOC channel 7
  plays. So param 74 and the calibration offset the OS writes act on the input
  for free.
- **Line path (mux 1).** Filter 7's output, unity gain, straight to the A/D:
  `128 + 127 · clamp(x)`, clamped to 1..255. A hot input clips hard, as the
  guide warns.
- **Mic path (mux 0).** The jack signal gets extra gain (*my call*: +20 dB, to
  tune by ear) before it joins the voice mix, and the existing compressor
  (`compressedAdcCode`) shapes it. The boot calibration, which also reads mux
  0 with nothing plugged in, is unchanged: with a silent jack mux 0 reads
  exactly what it reads today.
- The exact topology above is a model choice. The builder confirms what it
  can (the OS's sampling routine: which mux for which param 75 value, which
  filter, when it reads `$ECE2`) by disassembly and records the evidence and
  the remaining inferences in `filters.md` §3.3. If the evidence contradicts
  the model, the evidence wins and the design is updated.

### 6.3 What the user hears while sampling

Whatever the emulated analog chain puts on the audio output: filter 7's output
is part of the voice mix, so the input is audible through channel 7 while the
OS has it routed there. This is a consequence of the model, not a feature; if
it sounds wrong on the hardware evidence, it changes with §6.2.

### 6.4 Host: `RealtimeHost`

- `process(const float* in, float* out, size_t frames, double hostRate,
  double nowSeconds)`: the input block from the same device callback is passed
  straight to `Machine::render`. A null `in` is silence. No queue is needed:
  input and output are the same block of the same callback.
- A host-side **input gain** (0 to 4, default 1) is applied before the jack,
  like the output volume slider: it stands in for the level knob of the mixer
  or preamp the guide tells you to use, not for anything inside the Mirage.
- `HostStatus` gains an **input peak** (last block, host side, before the
  jack) so the UI can show that a signal is arriving.

### 6.5 The app

- *Audio → Audio Settings...* also chooses the **input device** and its
  channels (JUCE's device selector already supports inputs). Mono jack: one
  chosen channel, or the mix of a stereo pair.
- No input device chosen (or none available) means nothing is plugged into the
  jack.
- An **input level** slider and a small **input peak meter** next to the
  volume slider. *(Panel artwork is finished: the builder places these in the
  status strip or a separate area, never on the replica panel itself.)*
- Microphone permission: Windows may ask the first time an input device is
  opened. A refused or missing input is silence, never a crash.

### 6.6 The CLI

`mirage_cli run --audio-in "FILE@T[,gain=G][,loop=0|1]"`: plays the WAV into
the jack from emulated time T (repeatable; later ones replace earlier ones from
their T). Requires `--wav` or forces render mode internally so input is
placed through `render`.

### 6.7 Tests

- Unit: input resampling places frame *k* at the right emulated cycle; a
  silent jack leaves every existing mux reading bit-identical (the boot
  calibration and all boot timings unchanged); mux 1 reads a DC input as the
  expected code; mic gain + compressor bound the code to 1..255; snapshot
  round-trip mid-input.
- End to end (`[assets][acceptance]`): boot `SND-A01`, press the OS 3.2
  sampling keys (*Sample Lower*, *Enter*) with param 75 on (line), feed a
  440 Hz sine through `--audio-in`-style input. Then:
  - wave RAM's lower half holds a sine whose period in bytes is
    rate / 440 (rate from param 73's default, 29.4 kHz → about 66.8 bytes);
  - a played note has the expected relative pitch (octave test as in
    acceptance 3);
  - with the jack silent, the threshold (param 76 > 0) never triggers.
- Real-time: the allocation-counting test covers `process()` with an input
  buffer.

## 7. Suggested build order

1. Disassemble OS 3.2's sampling routine far enough to confirm the mux,
   filter, param 74/75/76 roles and the A/D read loop; record in `filters.md`.
2. Engine: input resampler, jack into filter 7, line and mic paths, snapshot
   bump; unit tests.
3. CLI `--audio-in`; the end-to-end sampling test.
4. Host: `process` with input, input gain, input peak.
5. App: input device, level slider and meter.
6. Docs: `app.md` guide and acceptance 7-8 checklists; main spec §2/§3/§5
   updated to point here.

## 8. Left to the builder

- The mic-path gain and how the compressor treats the input (by ear).
- The input level units (what ±1.0 means) and the default host input gain.
- Where the input slider and meter sit (not on the replica panel).
- Whether MASOS and SoundProcess sampling get their own end-to-end tests
  (recommended if their routines differ from OS 3.2's).

## 9. Later: the VST3

A plugin gets the jack for free: the plugin's audio input bus (or a sidechain)
is the `in` buffer to `process()`. Nothing in this design blocks that.
