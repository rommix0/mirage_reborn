# Voice filters, the CV path and the DOC A/D

Code: `engine/include/mirage/voice_filter.h`, `engine/src/voice_filter.cpp` (filters, CV
decode, the CV mappings), `engine/src/doc5503.cpp` (A/D conversion), `engine/src/machine.cpp`
(wiring). Tests: `tests/test_voice_filter.cpp`, `tests/test_doc5503.cpp`,
`tests/test_machine_map.cpp`, `tests/test_boot.cpp`.

Sources: the boot ROM (**ROM**), the OS 3.2 RAM dump `EnsoniqOS/resmos32.bin` (**OS**; the OS on
`SND-A01.img` and on `EnsoniqOS/os32` is byte-identical to it at every code address quoted here), `masos`
(**masos**), the Soundprocess OS (**SP**), the CEM3328 and 5503 data sheets, MAME
`enmirage.cpp` (**MAME**, behaviour only). OS images were cut out of the disk images with the
ROM's load layout (boot-rom.md 6) and disassembled with `mirage_cli disasm --org 0x8000`.
Anything not read directly from code or a data sheet is marked **inferred** or **model choice**.

## 1. The analog chain (model)

```
DOC channel n (sum of (byte-128)*volume / 32768) --+-- VoiceFilter n (cutoff CV n, resonance CV n) --+
  + filter input noise (1e-4)                                                                      |
                                              sum of the 8 filter outputs = the voice mix  <-------+
voice mix (+ audio input, silent in Plan 1) --> compressor --> A/D mux input 0
```

