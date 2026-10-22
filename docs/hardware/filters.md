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
voice mix (+ audio input, always silent) --> compressor --> A/D mux input 0
```

- **One CEM3328 per DOC channel** (8 channels = the 8 voices, CA & 7). The DOC volume register
  is the VCA, applied before the filter (the DOC's own multiplying DAC). **Model choice** (the
  controller's ruling); the real board's order of VCA and filter is not visible from code.
- The filters run **per DOC scan** (`Doc5503::onFrame`, 29,411.76 Hz with the 32 oscillators the
  ROM enables), in emulated time, so an A/D conversion sees the analog mix at that instant.
- `Machine::onMix` hands the raw mix to the host once per scan; `Machine::render` turns it into audio (section 8).
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
| ROM `$F0FC-$F106` | `$E400-$E407`←0 (no preset), then `$E418`←`$10` | power-on clear of both CVs; channel 0 takes the DAC's power-on value (section 2, consequence) |
| SP `$86FB-$870C` | `CLR $E418`, then `CLR $E408,X`, `CLR $E410,X` for X = 0..7 | clear |
| SP `$97B8-$97C8` | `$E400+X`←0 for X = 0..7 (no preset), then `$E418`←0 | clear both through function 0; channel 0 keeps the DAC value from before the loop (section 2, consequence) |
| SP `$A974-$A98B` | `$E418`←A, `$E410`←A, `$E418`←A; `$E418`←R, `$E408`←R, `$E418`←R | cutoff A, resonance R |
| MAME `coefficients_w` | address bit 3 = 0 → "VF" (cutoff), bit 4 = 0 → "VQ" (resonance), both 1 → "preload dac" | agrees with the code; MAME's header comment (`e408` cut-off, `e410` resonance) contradicts its own code and the ROM |

**Why the preset (inferred), and the semantics chosen.** If the DAC input latch is clocked at
the end of the write strobe (a rising-edge latch) while the S/H switch is open only during the
strobe, the S/H sees the previous DAC value. Every routine that sets a real CV (ROM `$F5DC`,
`$F5E7`; OS `$8AE4`, `$9742`, `$AE17`; SP `$86FB`, `$A974`) deliberately writes the same value to
`$E418+ch` first. That preset is only necessary if the strobe samples the DAC before the new
value has settled; if a strobe latched the value being written, it would be pointless. The model
therefore latches the **previous** DAC value.

The two clear loops do **not** preset: ROM `$F0FC-$F104` and SP `$97B8-$97C6` write 0 to
`$E400+X` for X = 0..7. With previous-value latching, channels 1-7 are cleared (the preceding
write left the DAC at 0), but channel 0 takes whatever the DAC held before the loop (at power-on,
the DAC's reset value; in SP, for example the last resonance `$A974` wrote). The SP author
evidently meant to clear all 8, which is weak evidence for new-value latching. It is outweighed by
the deliberate presets above, and the consequence is harmless: every note-on path presets and
strobes before it uses a voice, so a stale channel-0 CV never reaches a played note. The model
keeps this behaviour (test "CV decode: a clear loop without preset leaves channel 0 at the
previous DAC value"). S/H droop is not modelled (the OS refreshes the CVs from its envelope code,
and the calibration rewrites them on every pass of its wait loop).

## 3. The DOC A/D and its input mux

### 3.1 The mux (VIA PB3:PB2)

The mux is U45, a CD4052 (dual 4-to-1), AN1 = PB3, AN0 = PB2. Its X half picks the A/D input;
its Y half picks what the audio input jack feeds into filter 8 (voice channel 7, section 3.3).
Schematic *Main Board DSK-8, Analog Section* (1984 rev. N and 1985 rev. G sheets, "Table 1"):

| PB3:PB2 | COM X (to the A/D) | COM Y (to VCF8 `IN+`) | Model (Plan 1-3) |
|---|---|---|---|
| 0 | **compressor**: the voice mix (summer U56 output) through the NE570 U48 | **MIC**: the jack at full level | `compressedAdcCode(voice mix)`: `128 + 127 * clip(mix)`, clip = the tanh approximation of section 6, clamped to 1..255. `$80` = 0 V. Audio in is silent. |
| 1 | **line**: the same voice mix, not compressed | **LINE**: the jack through R51 200K (about 1/11 of the mic level) | `$80` (nothing connected) |
| 2 | pitch wheel | GND (jack disconnected) | `kPitchWheelRest` = `$80` |
| 3 | mod wheel | GND (jack disconnected) | `kModWheelRest` = `$00` |

So "line" and "compressed" are both the **whole voice mix** (all 8 filters summed), differing only
by the compressor; the jack reaches the A/D only through filter 8, and only while the mux is 0 or 1.

Evidence for the numbering: ROM `$F578` sets ORB = `$10` (mux 0) for the calibration; OS `$B935`
sets `$18` (mux 2) for the pitch-wheel centre; OS `$9039-$9058` clears PB2 and sets PB3 (mux 2)
for the pitch wheel and then ORs in `$0C` (mux 3), whose result is shifted to 0-127 like a mod
wheel (`$90A9-$90AC`). The schematic's Table 1 agrees (AN1 AN0 = 1 0 is the pitch wheel). The
scale of the compressed input (gain 1, so a self-oscillating filter at about 0.48 swings
`$4B-$B9`) is a **model choice**.

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

### 3.3 Sampling from the audio input (Plan 4)

Sources: the OS 3.2 disassembly (`resmos32.bin`, `--org 0x8000`; direct page `$80`, so `<$17`
is `$8017`), the analog schematic (section 3.1), and a headless run of SND-A01 with a scratch
probe that traced PCs and mux writes, logged every `$ECE2` conversion and, for the trigger,
substituted a 440 Hz sine for the A/D input while the mux selected the path under test.

**The analog path (schematic, confirmed).** Jack J11 → R53 10K → C72 3.3 µF (AC coupling) →
node clamped by CR9/CR10 to the ±5 V rails → U45 `Y0` (mic) directly, and → R51 200K → `Y1`
(line). `COM Y` → filter 8's (U53, VCF8) `IN+`, with R52 10K to ground. VCF1-7 have `IN+`
grounded; every VCF takes its DOC channel on `IN` (VCF8: AF8, DOC channel 7). All 8 outputs
→ summer U56 → output buffer **and** → `X1` (line, R93 7.5K) **and** → NE570 compressor U48 →
`X0` (R61 6.8K). `COM X` → inverting level shifter (U56, R79 18K) → input sample-and-hold
(U44 4016, C105) → DOC A/D pin. Consequences:

- The sampled signal passes through **filter 8 = voice channel 7's VCF** (the one the OS sets
  up), summed with whatever DOC channel 7 plays. The A/D reads the **whole voice mix**, not
  filter 8 alone.
- Mic vs line gain from the resistors (4052 on-resistance and the CEM3328 `IN+` load
  neglected): mic `R52/(R53+R52)` = 0.5, line `R52/(R53+R51+R52)` = 0.045, ratio **11
  (+21 dB)**. The guide says mic is "about 20 times more sensitive", so its figure probably
  includes the compressor's make-up gain (**inferred**). Absolute levels (jack volts to A/D
  codes) are not on the sheet.
- The jack is fed to filter 8 only while PB3:PB2 is 0 or 1. **OS 3.2 mux timeline** (probe,
  SND-A01): `$F0F9` mux 0 at power-on; `$B937` mux 2 and `$905A` mux 3 at 5.17 s; `$F54B`
  (ROM `$F541`) mux 0 at 5.18 s, held through the lower-sound load; from the main loop
  (12.56 s) `$9042`/`$905A` alternate 2 and 3 every pass, for good. In 5 s of idle play every
  conversion was mux 2 or 3 (990 each). So in normal play the jack is disconnected; it reaches
  filter 8 at mic gain only during boot until the main loop starts, and for one main-loop pass
  after a sampling run (`$9F6F` → `$F541` writes ORB `$30`).
- **OS 3.2 selects mux 1 only at `$9704`**, inside the sampling set-up. Every other ORB write
  either picks 0/2/3 or preserves PB3:PB2 (`$9858`, `$9887`, `$98A3`, the disk code
  `$A509-$A7C0`, ROM `$F4C6/$F4D6`). Mux 1 is never read outside a sampling run.

**Parameters (confirmed, probe: `showParameter`, Up/Down, RAM).** SND-A01 defaults:

| Param | Byte | Stored | Default | Use |
|---|---|---|---|---|
| 73 Sample Time | `<$17` | the value, µs | 34 (`$22`) | VIA T1 latch = 2·(p73 − 1) → period 2·p73 E cycles = p73 µs; 34 → 29.41 kHz |
| 74 Input Filter | `<$18` | 2 × value | 80 (`$A0`) | filter 8 cutoff CV = `<$18` + `[$B283]` + `$5B`, `$FF` if it carries; 80 → `$FB` |
| 75 Line/Mic | `<$19` | 0 = off (**mic**), 1 = on (line) | off | `<$19` = 0 → mux 0, else mux 1 |
| 76 Threshold | `<$1A` | 2 × value | 24 (`$30` = 48) | trigger when \|code − `$80`\| ≥ `<$1A` |
| 77 Multisampling | `<$1B` | 0 = off | off | off: the 8 wavesamples of the half are reset first |

p74 moves the CV in steps of 2, i.e. a semitone with the section 5 mapping (24 steps per
octave), as the guide says ("adjustable in semitone steps"): independent support for that model
choice. The resonance CV comes from `$96FF` = `$18`, a constant in the OS image (nothing writes
it). `$B283` is filter 8's calibration offset (`$B068 + 7 * $4D`, section 4).

**Key sequence and what happens (OS 3.2, addresses and probe times):**

1. **Sample Lower** (key code `$13`; Sample Upper is `$12`): `$949B-$94B7` sets `<$A4` = 0
   (lower; upper sets 1), the display blinks "SL" (`6D 38`) for a 1000-tick countdown
   (`<$34`, `$94C1-$94D0`), **2.53 s**.
2. `$9F60`: interrupts masked (`ORCC #$50`) for the whole run; ROM `$F1BB` halts **all 32 DOC
   oscillators** (control `$63` = halt, IE off; pointers `$10`) and drains the IRQ FIFO; ROM
   `$F541` disables VIA interrupts, resets the ACIA, ORB `$30`. DOC channel sums measured 0 for
   the whole run: channel 7 plays nothing, only the jack drives filter 8.
