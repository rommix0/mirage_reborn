# Clocks and MIDI timing

Evidence is from the boot ROM analysis in `boot-rom.md` (Task 5). All ROM
addresses are in `mirage.bin`, mapped at `$F000`.

## The MIDI bit clock

The ACIA has no crystal of its own. Its transmit and receive clocks come from
VIA port B bit 7, which is the output of VIA timer 1.

| Setting | Value | ROM evidence |
|---|---|---|
| VIA ACR | `$CC` | `$F16B`. ACR7-6 = 11: T1 free-runs with a square wave on PB7. |
| VIA T1 latch | `$0000` | `$F17D-$F186`: T1L-L, T1L-H, T1C-L, T1C-H all cleared. |
| ACIA control | `$95` | `$F152`, at OS hand-off. CR1-0 = 01: divide by 16. CR4-2 = 101: 8 data bits, no parity, 1 stop bit. CR7 = 1: RX IRQ on. CR6-5 = 00: TX IRQ off, RTS low. |
| ACIA reset | `$03` | `$F1A6`, and again at `$F541` / `$F548`. |

A 6522 in T1 free-run mode with latch N toggles PB7 every N+2 Phi2 cycles. With
N = 0 that is every 2 cycles, so one PB7 period is 4 cycles: **PB7 = Phi2 / 4**.

The ACIA divides that by 16, which gives **Phi2 / 64**. For the MIDI rate of
31,250 baud:

    Phi2 = 31,250 x 64 = 2,000,000 Hz

So the VIA's Phi2 (the CPU E clock) is 2 MHz, which confirms
`kCpuClockHz = 2'000'000` in `clocks.h`. No change was needed. MAME clocks its
VIA at 3 MHz, which would give 46,875 baud, so MAME is wrong here.

The same E clock drives the 6809, so both run from the 8 MHz master divided by 4.

## Byte time

A MIDI byte on the wire is 10 bits (start, 8 data, stop):

    10 / 31,250 = 320 us = 640 E cycles at 2 MHz

`kMidiByteSeconds = 10.0 / 31250.0` in `clocks.h`. `Machine` derives its
per-byte cycle count from `kMidiByteSeconds * kCpuClockHz`.

## How the emulator uses it

- The PB7 bit clock is **not** emulated. `Machine` instead delivers one queued
  MIDI byte to the ACIA every 640 E cycles of emulated time, whatever the ACIA's
  divide setting. With the ROM's `$95` setting the two agree.
- The first byte queued onto an idle line arrives one byte time after it is
  queued. Later bytes follow back to back.
- If the OS has not read the previous byte when the next one arrives, the ACIA
  sets OVRN and the new byte is lost, as on hardware. The host-side queue itself
  is unbounded.
- The MIDI interface ties CTS and DCD active, so status bits 2 (DCD) and 3
  (CTS) read 0. After `$03` then `$95` the status register reads `$02` (TDRE).
- The ACIA IRQ output is wired to the CPU **FIRQ** line.
- Transmit is instantaneous: a write to `$E101` calls `Machine::onMidiOut`
  immediately and TDRE stays set. Real hardware would take one byte time per
  transmitted byte; this is a known simplification.
- The test `machine paces 1000 MIDI bytes ...` runs a ROM program that sets up
  the ACIA with `$95`, polls RDRF and stores each byte. All 1,000 bytes (including
  `F8` and `FE`) arrive in order with no OVRN, in 1000 x 640 cycles +/- one byte.

## Checked in Task 9: the OS keeps the boot ROM's settings

The OS (SND-A01) reprograms the VIA timer and the ACIA, but with the same
values, so the MIDI rate stays 31,250 baud and the pacing above holds.

| OS address | Write | Meaning |
|---|---|---|
| `$9F80-$9F8E` | ACR `$CC`; T1L-L, T1L-H, T1C-L, T1C-H cleared | T1 free-run on PB7, latch 0, as the ROM |
| `$A07E-$A085` | ACIA `$03`, then `$95` | master reset, then RX IRQ, 8N1, divide by 16, as the ROM |
| `$A14B-$A14D` | ACIA `$B5` | queueing a byte to send: the same, plus the TX interrupt (CR6-5 = 01) |
| `$A1D3-$A1D5` | ACIA `$95` | transmit queue empty: TX interrupt off again |
| `$F541` (ROM, called from `$9F6B`/`$9F71`) | ACIA `$03` | reset before the re-init above |

The divide bits (CR1-0 = 01) are the same in every value the OS writes, and no
other write to T1 (`$E204-$E207`), the ACR (`$E20B`) or `$E100` was executed.
(T2, `$E208/$E209`, is reloaded by the 400 Hz tick `$F4DF` and `$F52D`; it does
not affect PB7.) Evidence: every PC
executed after `$800E` in a 100 s boot of SND-A01, disassembled and filtered
for `$E1xx`/`$E2xx` operands (indexed accesses through a pointer register
would not show up this way; none of the OS's VIA/ACIA accesses seen use one).
In that run `$9F80` and `$A07E` execute at 98.07 s, after the sound load, and
`$A14B` at 70.27 s, when the OS sends its first 5 MIDI bytes.