- **One CEM3328 per DOC channel** (8 channels = the 8 voices, CA & 7). The DOC volume register
  is the VCA, applied before the filter (the DOC's own multiplying DAC). **Model choice** (the
  controller's ruling); the real board's order of VCA and filter is not visible from code.
- The filters run **per DOC scan** (`Doc5503::onFrame`, 29,411.76 Hz with the 32 oscillators the
  ROM enables), in emulated time, so an A/D conversion sees the analog mix at that instant.
- `Machine::onMix` hands the mix to the host once per scan (Task 12 will resample it).
- **Input noise.** Each filter input gets white noise of amplitude 1e-4 (a few hundred µV on a
  5 V swing; the CEM3328 data sheet gives 200 µV rms output noise wide open). It is what starts a
  self-oscillation when the DOC is silent, as during the calibration (section 4). Deterministic
  (xorshift32, saved).

## 2. `$E400-$E41F`: the CV DAC and the sample-and-holds

One 8-bit DAC feeds 16 sample-and-holds (cutoff and resonance of each voice). For a write to
`$E400 + offset`, channel = offset & 7, and

| Address | A4 A3 | Effect |
|---|---|---|
| `$E400-$E407` | 0 0 | strobe cutoff **and** resonance S/H of the channel |
| `$E408-$E40F` | 0 1 | strobe **resonance** S/H |
| `$E410-$E417` | 1 0 | strobe **cutoff** S/H |
| `$E418-$E41F` | 1 1 | no strobe: DAC preset only |

and every write loads the DAC latch. **A strobed S/H takes the value the DAC held before the
write** (the preset); the DAC then holds the written value. Reads of `$E400-$E41F` return `$FF`
and do nothing.

### Evidence

Every OS and ROM routine that sets a CV writes the value to `$E418+ch` first and then the same
value to the strobe address, and the roles follow from what is written:

| Where | Code | Meaning |
|---|---|---|
| ROM `$F5DC` (from the calibration `$F571`, X = `$E418+ch`) | `,X`←`$FF`; `-$10,X` (`$E408+ch`)←`$FF`; `,X`←B; `-8,X` (`$E410+ch`)←B | `$E408` gets the fixed maximum, `$E410` the value the calibration sweeps: **`$E408` = resonance, `$E410` = cutoff.** A filter is tuned by making it self-oscillate (full resonance) and moving its cutoff. |
| ROM `$F5B1-$F5B5` (end of `$F571`) | `CLRB; JSR $F5DC; STB -$10,X` | leaves cutoff 0 and resonance 0 (the final `$E408` write latches the DAC's 0) |
| ROM `$F5E7`, called by OS `$8AE4` once per voice with U = `$E418+ch` | `,U`←A; `-8,U` (`$E410`)←A; `,U+`←B; `-$11,U` (`$E408`)←B | OS: A = the filter envelope clamped to `$27,X`, B = `$28,X`: cutoff and resonance of the playing voice |
| OS `$AE17` | `$F5E7` with D = 0 for U = `$E418..$E41F` | clears all 16 S/H |
| OS `$9742` (sampling set-up `$9702`) | `,X`←B; `-$10,X`←B; `,X`←A; `-8,X`←A, X = `$E41F` | resonance and cutoff of channel 7 (see section 3.3) |
| ROM `$F0FC-$F106` | `$E400-$E407`←0, then `$E418`←`$10` | power-on clear of both CVs of every channel |
| SP `$86FB-$870C` | `CLR $E418`, then `CLR $E408,X`, `CLR $E410,X` for X = 0..7 | clear |
| SP `$97B8-$97C8` | `$E400+X`←0 for X = 0..7, `$E418`←0 | clear both through function 0 |
| SP `$A974-$A98B` | `$E418`←A, `$E410`←A, `$E418`←A; `$E418`←R, `$E408`←R, `$E418`←R | cutoff A, resonance R |
| MAME `coefficients_w` | address bit 3 = 0 → "VF" (cutoff), bit 4 = 0 → "VQ" (resonance), both 1 → "preload dac" | agrees with the code; MAME's header comment (`e408` cut-off, `e410` resonance) contradicts its own code and the ROM |

**Why the preset (inferred).** If the DAC input latch is clocked at the end of the write strobe
(a rising-edge latch) while the S/H switch is open only during the strobe, the S/H sees the
previous DAC value. That is exactly what the ubiquitous "write `$E418+ch` first" pattern
compensates for. With every observed writer presetting first, latching the old or the new value
gives the same CVs for all known software; the model takes the old value, as the hardware
explanation suggests. S/H droop is not modelled (the OS refreshes the CVs from its envelope code,
and the calibration rewrites them on every pass of its wait loop).

## 3. The DOC A/D and its input mux

### 3.1 The mux (VIA PB3:PB2)

| PB3:PB2 | Input | Model |
|---|---|---|
| 0 | compressed input: audio in mixed with the voice output | `compressedAdcCode(voice mix)`: `128 + 127 * clip(mix)`, clip = the tanh approximation of section 6, clamped to 1..255. `$80` = 0 V. Audio in is silent in Plan 1. |
| 1 | line input | `$80` (nothing connected) |
| 2 | pitch wheel | `kPitchWheelRest` = `$80` |
| 3 | mod wheel | `kModWheelRest` = `$00` |

Evidence for the numbering: ROM `$F578` sets ORB = `$10` (mux 0) for the calibration; OS `$B935`
sets `$18` (mux 2) for the pitch-wheel centre; OS `$9039-$9058` clears PB2 and sets PB3 (mux 2)
for the pitch wheel and then ORs in `$0C` (mux 3), whose result is shifted to 0-127 like a mod
wheel (`$90A9-$90AC`). The scale of the compressed input (gain 1, so a self-oscillating filter
at about 0.48 swings `$4B-$B9`) is a **model choice**.

### 3.2 Conversion protocol

Data sheet (`$E2`): a read **starts** a conversion of 26 clocks, and a read before it completes
loses it. So a read returns the result of the conversion the **previous** read started. The OS
is written for exactly that:

- OS `$B935-$B958` (pitch-wheel centre): mux 2, `LDA $ECE1`, `LDA $ECE2` whose result is thrown
  away (A is overwritten by the `MUL`s), three `MUL`s (33 cycles), then three reads 38 cycles
  apart, each giving the conversion started by the read before.
- OS `$9039-$90A9` (main loop wheels): after each mux change one discarded `LDA $ECE2`, other
  work (a task switch `JSR $885E`, or the pitch arithmetic), then the read that counts. SP `$9767-$97AB` and masos `$BD88` do
  the same.

The model (`Doc5503::read`): the input (`adcRead`) is sampled when a conversion starts; a read
returns the stored result, first taking the pending sample as the result if 26 input clocks
(3.25 µs at 8 MHz) have passed. Power-on result `$80`. **Does it matter?** For the calibration,
no: its reads are 25 µs apart, so each sees the value from one pass earlier, a constant lag that
cancels in a period measurement. For the wheel code it does: without it the first read after a
mux change would return the new input and the OS's dummy reads would be pointless; with it the
dummy read discards the previous channel's value, as the OS intends. "26 cycles" is read as 26
DOC input clocks (**model choice**); 26 µs (26 oscillator slots) is ruled out because the
calibration loop reads every 25 µs and could then never complete a conversion.

### 3.3 Sampling (not used in Plan 1)

OS `$9702-$974D` (sampling) selects mux 1 (`$14`) or, when `<$19` = 0, mux 0 (`$10`), and sets
channel 7's filter: cutoff = `<$18` + `[$B283]` + `$5B`, resonance `[$96FF]`. `$B283` is filter
7's calibration offset (`$B068 + 7 * $4D`, section 4). **Inferred:** while sampling, the input
runs through voice 8's filter as the anti-alias filter. Not modelled until audio input exists.

## 4. The boot calibration (ROM `$F571`)

Called by the OS once per filter with X = `$E418+ch`, B = `$10` (anything else: "O.S." error and
cold boot), U = a per-voice block: OS 3.2 / SND-A01 `$B96E` (U = `$B050 + $4D * ch`), masos
`$BDC0` (U = `$B09A + $4D * ch`), SP `$9630` (U = `$7C59 + $80 * ch`, then adds `$1A` to the
result).

1. ORB = `$10`: mux 0, the compressed input.
2. B = `$B0`. Loop: set resonance `$FF`, cutoff B (`$F5DC`); one wait (`$F5BE`) to synchronise;
   Y = 0; four waits. A wait (`$F5BE`) loops until the A/D reads ≥ `$90`, then until it reads
   ≤ `$70`: one period of the oscillation. Every pass rewrites the CVs (`BSR $F5DC`) and
   increments Y; a pass is 50 E cycles (25 µs); a wait also ends if Y wraps.
3. Y = 0 (no oscillation) or Y ≥ `$FA0` (below 40 Hz): give up. Y ≥ `$85`: B+1 and measure
   again. Y ≤ `$80`: B−1 and measure again. Y in `$81-$84`: done.
4. Store B − `$7A` at `$18,U` (signed). Cutoff 0, resonance 0. OS `$8CB0` adds this byte, sign
   extended, into the cutoff CV of every note on that voice (`LDB $16,Y; SEX; ADDB ...`), so `$7A`
   is the CV the OS expects a nominal filter to need.

**Target frequency.** Four periods = 50 Y + 68 cycles (the call overhead of four `BSR $F5BE`),
so Y = 130.5 means 4 / ((130.5 * 50 + 68) / 2 MHz) = **1213 Hz**, and the accepted window
`$81-$84` is 1199-1227 Hz. The model therefore puts CV `$7A` at 1213 Hz (section 5).

**Convergence.** One CV step changes Y by about 130.5 × (1 − 2^(−1/24)) = 3.7 counts, less than
the 4-count window, so the walk from `$B0` cannot step over the window. (A mapping with fewer
than about 23 steps per octave could oscillate between `$80` and `$85` for ever.)

**Results (all three OSes, and os32):**

| | Filter 0-7 |
|---|---|
| CV B at exit | `7A 7A 7A 7A 7A 7A 7A 7A` |
| Last count Y | `83 82 82 83 82 82 83 83` (SND-A01/os32/masos); SP `83 83 83 82 83 83 82 82` |
| Stored offset (SND-A01, `$B068 + $4D * n`) | `00 00 00 00 00 00 00 00` |

The 8 model filters are identical, so the results are too. Real CEM3328s spread ±1/3 octave
(data sheet: initial frequency 260-470 Hz), which would give offsets of about ±8; no spread is
modelled.

**Timing (SND-A01).** OS entry `$800E` at 6.25 s; calibrations end at 6.390, 6.525, 6.667,
6.810, 6.946, 7.085, 7.223 and 7.358 s (about 0.14 s each: 0.1 s for the oscillation to grow
from the input noise, then about 45 measurements walking B from `$B0` to `$7A`); pitch-wheel
centre `$B935`, then the main loop `$B900` at **7.36 s** (cycle 14,717,124). Before Task 11 this
took until 70.27 s. The display lights at the main loop (7.37 s), goes dark while the OS loads
the lower sound (sectors 23-155, until about 35 s with the FDC model's one sector per
revolution, fdc.md), and is steady at `DB 06` from about 35 s. masos: same times, steady
`DB 06`. SP: same calibration times, steady `3F 06` ("01") at about 34 s, 142 sectors.

**Wheel centre.** `$B935` averages three pitch-wheel conversions into `$900A` as
(sum of 3) / 4, i.e. `$60` for `kPitchWheelRest` = `$80`, and the main loop subtracts it from
the same (sum of 3) / 4 of later readings, with a ±8 dead band. Any rest value is accepted, so
the OS does not disagree with `$80`.

## 5. The CV mappings (`voice_filter.cpp`, the only place they live)

- **Cutoff:** `cutoffHzFromCv(cv) = 1213 * 2^((cv - $7A) / 24)`: exponential (the CEM3328's
  20 mV/octave scale behind a DAC and divider), 24 steps per octave, 35.8 Hz at `$00`, 1213 Hz at
  `$7A`, 20 kHz at `$DB`; clamped to 0.45 × the scan rate (13.2 kHz) in the filter. The 1213 Hz
  anchor comes from the calibration (section 4); 24 steps per octave is a **model choice** that
  covers about 36 Hz-20 kHz and keeps the calibration's convergence margin.
- **Resonance:** `resonanceFromCv(cv) = 1 - (1 - cv/255)^3`, a reverse-audio taper (data sheet:
  finer resolution at high resonance). The filter's feedback is 4.2 × this, so oscillation
  (feedback 4) starts at CV 162.6 = 3.19 V of the 0-5 V range (data sheet: 3.2 V typical).

## 6. The filter (`VoiceFilter`)

A TPT (zero-delay feedback, prewarped) ladder of four one-poles with feedback k = 4.2 × r through
a soft limiter: clip(x) = x(27 + x²)/(27 + 9x²) for |x| < 3, ±1 beyond (monotonic, zero slope at
±3). The limiter is linearised once per sample (k × clip(e)/e × y4, e the linear estimate), so
the loop stays delay-free. The input is scaled by 1 + 0.08 k, so the passband loss at full
resonance is about 11 dB instead of the plain ladder's 14 dB (data sheet: "the usual 12 dB").
Measured at 48 kHz, cutoff 1 kHz, 100 Hz sine: −0.2 dB (r 0), −8.3 dB (r 0.5), −11.2 dB (r 0.9),
−11.1 dB (r 1, oscillating). Self-oscillation at the cutoff from r = 0.952, amplitude about 0.48.
Float state, no allocation; denormals cannot build up in the bank because of the input noise.

**Cost.** Debug build, `mirage_cli run --disk SND-A01.img --seconds 10`: 0.93-0.97 s wall
before Task 11, 1.38-1.45 s after (8 filters × 29.4 kHz, plus the A/D path).

## 7. Not modelled / open

- Per-chip spread of cutoff and resonance (calibration would return different offsets).
- S/H droop and CV feedthrough.
- The audio input and the input compressor's real curve; the routing of the sampling input
  through voice 8's filter (section 3.3).
- Whether reading `$E400-$E41F` strobes anything (SP clears CVs with `CLR`, which reads first;
  the model ignores reads).
