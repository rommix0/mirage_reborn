# WD1772 floppy controller model

Source of behaviour: `Manuals/wd1772.txt`. How the Mirage drives it: `boot-rom.md`
section 5. Code: `engine/src/wd1772.cpp`, wired in `Machine` (`$E800-$E803`).

## Wiring

| Signal | Mirage connection | Evidence |
|---|---|---|
| INTRQ | CPU NMI (edge) | NMI handler `$F0B0` ends every command (boot-rom.md 5.2) |
| DRQ | CPU IRQ (OR with VIA, DOC) | ROM moves data with `SYNC` while I/F are masked (5.1) |
| Motor / drive select | VIA PB4, **active low** | `$F4C6` clears PB4, `$F4D6` sets it |
| Disk present | VIA PB6, **level**, 1 = disk in | `$F116` tests it before any step |

The 1772's own spin-up is unused: every ROM command sets h=1. Status bit 7 (MO) is the chip's
internal motor line, not the drive motor.

## Timing (E cycles, `clocks.h`)

- Byte time: 250 kbit/s MFM = 32 us = 64 cycles (`kFdcByteCycles`).
- Index: 300 rpm = 200 ms = 400000 cycles (`kDiskIndexCycles`). The index status bit is high for
  the first 2 ms of a revolution.
- Step rates r1r0 = 00/01/10/11: 6/12/2/3 ms. Head settle (E, and V before verify): 15 ms.
- Spin-up (h=0, MO off): 6 index pulses. MO drops after 9 idle revolutions.
- Time-outs (RNF, Seek Error) are 5 index pulses.
- **The disk turns only while the motor is on AND a disk is present.** Anything that waits for
  rotation (spin-up, sector search, data bytes) stalls otherwise. This is the datasheet's index-counted
  time-out taken literally: with no index pulses the chip never gives up.

## Track layout seen by the controller

Six sector slots per revolution, ids 0-5, starting at k/6 of a revolution (even spacing). A slot
starts with the ID field (10 bytes: `A1 A1 A1 FE`, track, side 0, sector, size code, CRC-CCITT of
those eight bytes), then gap 2 (22 bytes), 12 zeros, `A1 A1 A1 FB` (data begins 48 bytes in), the
data, and 2 CRC bytes. Size code 3 = 1024 bytes (ids 0-4), 2 = 512 bytes (id 5), from
`DiskImage::sectorSize`.

A 1024-byte sector needs 1074 byte times but a slot is only 1041 apart, so after reading a big
sector the next slot has already gone by: sequential reads cost about one revolution per sector.
Measured with the real boot ROM and `SND-A01.img`: 22 sectors in 6.25 s of emulated time from power
on (about 0.5 s spin-up, 5 sectors/s). The real format's gaps are not known; this is a model
choice, not a measurement of a real drive.

## Choices where the sources are silent or disagree

- **Track 0 status bit (S2).** `wd1772.txt` says 0 at track 0. The WD datasheet and MAME say 1 at
  track 0, and the transcription looks inverted. The model sets S2 = 1 at track 0. The ROM masks
  Type I status with `$18`, so it never looks at the bit.
- **No disk at command start.** Type II/III (and Type I verify) wait for ever with BUSY set; Force
  Interrupt (`$D0`) frees the chip, and inserting a disk lets the command carry on (a search that had
  nothing to look at starts again). The host never hangs: `tick` is bounded work. A Type I step
  without verify needs no disk and completes.
- **Eject during a command.** `setDisk` aborts a Type II/III command (or a Type I verify) at once
  with RNF / Seek Error and INTRQ. Nothing throws and a write in progress is not committed. The
  chip cannot see the eject; this stands in for its time-out so the ROM (which has no time-outs) can
  go on to "nd".
- **Motor off during a command.** Rotation stops, the command waits (as no disk).
- **Lost Data.** Read: the byte is overwritten, S2 set, the transfer runs to the end. Write: a
  zero byte is written, S2 set. A write whose first byte misses the 22-byte gap ends at once
  with S2 set and writes nothing.
- **Head.** 0-83, stops mechanically. Power-on homes it to track 0 (determinism).
  Restore gives up with Seek Error (V=1) after 255 steps.
- **Read Track / Write Track.** Complete with INTRQ, no data, no error (Plan 3).
- **Force Interrupt.** `$D0` terminates without INTRQ and clears DRQ; `$D8` raises INTRQ until
  `$D0` or the next command (status reads do not clear it); `$D4` raises it at the next index pulse.
  Idle `$Dx` refreshes the status as Type I.
- **Disk change.** `diskChanged()` latches on any insert/eject; it is not PB6.