3. `$9700-$9741`: ORB `$14` (mux 1, motor off); `$AE17` clears all 16 CVs; filter 8 cutoff and
   resonance as above (`$9742`); 8 ms settle (`$F0A7`, Y = 1000); mux 0 if p75 = off (`$9722`);
   `$9858` remembers the two bank values (`<$9B` = mux bits + quarter 0 or 2, `<$9C` = +1);
   VIA T1 latch = 2·(p73 − 1), ACR `$4C` (T1 free-running, **no PB7 output**, so no MIDI clock),
   IER `$C0` (T1 IRQ enabled, masked in the CPU).
4. **Level-detect mode** from **+2.55 s** after the key: loop `$94D8-$94FA`. Each pass
   (about 1.8 ms) `$974E` refreshes filter 8's CVs, does one discarded `$ECE2` read and 20
   counted reads (`$977B`: B = \|code − `$80`\|), then lights one segment on **both** digits
   (port A = code, PA3 = PA4 = 0): **top bar** `01` if ≥ 3 reads were ≥ `$7F` (clip), else
   **middle bar** `40` if ≥ 3 reached the threshold, else **bottom bar** `08` if ≥ 3 were ≥ 8
   (−24 dB), else **the decimal points** `80` (no level). Probe: silent → `80 80`; sine
   amplitude 20 → `08 08`; 60 → `48 48`; 127 → `41 41` (passes alternate, persistence merges
   them). Then port A = `$1F` (column 7) and PA7:PA6: **Enter** (row 1) → record; **Cancel**
   (row 2) → leave (`<$93` = 2).
