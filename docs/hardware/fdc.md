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
internal motor line, not the drive motor. Every command turns MO on; h=1 only skips the
6-revolution wait. `wd1772.txt` (lines 127-132) is ambiguous here: "turn the motor on" sits inside
the h=0 branch ("Otherwise, if the motor is off ... the chip will turn the motor on and wait 6
revolutions"), so for h=1 it says only that the spin-up sequence is disabled, not whether MO goes
on. The Western Digital flowchart sets MO for every command, and so does MAME
(`wd_fdc_device_base::spinup()`, current master: `status |= S_MON|S_SPIN` for h=1 as well; it is
called whenever a command starts with MO off). The model follows them. Until Plan 3 the
model left MO off for h=1 commands, so it read 0 throughout the ROM's boot; it now reads 1 from
the first command until 9 idle revolutions pass. No boot time moved (Plan 3 Task 7).

## Timing (E cycles, `clocks.h`)

- Byte time: 250 kbit/s MFM = 32 us = 64 cycles (`kFdcByteCycles`).
- Index: 300 rpm = 200 ms = 400000 cycles (`kDiskIndexCycles`). The index status bit is high for
  the first 2 ms of a revolution.
- Step rates r1r0 = 00/01/10/11: 6/12/2/3 ms. Head settle (E, and V before verify): 15 ms.
- Spin-up (h=0, MO off): 6 index pulses. Any command sets MO; it drops after 9 idle revolutions.
- Time-outs (RNF, Seek Error) are 5 index pulses.
- **The disk turns only while the motor is on AND a disk is present.** Anything that waits for
  rotation (spin-up, sector search, data bytes) stalls otherwise. This is the datasheet's index-counted
  time-out taken literally: with no index pulses the chip never gives up.

## Track layout seen by the controller

Six sector slots per revolution, ids 0-5, laid out at **cumulative byte offsets** (one revolution
is 6250 byte times, `kDiskIndexCycles / kFdcByteCycles`). A slot starts with the ID field (10
bytes: `A1 A1 A1 FE`, track, side 0, sector, size code, CRC-CCITT of those eight bytes), then gap
2 (22 bytes), 12 zeros, `A1 A1 A1 FB` (data begins 48 bytes in), the data, and 2 CRC bytes. Size
code 3 = 1024 bytes (ids 0-4), 2 = 512 bytes (id 5), from `DiskImage::sectorSize`.

| Part | Bytes |
|---|---|
| Gap 1, index to the first ID field | 16 |
| Slot, ids 0-4 (10 + 22 + 16 + 1024 + 2) | 1074 |
| Slot, id 5 (10 + 22 + 16 + 512 + 2) | 562 |
| Gap 3, after every sector: (6250 - 16 - 5 x 1074 - 562) / 6 | 50 |
| Gap 4, last sector to the index (the remainder) | 2 |

Slot k's ID field starts at `16 + sum over j < k of (slot(j) + 50)` byte times after the index
pulse (16, 1140, 2264, 3388, 4512, 5636). A 1024-byte sector therefore ends 50 bytes (1.6 ms)
before the next ID field, which is enough for an OS that issues its next Read Sector from the
INTRQ NMI handler (the back-to-back test uses 200 cycles = 3 byte times).

This replaced the first model, which put slot k at k/6 of a revolution (1041 bytes apart). A
1024-byte sector needs 1074 byte times, so the next slot had always just gone by and sequential
reads cost one revolution per sector (5 sectors/s). Now a track of six sectors reads in about
0.99 revolutions (397,000 cycles from the first ID field to the last CRC, with a 200-cycle OS
latency between sectors), about 30 sectors/s; reading all six back to back from an arbitrary
head position takes under two revolutions (`wd1772 reads sectors 0-5 ... back to back`, 760,640
cycles). The OS loads the lower sound of SND-A01 (133 sectors, with track steps) in about 7.4 s
instead of 28 s (boot-rom.md 13.6).

The real Mirage format's gap lengths are not known. The 16-byte gap 1 and 50-byte gap 3 are a
model choice (gap 3 is simply what is left of the 6250-byte track); only the 22-byte gap 2 write
window comes from the controller documentation. None of it is a measurement of a real drive.

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
- **Read Track.** Not modelled: completes with INTRQ, no data, no error.
- **Write Track.** See the next section.
- **Force Interrupt.** `$D0` terminates without INTRQ and clears DRQ; `$D8` raises INTRQ until
  `$D0` or the next command (status reads do not clear it); `$D4` raises it at **every** index
  pulse until the next Force Interrupt or command, and a status read clears each one. The Western
  Digital data sheets list the I2 condition as "every index pulse" (they are not in the asset
  tree; MAME's `wd_fdc` raises INTRQ at each index pulse while the I2 condition is set);
  `wd1772.txt` only says the chip acknowledges "at the next index pulse", which is the first of
  them and does not say it stops there, so the two agree. Until Plan 3 Task 7 the model raised
  one sticky interrupt.
  Idle `$Dx` refreshes the status as Type I.
- **Disk change.** `diskChanged()` latches on any insert/eject; it is not PB6.

## Write Track (format)

`$F0-$FF` (h and E as in Type II; P is ignored). After the command latency (and spin-up / settle
as asked) DRQ goes up at once; the first byte is taken at the next index pulse, then one byte per
byte time until the index pulse after that, which ends the command. Each time a byte is taken,
DRQ asks for the next. A byte the CPU did not load in time is written as `$00` and sets Lost
Data (S2); the command carries on. `$F5` writes `$A1` and presets the CRC, `$F6` writes `$C2`,
`$F7` writes the two CRC bytes and so takes two byte times (the next byte is taken two byte times
later). The datasheet's "no first byte within three byte times ends the command" is not modelled:
an unanswered start just writes zeros.

The controller keeps the bytes the CPU loaded (at most 6250, one revolution) and at the closing
index pulse parses them as follows:

- Find every ID field, `F5 F5 F5 FE t s n z F7`.
- After each ID field, the next address mark must be a data mark, `F5 F5 F5 FB`, followed by the
  sector's bytes (1024 for size code 3, 512 for 2) and `F7`.
- If the ID fields are exactly ids 0-5, once each, all with t = the head's track, s = 0 and size
  codes 3, 3, 3, 3, 3, 2, the track becomes **formatted** holding the written data
  (`DiskImage::setTrack`, sector order as in the raw image). Anything else makes it
  **unformatted** (`DiskImage::unformatTrack`): a non-Mirage layout reads as unreadable, as it
  would on the Mirage. Either way the disk is dirty and counts one write.

Gaps, sync bytes and the exact byte positions are not kept: the track reads back with the fixed
layout above. A head beyond track 79 writes nothing. No disk: the command waits for rotation, as
Type II. Eject during the command aborts it with RNF and leaves the track as it was. A snapshot
holds the bytes loaded so far (machine snapshot version 3 and later).

## Disk file formats

`DiskImage` (`engine/src/disk_image.cpp`, `engine/src/edm_format.cpp`) reads two file formats
and tells them apart by content, never by extension (`detectDiskFormat`):

1. **Exactly 450,560 bytes: a raw image**, whatever the name. Track 0 sector 0 first, each track
   sectors 0-4 (1024 bytes) then sector 5 (512), 5632 bytes per track. This covers `.img`, the
   extensionless `EnsoniqOS/` files and the 450,560-byte `.edm` files (Metascape, MUG, K-Muse,
   the Triton `SoundProcess_Vol*.edm`, which the Triton readme calls raw disk rips).
2. **Starts with `0D 0A "Mirage Disk"`: a Giebler .edm file** (any other size).

Anything else is refused with `<name>: not a Mirage disk image (expected 450,560 bytes or a
Giebler .edm file)`. A Giebler file is never 450,560 bytes (512 + 1024k + 1), so the size test
comes first. `loadFile` does not read files over 1 MiB (the largest Giebler file is 492,033
bytes).

### The Giebler .edm layout

Decoded from the seven Triton pairs in `Soundprocess/TritonSoundprocess/` (`SP1.EDM`-`SP6.EDM`
with `SoundProcess_Vol1.edm`-`Vol6.edm`, `SNDPROCS.EDM` with `Triton - Soundprocess OS.edm`) and
checked against the three EDM2IMG v0.3 samples (`M_10.EDM`-`M_12.EDM`, kept in
`(xtras)/DaDash Collection/`). It is Gary Giebler's `.EDE` disk layout (`Tools/Ensoniq EFE File Format (Steve Quartly
& Gary Giebler).txt`, which describes it for the EPS) applied to the Mirage's sectors.

| Offset | Contents |
|---|---|
| `$000-$00C` | `0D 0A` "Mirage Disk" |
| `$00D-$04D` | spaces |
| `$04E-$04F` | `0D 0A` |
| `$050-$09C` | spaces |
| `$09D-$09F` | `0D 0A 1A` (CR, LF, DOS end of file: `type` shows one line) |
| `$0A0-$0DB` | skip bitmap: 480 bits, one per sector, MSB first, per track in the order 5, 0, 1, 2, 3, 4. **1 = left out** |
| `$0DC-$1FE` | zero (the `.EDE` text puts a compression flag at `$1FD`; always 0 here) |
| `$1FF` | `$01`, the disk type (the `.EDE` text gives `$03` for an EPS disk) |
| `$200-` | the stored sectors in bitmap order, **each in 1024 bytes**: sector 5's 512 bytes are followed by 512 zeros |
| last | one `$1A` |

So a file is 512 + 1024 x (stored sectors) + 1 bytes: 389,633 for the Triton files (100 sectors
left out), 457,217 for `M_10` (34), 400,897 for `M_11`/`M_12` (89). The `512k + 1` excess over a
reading with 512-byte sector 5 slots is the sector-5 padding plus the end byte.

**Which sectors are left out:** exactly the all-zero ones. In every Triton pair each left-out
sector is all zero in the raw rip and no stored sector is all zero; in the M files no stored
sector is all zero either. The decoder fills left-out sectors with zeros, and every Triton pair
decodes to its raw rip byte for byte.

**Writer** (`DiskImage::toEdmBytes`, the same rule): leave out each all-zero sector, store the
others padded to 1024 bytes, end with `$1A`. Re-encoding a decoded file gives the original byte
for byte for all seven Triton files and `M_10`/`M_12`. `M_11.EDM` differs only in its last byte,
`$98` instead of `$1A` (a stray byte from whatever wrote it); the reader ignores everything after
the stored sectors. Decoding and re-encoding is lossless for any disk, since a left-out sector is
exactly a zero one.

**The owner's `flopimage_convert.exe -IMG`** (spec 6.6; run on copies of `M_10`-`M_12` and
`SP1.EDM` in a scratch folder) agrees on every stored sector but fills left-out sectors with the
EPS "unused" pattern `6D B6` instead of zeros: its `SP1` output differs from the Triton raw rip in
exactly the 100 left-out sectors. The sample `M_13.img` next to the M files carries the same
`6D B6` fill (boot-rom.md 13.2). We follow the raw rips: zeros. `SNDPROCS.EDM` boots and settles
exactly like `Soundprocess OS.img` (acceptance 6), which is byte for byte `Triton - Soundprocess
OS.edm`, zeros included.

**Damaged files** are refused, with "damaged Giebler .edm file" in the message: a header shorter
than 512 bytes, or fewer bytes than the bitmap's stored sectors need (a file cut inside a
sector, or a bitmap that lists more sectors than the file holds). The decoder checks the size
before copying anything.

### Saving

*Save* writes the format the file was read in; *Save As* writes Giebler for a name ending in
`.edm` (any case) and a raw image otherwise (`DiskSession`, `docs/app.md`). Both go through the
atomic `DiskImage::saveFile(path, format)`, and both refuse a disk with an unformatted track
before anything is written.