5. **Enter**: `$9795` (entered within one pass). If p77 = off, `$97B3-$97CF` resets the 8
   wavesamples (start `$00`, end `$FF`, ...). `$986C` takes the current wavesample's start/end
   pages (`$0F,X`); for the defaults: quarter 0 from `$0000`, then quarter 1 up to `$7FF0`.
   The end page is patched into the loop's `CMPX` (`$9847`). **Wave RAM is not touched before
   the trigger** (probe: 0 bytes changed 0.5 s after Enter).
6. **Waiting for the threshold** `$9802-$9817`: an unpaced loop of `$977B` reads (one per 56 E
   cycles, 18,080 reads in 0.51 s) until one read has \|code − `$80`\| ≥ `<$1A`. p76 = 0 triggers
   on the first read. Y counts to 65,535: no trigger in **1.84 s** → return 1 → "nS" (`54 6D`)
   blinks (`$9512`), back to normal play. (The guide says "about six seconds"; the loop at 2 MHz
   gives 1.84 s.) The display is not driven here, so it goes blank.
7. **Recording** `$9819-$9851`: port A = 6 (`$981B`: middle bar on both digits, driven
   **statically** for the whole recording, as the guide says, "the middle bar should go on ...
   and stay on for about two seconds"). Then per sample: `SYNC` (masked T1 IRQ), `LDB $E204`
   (clears T1's flag), `LDB $ECE2`, 0 → 1 (0 is the DOC's end marker), `STB ,X+`. **One `$ECE2`
   read per T1 period**: probe, 65,520 reads all exactly **68 E cycles** apart at p73 = 34. At
   `$8000` the bank switches (`$9837`: ORB = `<$9C`, mux bits kept), X = 0. The triggering read is
   not stored; each stored byte is the conversion the previous read started (section 3.2).
8. **End** `$984B`: 16 zero bytes (`$FFF0-$FFFF` of the half), `$9941` (wavesample
   bookkeeping), return 0 → `$9F6F`: ORB `$30` (mux 0), T2/IER restored, ACR `$CC`, T1 latch 0
   (MIDI clock back), interrupts on; the display becomes steady `6D 71` ("SF" in the probe's
   decoding). Probe, trigger at once: recording **2.228 s** (65,520 × 34 µs), end at Enter +
   2.738 s, display `6D 71` from +2.76 s.

**Where it writes (lower, p77 off):** wave RAM `$00000-$0FFEF` (65,520 bytes, quarters 0 and 1
through the `$0000-$7FFF` window), `$0FFF0-$0FFFF` = 0. Upper (`<$A4` = 1, from the code, not
run): PB1 set, `$10000-$1FFEF`. The probe's capture contains no 0 byte below `$FFF0`.

**What the model needs (Plan 4 Task 2).** The decision table's model is **supported** where it
says the jack enters voice channel 7's filter (so p74 and the calibration offset apply), mux 1 is
line with no compressor, and mux 0 is mic: more gain plus the compressor, with mux 0 still reading
`compressedAdcCode(mix)`. Two details **contradict** it, and the schematic wins:

- **Mux 1 reads the uncompressed voice mix** (all 8 filters), not filter 8 alone. While the OS
  samples, all 32 oscillators are halted, so the mix is filter 8's output plus the other filters'
  input noise: in practice the same number. Corrected: mux 1 = `128 + 127·clamp(mix, −1, 1)`
  rounded, clamped to 1..255. With nothing connected and the voices halted that is `$80`, as the
  current constant gives.
- **The jack feeds filter 8 only while the mux selects 0 or 1** (`COM Y` is grounded for 2 and
  3): jack × `kMicGain` for mux 0, jack × 1 for mux 1, nothing for mux 2/3. Since the OS keeps the
  mux at 2/3 in normal play, a connected jack is not heard while playing; with a silent jack
  nothing changes anywhere.

`kMicGain` = 10 (+20 dB) sits between the resistor ratio (11) and the guide's "about 20 times";
still to tune by ear. Not on the sheet, so still **model choices**: the jack's scale (±1 = line
full scale) and polarity (the summer and the level shifter both invert), and the compressor's curve.

**Not modelled, noted for the display:** port A = 6 drives the middle bars statically, but the
panel model only lights a segment for 35 ms after a port A write (`Panel::portAWritten`), so in
the emulator the display goes blank during the recording instead of showing "- -".

**MASOS** (RAM after boot, `$96C2-$9718`, loop `$9852`): same mux bits (`$14`; `$10` if `<$19` = 0),
filter 8 (`$E41F`, offset `$B2CD`), the same T1 pacing and threshold loop. Additions: with param
93 ≠ 0 (`<$2B`) it reads the external Input Sampling Filter cartridge at `$DC00` instead of
`$ECE2`, and below 28 µs it jumps to unrolled loops (`JMP [B,X]`, `$9836`).
**Soundprocess:** no sampling routine: it never selects mux 1, never sets up T1 or the ACR, and reads
`$ECE2` only for the wheels (mux 2/3, `$97D2-$9844`) and the calibration (mux 0, `$9250`).

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
(These times are from the FDC's first sector layout. With the cumulative layout the OS entry is
at 4.06 s, the last calibration at 5.17 s and the display steady at 12.6 s; boot-rom.md 13.6.)

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
- The audio input (out of scope, spec §3) and the input compressor's real curve; the routing of the sampling input
  through voice 8's filter (section 3.3).
- Whether reading `$E400-$E41F` strobes anything (SP clears CVs with `CLR`, which reads first;
  the model ignores reads).

## 8. Audio output (`Machine::render`, Task 12)

`render(out, frames, hostRate)` runs the machine for exactly `frames / hostRate` seconds (the
fractional E cycles are carried between calls; `runCycles` credits its overshoot, so the long-run
total is exact) and returns mono samples at `hostRate`:

```
voice mix (once per DOC scan) -> DC blocker -> x kOutputGain -> StreamResampler -> host samples
```

- **DC blocker:** y = x - x1 + R y1, R = 1 - 2 pi 5 Hz / scan rate (corner 5 Hz; the coefficient
  follows the scan rate).
- **Gain:** `Machine::kOutputGain` = 0.5, fixed. The mix of one full-volume oscillator through an
  open filter peaks at 1.0 (section 1), so one voice peaks at 0.5 and two coincident full-volume
  voices just reach full scale. More are not limited (`writeWav` clamps). Measured on SND-A01 at
  velocity 100: 0.26 (key 44), 0.12 (key 60).
- **Resampler:** `StreamResampler` (`stream_resampler.*`): Kaiser-windowed sinc (beta 9, 64 taps,
  cutoff 0.94 of the input Nyquist, table of 256 phases per sample, linear interpolation). It
  tracks position in input samples, so a change of the input rate (the scan rate follows the
  oscillator-enable count) only changes the step; it is applied where the new-rate samples start
  in the stream, nothing resets and nothing clicks. When the input is faster than the output the
  kernel is stretched by out/in, which band-limits to the output Nyquist. Group delay 32 input
  samples (about 1.1 ms at 29.4 kHz): the first output samples after power-on are silence. There
  is no interpolation inside the oscillator model.
- **Hand-out:** the resampler produces one output more or fewer than requested from call to call;
  `render` keeps the surplus for the next call and repeats the last sample on a shortfall.
- **State:** resampler, DC blocker, the cycle fraction and the undelivered samples are part of
  `saveState()` (after the filter bank). Mix made by `runCycles()` outside `render()` is not
  resampled.
- **What it sounds like:** the boot calibration (6.26-7.57 s on SND-A01) is audible as eight
  filter self-oscillation tones, RMS 0.13, peak 0.29; the model does not mute the output
  during it. After boot the noise floor is below 1e-3 RMS.
