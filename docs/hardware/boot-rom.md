# Mirage boot ROM findings

ROM: `Ensoniq Mirage EPROM Firmware/mirage.bin`, 4096 bytes, SHA1
`ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d`, mapped at `$F000-$FFFF`.

Every claim below cites the ROM address it comes from. To regenerate the
listing:

```
mirage_cli disasm "Ensoniq Mirage EPROM Firmware/mirage.bin" --from 0xF000 --to 0xF614
```

A linear sweep is only meaningful over the code ranges in the ROM map; the
rest is data or `$FF` fill.

Cross-checks outside the ROM:

- The OS image on the factory disk `Factory Library/IMG/SND-A01.img` (tracks
  0-1, then sector 5 of tracks 2-10, laid out as RAM `$8000-$BDFF`).
- The RAM dump `EnsoniqOS/resmos32.bin`.
- MAME `enmirage.cpp`.

These are marked **(OS)**, **(dump)** and **(MAME)**. Anything that comes
from reasoning rather than a direct read is marked **inferred**.

## 1. ROM map

| Range | Contents |
|---|---|
| `$F000-$F0AF` | FDC command primitives and a delay loop (§5.1) |
| `$F0B0-$F0EF` | NMI handler, the FDC command-completion handler (§5.2) |
| `$F0F0-$F15C` | Cold boot main line (§3) |
| `$F15D-$F20C` | Hardware init, DOC reset, sample-RAM fill (§3, §4) |
| `$F20D-$F305` | OS loader and config loader (§6) |
| `$F306-$F38B` | Error-code display and the 7-segment scan routine (§8) |
| `$F38C-$F4DE` | Loader helpers, retry wrappers, motor on/off (§5.3, §6) |
| `$F4DF-$F614` | Routines that only the OS calls (§9) |
| `$F615-$F6DC` | `$FF` fill (unused) |
| `$F6DD-$F6F3` | An OS-called routine (§9) |
| `$F6F4-$F77C` | `$FF` fill (unused) |
| `$F77D-$FC7E` | Data tables (§10). These are not code; the linear disassembly there is meaningless. |
| `$FC7F-$FC90` | Reset entry, with an expansion-cartridge hook (§3) |
| `$FC91-$FCB6` | "O.S." error display, then a cold restart (§8) |
| `$FCB7-$FFEF` | `$FF` fill (unused) |
| `$FFF0-$FFFF` | Vectors (§7) |

## 2. I/O register accesses

The ROM sets DP=`$E8` at `$F15D-$F15F` and keeps it for the whole boot path.
So the direct-page operands `<$00..<$03` in `$F000-$F0B6` and at `$F146` are
the WD1772 at `$E800-$E803`.

| Address | Device register | ROM accesses (address: value, meaning) |
|---|---|---|
| `$E100` | ACIA control | `$F1A6`: `$03` master reset. `$F152`: `$95` at OS hand-off: RX IRQ enabled (CR7), TX IRQ off and RTS low (CR6-5=00), 8N1 (CR4-2=101), ÷16 clock (CR1-0=01). `$F548` (OS-called): `$03` reset. |
| `$E101` | ACIA data | Never touched by the ROM. |
| `$E200` | VIA ORB | `$F0F9`: `$10`. `$F1EB/$F1EE`: OR in the sample-RAM bank. `$F209`: `$10`. `$F17A`: `$30`. `$F118`: read and AND with `$40` (PB6). `$F4C6-$F4CB`: clear PB4. `$F4D6-$F4DB`: set PB4. `$F157`: `$30` at hand-off. `$F54B/$F552` and `$F578` (OS-called): `$00`, `$30`, `$10`. |
| `$E201` | VIA ORA | `$F373`: display scan (segment number in PA2-0, digit enables PA3/PA4). The ROM never reads port A, so it never scans the keypad. |
| `$E202` | VIA DDRB | `$F0F4`: `$1F`. PB0-4 are outputs, PB5-7 inputs. |
| `$E203` | VIA DDRA | `$F175`: `$1F`. PA0-4 are outputs, PA5-7 inputs (key rows). |
| `$E204/$E205` | VIA T1C-L/H | `$F183/$F186`: both cleared. Writing T1C-H starts T1 with latch `$0000`. |
| `$E206/$E207` | VIA T1L-L/H | `$F17D/$F180`: both cleared. |
| `$E208/$E209` | VIA T2C-L/H | `$F18C/$F18F`: `$88/$13`, which is 5000 cycles, one-shot. `$F4E2` and `$F530/$F533` (OS-called): reload. |
| `$E20B` | VIA ACR | `$F16B`: `$CC`. T1 runs free with a square wave on PB7 (ACR7-6=11). T2 is a one-shot interval timer (ACR5=0). The shift register shifts in on the external CB1 clock (ACR4-2=011). No latching. |
| `$E20C` | VIA PCR | `$F170`: `$0E`. CA1 triggers on the negative edge. CA2 is a manual output held high (PCR3-1=111). CB1 triggers on the negative edge. CB2 is an input on the negative edge. |
| `$E20E` | VIA IER | `$F166`: `$7F`, all VIA interrupts off during boot. `$F14D`: `$A6` at OS hand-off, enabling T2, SR and CA1. `$F538`: `$A6` and `$F543`: `$7F` (OS-called). |
| `$E20F` | VIA ORA, no handshake | `$F557` (OS-called): `$18`, both digits off. |
| `$E400-$E407` | Filter CV, function 0: strobe cutoff and resonance S/H (filters.md 2) | `$F0FC-$F104`: all 8 cleared. |
| `$E418` | Filter CV, function 3, channel 0: DAC preset (filters.md 2) | `$F106`: `$10`. `$F5DC` (inside the OS-called `$F571`, with X=`$E418` set by the OS at `$B971`): writes `,X`, `-$10,X` (=`$E408`) and `-8,X` (=`$E410`). |
| `$E800` | WD1772 command/status | Commands in §5.1. Status is read at `$F096` (busy poll) and `$F0B2` (NMI handler). |
| `$E801` | WD1772 track | `$F146`: read, copied to `$8002` at hand-off. |
| `$E802` | WD1772 sector | `$F003/$F016/$F027/$F03A`: written from `$8003`. |
| `$E803` | WD1772 data | Read in loops at `$F00D` and `$F020`; written at `$F033` and `$F046`. `$F072`: seek target (from `$8002`). |
| `$EC00-$ECDF` | DOC oscillator registers | `$F197-$F19F`: all cleared. `$F1BB-$F1C5` (also OS-called): `$80-$9F` (wavetable pointer) set to `$10`. `$F1C7-$F1D1`: `$A0-$BF` (control) set to `$63` (halt=1, mode bits=01, IE=0, channel nibble 6). |
| `$ECE0` | DOC interrupt status | `$F1D3-$F1E2`: read 32 times, 192 cycles apart, to flush pending oscillator interrupts. |
| `$ECE1` | DOC oscillator enable | `$F192`: `$FF` (all 32 oscillators). |
| `$ECE2` | DOC A/D | `$F5BE/$F5C7/$F5D4` (OS-called filter calibration; §9). |

No other I/O address is accessed. The bytes at `$F8D5` (`$EEEC`) and
`$FB53` (`$E0FE`) look like I/O operands in the listing, but they are inside
data tables.

### 2.1 What the accesses imply about port bits

- **PB1:PB0 select the sample-RAM quarter.** `$F1E5-$F20C` ORs `0..3` into
  ORB and fills `$0000-$7FFF` with `$7F` for each value. This agrees with the
  table.
- **PB4 is active low (0 = drive selected and motor on).** `$F4C6` clears PB4,
  then waits `Y=$FFFF`×16 cycles (about 0.52 s at 2 MHz) for spin-up.
  `$F4D6` sets PB4 again after loading and on every error path. The final ORB
  of `$30` has PB4=1, so the motor is off at hand-off. MAME's
  `mon_w(data & 0x10)` agrees, because MAME's `mon` is active low.
- **PB6 = 1 means a disk is present.** `$F116-$F11D` tests PB6 after spin-up.
  When it is 0, the ROM goes to `$F131`: motor off, error code 7, display
  "nd" (no disk), then retry. No step pulse comes before this test, at power-on
  or on any retry pass (`$F110` → `$F15D` → `$F4C6` → `$F116`). So PB6 cannot
  be a PC-style disk-change latch that clears on a step, or the machine could
  never boot. **Inferred:** PB6 is a level "disk in drive" signal.
- **PB2-3 set the ADC mux.** The ROM writes `$10` (mux 0, the compressed
  input) before the OS-called filter calibration that reads the DOC A/D
  (`$F578`). This agrees with MAME, which feeds fake data on mux 0 to get past
  that calibration.
- **PB7 is the ACIA clock.** `$F16B` (ACR=`$CC`) puts T1 in free-run mode with
  PB7 output, and `$F17D-$F186` makes the latch 0. On a 6522, free-run mode
  with latch N toggles PB7 every N+2 Φ2 cycles, so PB7 = Φ2/4. The ACIA is set
  to ÷16 (`$F152`), so the MIDI rate is Φ2/64. That is 31,250 baud only if
  **Φ2 = 2 MHz**. This supports the 2 MHz E clock in `clocks.h`. MAME clocks
  its VIA at 3 MHz, which would give 46,875 baud. **Inferred:** the VIA's Φ2
  is the CPU E clock. Task 7 verifies the timing.
- **PA3 enables the left digit and PA4 the right digit, both active low. This
  contradicts MAME and the plan, and the ROM wins.** See §8.
- **PA2-0 select the segment, with 0..7 = a, b, c, d, e, f, g, dp.**
  `$F33C-$F38B` sends segment number B=0..7 to PA2-0, shifting the pattern
  MSB first. The font at `$FB4D` decodes to correct digits only with
  bit7=a … bit0=dp (§10). This agrees with the table.

## 3. Reset path

The RESET vector `$FFFE` = `$FC7F`. SWI, SWI2, SWI3 and the reserved vector
also point at `$FC7F`.

1. **`$FC7F-$FC90`: expansion hook.** If `$C000-$C00F` holds the bytes
   `00 01 02 … 0F`, the ROM jumps to `$C010` (expansion RAM or cartridge
   code). On the first mismatch it takes `LBNE $F0F0` into the normal boot.
   With expansion RAM cleared at power-on, `$C001`≠1, so the normal boot runs.
2. **`$F0F0`.** `ORCC #$50` masks IRQ and FIRQ. They stay masked for the whole
   ROM: there is no `ANDCC` on the boot path. DDRB=`$1F`, ORB=`$10` (motor
   off).
3. **`$F0FC-$F106`.** Clears filter CVs `$E400-$E407` and writes `$10` to
   `$E418`.
4. **`$F109`.** `LDS #$BF80`. The stack grows down from `$BF80`. Loading S
   arms NMI.
5. **`$F10D` → `$F1E5`.** Fills the sample RAM with `$7F`, one 32 KB quarter
   at a time. This takes about 1.7 M E-cycles, which was observed in `Machine`.
   **Quirk:** each pass ORs the bank number into the previous ORB value
   (`LDA $BF85 / ORA $E200`), so the quarters written are 0, 1, 3, 3. Quarter
   2 (sample RAM `$10000-$17FFF`) is never filled. The routine ends with
   ORB=`$10`.
6. **`$F110`: retry loop start** (every retry comes back here).
   - `JSR $F15D`, the hardware init:
     - DP=`$E8`.
     - `JSR $F08F`: FDC Force Interrupt `$D0`, then a poll until busy (status
       bit 0) clears.
     - VIA: IER=`$7F`, ACR=`$CC`, PCR=`$0E`, DDRA=`$1F`, ORB=`$30`. T1
       latch and counter = 0. T2 = `$1388` (5000 cycles, one-shot).
     - DOC: OER=`$FF`, registers `$00-$DF` cleared, then `JSR $F1BB`
       (pointer and control registers, interrupt flush).
     - ACIA master reset `$03`.
     - Clears `$8002`, `$8003`, `$8006` and `$8007`, and sets the flag
       `$BF8C`=1.
   - `JSR $F4C6`: motor on, about 0.52 s delay.
   - `$F116`: if PB6=0, go to `$F131` (no disk; §8).
   - `JSR $F20D`: load the OS and the config (§6).
   - On error (`$8006`≠0): `JSR $F413` sets the error code, `JSR $F306` shows
     it for about 1.1 s, and `$F13F-$F144` loops back to `$F110`. There is no
     retry limit: the ROM retries forever.
7. **`$F146-$F15A`: hand-off.**
   - `$8002` ← FDC track register.
   - VIA IER=`$A6`.
   - ACIA control=`$95`.
   - ORB=`$30`: motor off, sample bank 0, mux 0.
   - `JMP $800E`. CC still has I and F set, so the OS enables interrupts
     itself.

Observation in `Machine` (all I/O reads `$FF`): the run reaches the busy
poll at `$F096-$F09A` after about 1.70 M cycles and spins there, because a
stub status of `$FF` always reads as busy. No illegal opcodes were hit.

## 4. RAM used by the ROM

**`$8000-$8007`** is the FDC parameter block. The OS shares it when it calls
the ROM disk routines (§9). It is in the OS image area: §6 explains how the
loader protects it.

| Address | Use |
|---|---|
| `$8000` | Last FDC command written. The NMI handler uses it to pick the status mask. |
| `$8001` | Retry counter |
| `$8002` | Track (seek target, current track) |
| `$8003` | Sector |
| `$8004-$8005` | Buffer pointer for the next sector |
| `$8006` | Result: the masked FDC status. 0 means OK. |
| `$8007` | Boot error code: 1, 6 or 7 |

**`$8011-$802D`** receives the 29-byte configuration block (§6).

**`$BF80-$BF94`** is ROM-private:

| Address | Use |
|---|---|
| `$BF80` | Table offset |
| `$BF81-$BF84` | Current load-table entry: start track, start sector, end track, end sector |
| `$BF85` | Loop counter |
| `$BF86` | Blink phase. Incremented once per segment slot. |
| `$BF87` | Segment counter |
| `$BF88/$BF89` | Left/right pattern shift registers |
| `$BF8A-$BF8B` | Display word (high byte = left digit) |
| `$BF8C` | "First sector not yet read" flag |
| `$BF8D-$BF94` | Save area for `$8000-$8007`. `$BF8D-$BF8E` is reused as a copy-end pointer. |

The stack is below `$BF80`.

`$0000-$01FF` (sample bank 0) is a scratch buffer for the config sector. It
is refilled with `$7F` afterwards.

## 5. Floppy controller usage

### 5.1 Command primitives

Every command is written twice: to `$8000` (RAM copy) and to `<$00`
(`$E800`). All of them set `h`=1, so the 1772's own spin-up is disabled and
the motor is controlled by PB4.

| Entry | Command | What it does |
|---|---|---|
| `$F000` | `$88` Read Sector | Sector reg ← `$8003`. The loop `SYNC / LDA <$03 / STA ,X+` stores data at X. |
| `$F013` | `$88` Read Sector | Same, but discards the data (verify read). |
| `$F024` | `$A8` Write Sector | Sector reg ← `$8003`. The loop `SYNC / LDA ,X+ / STA <$03` writes from X. |
| `$F037` | `$A8` Write Sector | Writes the byte at `,X` repeatedly (fill). |
| `$F04A` | `$E8` Read Track | Loop stores at X+. |
| `$F058` | `$F8` Write Track | Loop writes from X+. |
| `$F066` | `$0C` Restore | Verify on, r=00 (6000 FDC cycles = 6 ms step). Then `BRA *`. |
| `$F06F` | `$1C` Seek | Data reg ← `$8002` (target track). Verify on, r=00. Then `BRA *`. |
| `$F07D` | `$58` Step In | u=1, no verify. Then `BRA *`. |
| `$F086` | `$78` Step Out | u=1, no verify. Then `BRA *`. |
| `$F08F` | `$D0` Force Interrupt (no interrupt) | Polls status bit 0 until clear, copies the status to `$8006`, then a short delay (`Y=1`). Does not wait for NMI. |
| `$F0A7` | (none) | Delay of Y×16 E-cycles. |

**How data moves.** Each data byte is moved after a `SYNC` (`$F00C`,
`$F01F`, `$F030`, `$F043`, `$F051`, `$F05F`). IRQ and FIRQ are masked, so
`SYNC` only resumes at the next instruction when an interrupt line asserts.
This is consistent with **DRQ wired to IRQ**, as in MAME, and the ROM
disables the other IRQ sources beforehand (VIA IER=`$7F`, DOC halted and
flushed, ACIA reset). The ROM cannot tell IRQ from FIRQ here, though: `SYNC`
wakes on either.

**How commands end.** The transfer loops and the `BRA *` after Type I
commands have no exit. A command ends only when an **NMI** (WD1772 INTRQ)
arrives. That confirms INTRQ → NMI. The ROM has **no timeouts**: a command
that never raises INTRQ hangs the ROM for good, and so does a status that
never clears busy at `$F096`.

Sector size is never programmed. A read loop stores bytes until INTRQ, so a
1024-byte sector (0-4) moves X by `$400` and the 512-byte sector 5 moves it by
`$200`.

### 5.2 NMI handler `$F0B0` (vector `$FFFC`)

1. `LEAS $0C,S` drops the 12-byte frame that the NMI stacked (the 6809 NMI
   always stacks the full state). This leaves the return address of the
   `JSR` that called the command primitive.
2. `LDB <$00` reads the status, which also clears INTRQ.
3. If `$8000` is `$88`, `$E8`, `$A8` or `$F8` (Type II or III), the status is
   ANDed with `$5C`: write-protect, RNF, CRC and lost data. Otherwise (Type I)
   it is ANDed with `$18`: seek error and CRC.
4. The result goes to `$8006` (and stays in B), and the handler returns with
   `RTS`.

The effect is that every primitive behaves like a subroutine that returns
when INTRQ fires, with the result in `$8006`. The OS reuses this handler,
because the NMI vector is in ROM.

Side effects: CC keeps I and F set, from the NMI entry, and the caller's
registers are lost. B equals `[$8006]` on return.

### 5.3 Retry wrappers

- **`$F3F1` (boot) and `$F4A4` (OS): seek with 2 attempts.** On an error they
  `Restore` and count down. **Quirk:** if the Restore succeeds, `$8006`
  becomes 0 and the loop exits as "OK" even though the seek failed.
- **`$F3AC` (boot) and `$F448` (OS): read sector at `$8004` with 10
  attempts.** If all fail, they Step Out, Step In, then make one last read.
  This retry behaviour is real for every sector except the first one the boot
  loader reads. For that one, see the quirk below.
- **Quirk: a failed first sector is silently accepted** (`$F3BE-$F3D5`,
  `$F25C-$F26B`). While `$BF8C`=1, a failed read calls `$F437`, which copies
  back the parameter block saved at `$F24D`. That copy holds `$8006`=0 and
  `$8001`=2 (or 1), because it was saved after a successful seek in `$F3F1`.
  - `$F3C6` decrements `$8001` to 1 (or 0), and `$F3CE` reads `$8006`=0, so
    the loop exits after **one** attempt. `$F3D5` then sees `$8006`=0 and
    returns success: no Step Out/In, no last read.
  - X is left at `$8008` by `$F437`'s copy loop. `$F263-$F269` keeps it, and
    `$F26B` stores it to `$8004`.
  - So when T0 S0 cannot be read (no ID for sector 0, or a CRC error), the
    ROM does not show "ud". It loads T0 S1 to `$8008`, carries on with the
    rest of the image about `$3F8` bytes lower than intended, and runs
    `JMP $800E` on whatever ended up there.
  - Example: a disk whose track 0 has readable IDs but no sector 0, such as a
    PC format numbering sectors 1-9, makes the restore and seek succeed. The
    error display is reached only if a later sector, the config sector, or
    the restore or seek fails.
- **`$F476` (OS):** write sector with the same retry scheme, using `$F024`.
- **`$F4C6` and `$F4D6`:** motor on (with the 0.52 s wait) and motor off.

## 6. OS load procedure (`$F20D`)

1. **`$F20D`: `Restore`.** On error, abort.
2. **Load table.** Destination `$8004` = `$8000`. For table index `$BF85`=0,1,
   `$F38C` copies the 4-byte entry at `$FB80 + 4*index` into `$BF81-$BF84`
   (start track, start sector, end track, end sector). It seeks to the start
   track with `$F3F1`.

   | Entry | Bytes | Meaning |
   |---|---|---|
   | `$FB80` | `00 00 01 05` | Tracks 0-1, starting at sector 0 |
   | `$FB84` | `02 05 0A 05` | Tracks 2-10, starting at sector 5 |

3. **Per track.** `$8003` ← start sector. The ROM reads sectors from the start
   sector up to 5: the limit is hard-coded as `CMPA #$06` at `$F274`, and the
   end-sector byte `$BF84` is never used. For each sector, `$F3AC` reads to
   `[$8004]`, then `$8004` ← X, the pointer after the sector. After sector 5
   it increments `$8002`, and if the track is still ≤ the end track it seeks
   there with `$F3F1` (`$F278-$F295`).
   - Entry 0 reads T0 S0-S5 and T1 S0-S5: 2×5632 = 11,264 bytes to
     `$8000-$ABFF`.
   - Entry 1 reads only S5 of tracks 2-10: 9×512 = 4,608 bytes to
     `$AC00-$BDFF`.
   - The whole OS image is `$8000-$BDFF` (15,872 bytes). It stops after
     index 1 (`$F29D`). The `CMPX #$BE00` at `$F2A1` is a second, redundant
     end test.
4. **Protecting `$8000-$8007`.** Before each read, while `$BF8C`=1 (set at
   `$F1B5`, so only until the first sector succeeds), `$F425` saves
   `$8000-$8007` to `$BF8D`. After the first successful sector, `$F437` copies
   them back and `$BF8C` is cleared (`$F25C-$F269`). So **the first 8 bytes of
   T0 S0 never reach RAM**: `$8000-$8007` stay the parameter block. The same
   restore makes a *failed* T0 S0 read look successful (§5.3 quirk): the load
   then continues from `$8008`.
   - (dump) `resmos32.bin` starts with `88 09 0B 05 00 00 00 00`. That is the
     parameter block exactly as the ROM leaves it: last command Read Sector,
     9 retries left, track 11, sector 5, pointer `$0000`, status 0, code 0.
5. **Configuration (`$F2AF`).**
   - It uses the table entry at `$FB88`: `0B 05 0B 05`, which is track 11,
     sector 5.
   - It reads that sector to `$0000`, the sample RAM window, bank 0.
     `$F4C6` left PB4-PB0 at 0, so PB1:PB0=0.
   - It copies bytes `$0000-$001C` to `$8011-$802D` (29 bytes, end pointer
     `$802E` in `$BF8D`), then refills `$0000-$01FF` with `$7F`.
   - `mirfmt.txt` calls T11 S5 the "configuration parameters". (OS) In
     SND-A01, T11 S5 bytes 0-28 equal the OS image bytes at `$8011-$802D`.
6. **Motor off** (`$F2AB` → `$F4D6`).
7. **Entry.** If `$8006`=0, the hand-off in §3 runs `JMP $800E`.

The ROM never checks what it loaded. There is no signature or checksum: any
readable disk is jumped into. Even a disk without a readable T0 S0 is jumped
into (§5.3 quirk).

## 7. Vectors

| Vector | Target | Notes |
|---|---|---|
| RESET | `$FC7F` | |
| SWI, SWI2, SWI3, reserved | `$FC7F` | |
| NMI | `$F0B0` | In ROM. This is the FDC handler for the ROM and the OS. |
| IRQ | `$8008` | In RAM |
| FIRQ | `$800B` | In RAM |

The ROM **never writes `$8008-$800D`**. Those bytes come from the disk: T0 S0
bytes 8-13 land there because the protected block ends at `$8007`. The OS
entry `$800E` comes from bytes 14-16. (OS) In SND-A01 they are:

- `$8008: JMP $893C`, the IRQ dispatcher. It reads the VIA IFR, sends SR to
  `$8989` and T2 to ROM `$F4DF`, and so on.
- `$800B: JMP $A151`, the FIRQ handler.
- `$800E: JMP $B920`, the OS entry.

IRQ and FIRQ are never taken while the ROM runs, because I and F stay set.
The only interrupt the ROM takes is NMI.

## 8. Display and error codes

**`$F33C` (one display scan).** The display word `$BF8A` is split into
`$BF88` (high byte) and `$BF89` (low byte). For each segment B=0..7:

- It starts from `A = B | $18`, both digits disabled.
- If bit 7 of the blink phase `$BF86` is set:
  - When the next MSB of `$BF88` is 1, it clears PA3 (`ANDA #$F7`).
  - When the next MSB of `$BF89` is 1, it clears PA4 (`ANDA #$EF`).
- It writes ORA, waits 256×8 cycles (about 1.07 ms per segment slot in
  total), and increments `$BF86`.

So the pattern is bit7=a … bit0=dp, and the high byte drives **PA3**. The
digits are dark while `$BF86` bit 7 is 0, so the code blinks: 128 slots on,
128 off. One slot is about 2,136 E-cycles (2,048 delay plus 88 overhead),
or 1.068 ms. So each phase is about 0.137 s, and a full on-plus-off period
is about 0.27 s.

**`$F306` (show an error code).** It maps error code `$8007` to a display word
and runs 127 scans (about 1.1 s):

| Code | Set at | Word | Left/right glyphs | Reading |
|---|---|---|---|---|
| 1 | `$F413` | `$7A9E` | d, E | "dE": disk error. Unreachable in practice; see the bug below. |
| 6 | `$F41F` | `$387A` | u, d | "ud": unformatted disk. Shown for every load error. |
| 7 | `$F134` | `$2A7A` | n, d | "nd": no disk (PB6=0) |
| (none) | `$FC9B` | `$FDB7` | 0., 5. | "O.S." error |

The "O.S." error comes from `$F571`, the OS-called calibration, when it is
entered with B≠`$10`. It shows for 2×127 scans (about 2.2 s), then
`JMP $F0F0` (cold boot).

The glyphs read as words only if the high byte is the **left** digit. That
means **PA3 = left-digit enable and PA4 = right-digit enable.** This
contradicts MAME's comment ("bit 3 is right anode, bit 4 is left anode").
**Inferred** from the meaning of the codes, but the evidence is consistent
across all four words.

**`$F413` bug.** The code does `LDA #$10 / BITB $8006`, where `BITA` was
clearly intended, to choose "ud" for RNF and seek errors. On every error path,
B already holds `[$8006]` (§5.2), so the test is non-zero whenever there is an
error, and the code is always 6. The emulator just runs it; this only matters
when you predict what the panel shows.

**Insert-disk state.** There is no separate prompt. With no disk, the ROM
loops: about 0.52 s of spin-up with the display off, then "nd" blinking for
about 1.1 s, and so on.

**Display during a normal load (inferred).** The ROM does not write ORA before
the first error display. After a VIA reset ORA=0, so once DDRA=`$1F`
(`$F175`), PA0-4 drive 0: segment a is selected and both digits are enabled.
That would light the top segment on both digits.

**Keypad.** The ROM never reads it. The keypad code table at `$FB68` exists
only for the OS (§10).

## 9. Routines that only the OS calls

(OS) These are the JSR/JMP targets in ROM from the SND-A01 OS image. The
`os32` and `masos` images use the same set.

| ROM | Purpose (from the code) | Example OS call site |
|---|---|---|
| `$F013`, `$F037` | Verify read; fill write | `$AA60`, `$AA6E` |
| `$F07D`, `$F086`, `$F08F` | Step in, step out, force interrupt | `$A809`, `$AA87`, `$A4CA` |
| `$F0A7` | Delay of Y×16 cycles | `$971F` |
| `$F1BB` | DOC: pointers=`$10`, control=`$63` (halt), flush interrupts | `$888E` |
| `$F448`, `$F476`, `$F4A4` | Read sector, write sector and seek, with retries | `$A74B`, `$A68B`, `$AABB` |
| `$F4C6`, `$F4D6` | Motor on (+0.52 s), motor off | `$A4D4`, `$A536` |
| `$F4DF` | T2 tick tail. Reloads T2 high (5000 cycles, so a 400 Hz tick at 2 MHz), re-enables FIRQ, decrements the software timers at DP `$2E-$37` (DP=`$80`, so `$802E-$8037`), then `RTI`. | `JMP` from the IRQ dispatcher at `$894E` |
| `$F514` | 16-bit lookup `[$F94D+2B]`, shifted right by 10−A bits (none if A>9), in the octave pitch table | `$8C6E` |
| `$F52D` | T2=5000, IER=`$A6`, DP=`$80` | `$9F74` |
| `$F541` | IER=`$7F`, ACIA reset, ORB `$00`→`$30`, ORA-nh=`$18` (display off) | `$9F6B` |
| `$F55B`, `$F564` | Copy Y bytes X→U; swap Y bytes X↔U | `$9FE0` |
| `$F571` | Filter calibration. Requires B=`$10`; otherwise "O.S." and a reboot. Sets ORB=`$10` (mux 0). Writes filter CVs through `$F5DC` (X=`$E418` → `$E418`, `$E408`, `$E410`). Times the oscillation of the DOC A/D (`$ECE2`) against the thresholds `$90`/`$70` and adjusts the CV until the count is in `$80..$85`, then stores `B-$7A` at `$18,U`. | `$B976` (X=`$E418`, B=`$10`, U=`$B050`) |
| `$F5E7`, `$F5F1`, `$F6DD` | Register writes with interrupts masked (`ORCC #$50` … `ANDCC #$AF`). Arguments come on the stack or in Y-relative operands. Probably DOC voice parameters (to be confirmed with the OS). | `$8AE4`, `$8C7C`, `$8BB9` |

Heads-up for first boot (Task 9) and filters (Task 11). Each A/D wait loop in
`$F5BE` ends only on a threshold crossing or when Y wraps (65,536
iterations). A constant A/D value therefore makes the calibration very slow:
seconds per call. That is why MAME feeds a fake waveform.

## 10. Data tables (`$F77D-$FC7E`)

Do not disassemble these. The OS references them by address.

| Range | Contents |
|---|---|
| `$F77D-$F7FC` | 128-byte monotonic curve, `00`…`7F` (OS `LDX #$F77D` at `$8C48`) |
| `$F7FD-$F84C` | 40 big-endian words, starting `FFFF 8000 4000 2EE0 2000 1770 …` and ending `0000`. Probably a rate table (OS `LDU #$F7FD`). |
| `$F84D-$F94C` | 256-byte triangle wave, `00`→`7A`→`00`→`86`→`FE`. Probably for an LFO (OS `LDX #$F8CD`, the midpoint). |
| `$F94D-$FB4C` | 256 big-endian words, `$4911`…`$91BD`: one octave of exponential pitch steps (ratio about 2). Used by `$F514`. |
| `$FB4D-$FB67` | 7-segment font, bit7=a … bit0=dp. Glyphs: 0-9, A-F, L, n, o, P, r, U, `_`, `-`, top bar (`$80`), c, u. (OS `LDU #$FB4D`) |
| `$FB68-$FB7F` | 24-byte permutation of 0..23: the keypad code map, 3 rows × 8 columns (OS `LDY #$FB68`) |
| `$FB80-$FBD3` | Disk area table, 4-byte entries (start track, start sector, end track, end sector). See the list below. |
| `$FBD4-$FBF7` | Small bit-mask and step tables (`01 02 04 … 80`, etc.). Referenced by the OS around `$A8D9-$AA23`. |
| `$FBF8-$FC7E` | 135-byte monotonic curve, `01`…`7F` |

The disk area table holds:

- `$FB80`: OS entries (§6)
- `$FB88`: config, T11 S5
- `$FB8C`: `20 05 22 05`, directory sectors T32-34 S5
- `$FB90`: lower sound halves (T2-14, T28-40, T54-66, sectors 0-4)
- `$FB9C`: upper sound halves (T15-27, T41-53, T67-79)
- `$FBA8`: the 8 short sequences (S5 of T20-23, 35-38, 55-58, 24-27, 28-31,
  39-42, 43-46, 59-62)
- `$FBC8`: the 3 long sequences (S5 of T12-31, 35-54, 55-74)

These agree with `mirfmt.txt`.

## 11. Corrections to the hardware reference (the ROM wins)

1. **Display enables.** PA3 = **left** digit and PA4 = **right** digit, both
   active low (§8). MAME and the plan had them the other way round.
2. **PB4** is active low: 0 = drive selected and motor on (§2.1).
3. **PB6** is a level "disk present" input, 1 = disk in (§2.1). It is not a
   latched disk-change.
4. **VIA Φ2.** The MIDI setup (T1 latch 0, PB7 → ACIA ÷16) gives 31,250 baud
   only at 2 MHz. This confirms the 2 MHz E clock in `clocks.h` (no change
   needed) and shows the VIA is clocked by E, not by MAME's 3 MHz.

**Confirmed:**

- Memory map rows for ACIA (`$E100`), VIA, filter CVs, WD1772, DOC (ROM
  accesses `$EC00-$ECE2`) and ROM.
- NMI ← INTRQ.
- IRQ ← DRQ. Consistent, but the ROM cannot tell it from FIRQ.
- Segment order a..dp = 0..7.
- PB1:PB0 bank select.
- PB2-3 ADC mux 0 used for calibration.
- DDRA and DDRB = `$1F`.

**Not exercised by the ROM:** the keypad rows (PA5-7), PB5 (DOC sync), the
ACIA data register, CA2/CB1/CB2.

## 12. Emulator implications

- **SYNC.** It must resume on an IRQ (or FIRQ) line assertion while I/F are
  masked, without vectoring.
- **NMI.** It must stack the full state (12 bytes), because the handler
  relies on `LEAS $0C,S`. INTRQ must fire exactly once per Type I/II/III
  command. Force Interrupt `$D0` must not raise INTRQ.
- **Busy bit.** The FDC must clear busy promptly after `$D0` (`$F096`), or
  the boot hangs before reaching the disk check.
- **Type I commands with no disk.** The ROM only issues them after PB6
  reports a disk. With a disk present but unformatted, Restore-with-verify
  must end with Seek Error (status bit 4) after the 1772's 5-revolution
  limit. The ROM then shows "ud" and retries forever.
- **PB6.** It must read 1 whenever a disk image is inserted, independent of
  any step history.
- **Expansion RAM.** `$C000-$C00F` must not power on holding `00..0F`, or the
  ROM jumps into expansion RAM.
- **Sample RAM quarter 2.** Its contents survive the ROM fill, so power-on
  RAM state there is visible to the OS.
- **Non-bootable disks (Task 9).** "ud" is shown only when the restore, a
  seek, a sector after T0 S0, or the config sector fails. A disk whose T0 S0
  cannot be read but whose other sectors can is loaded misplaced (from
  `$8008`) and jumped into (§5.3 quirk). The test should expect "the ROM jumps
  to `$800E` and the CPU runs whatever is there without crashing the host",
  not an error display. What Task 9 observed is in §13.

## 13. First boot in the emulator (Task 9)

Measured with `Machine` (no DOC or filters yet: `$EC00-$ECEF` reads `$FF`),
the real ROM and `tests/test_boot.cpp`. Reproduce with
`mirage_cli run --disk <img> --seconds S [--trace t.txt --trace-from T]`.
Times are emulated seconds from power-on.

### 13.1 SND-A01: where the OS boot stops

| Time | What happens | Evidence |
|---|---|---|
| 0-6.25 s | ROM loads the OS (22 sectors: 21 OS + config) | `sectors: 22` |
| 6.246 s | `JMP $800E` → `JMP $B920`, the OS entry. `LDS #$B052`, DP=`$80`, `JSR $875E` | trace |
| 6.248 s | `$B96E`: for X=`$E418`..`$E41F` (8 filters), B=`$10`, U=`$B050`+`$4D`·n, `JSR $F571` (ROM filter calibration, §9) | trace |
| 6.25-70.27 s | Inside `$F571`. The A/D reads `$ECE2`=`$FF`: the wait for a rise above `$90` (`$F5C7`) ends at once, but the wait for a fall below `$70` (`$F5D4`) never does, so each of the four timed waits ends only when Y wraps (65,536 passes). That is about 8.19 s per filter (6.68 s for the first, whose Y starts at `$EC9B`). With Y=0 after the four timed waits, `$F58E` takes the "no oscillation" exit and stores `B-$7A` = `$36` at `$18,U`. | trace: `B976` at 6.248, 12.926, 21.118, ... 62.078 s; `B983` at 70.270 s |
| 70.27 s | `$B935`: ORB=`$18` (A/D mux PB3:PB2 = 10, the pitch wheel), three `$ECE2` reads averaged into `$900A`: the pitch-wheel centre calibration (for Task 11). Then `JSR $896B`, `JMP $B900`: the main loop. The display lights and the OS sends 5 MIDI bytes (TX IRQ path `$A14B`). | probe |
| 71.1-98.1 s | The OS loads the lower sound: 133 more sectors (155 in all) | `sectors: 155` |
| 98.07 s | `$9F6F`: re-init (`$F541`, `$F52D`, `$A07E` ACIA `$03`/`$95`, ACR=`$CC`, T1 latch 0); then the main loop | probe |
| ≥ 98.1 s | Display steady at segments `DB 06` (DisplayState order, bit 0 = a): left a,b,d,e,g,dp, right b,c, i.e. "2." "1" | `display: DB 06` |

So **at 10 s the OS is in the ROM's filter calibration `$F571`, waiting for
the DOC A/D (`$ECE2`) to oscillate across `$90`/`$70`.** It does not hang: with
a constant A/D the calibration gives up per filter after the Y wrap, and the
whole boot completes at about 98 s with no illegal opcodes. Once the DOC
(Task 10) and the filter feedback (Task 11) exist, each calibration should take
milliseconds. Fitting the DOC (Task 10, §13.3) did not change this: its A/D
still read `$FF` until Task 11. With the filters (§13.4) the calibration takes
about 0.14 s per filter and the OS reaches its main loop at 7.36 s.

No chip bug was found during this boot: the ROM load, the hand-off, the OS's
IRQ/FIRQ set-up and the sound load ran on the existing CPU, VIA, ACIA and
WD1772 models unchanged.

Other observations:

- The OS IRQ handler `$893C` reads the VIA IFR; with IFR bit 7 clear it reads
  the DOC interrupt status `$ECE0` and returns (`RTI`) when bit 7 is set, as
  with the stub's `$FF`; bit 7 clear goes to the DOC interrupt code `$8871`.
  So the OS expects `$ECE0` bit 7 = 0 to mean "oscillator interrupt pending".
  This matches the 5503 data sheet (OIR bit 7 low while an interrupt is
  pending, oscillator number in bits 5-1); §13.3 shows the OS decoding it.
- The OS scans the display from its main loop (`$9DF0-$9E32`, ORA-nh
  `$E20F`), one segment per pass, and reads the keypad rows between two
  writes (`$9E25`). A pass sometimes takes longer than the panel model's 20 ms
  persistence, so about 1 in 10 display samples taken every 10 ms misses a
  segment.

### 13.2 Non-bootable and other disks

| Disk | What the ROM does | Test |
|---|---|---|
| None | PB6=0 → "nd" blinking, retry forever. No illegal opcodes. | `boot: no disk shows "nd"` |
| Blank (every track unformatted) | Restore with verify ends in Seek Error after 5 revolutions → "ud" blinking (code 6, §8), retry forever. No sector is read, `$800E` is never reached, no illegal opcodes. | `boot: a blank disk shows "ud" ...` |
| Soundprocess sound packs (`Soundprocess Sound Packs/IMG/*`) | **Not sound-only:** all 7 carry the Soundprocess OS in tracks 0-10 (`$8008: JMP $996E`, `$800E: LBRA $A5F1`). The ROM boots it exactly like SND-A01: 22 sectors, `$800E` at 6.25 s, the same `$F571` calibration, display "01" (`3F 06`) and 142 sectors by 120 s. No illegal opcodes. | `boot: a Soundprocess sound pack boots its own OS` |
| SND-A01 with tracks 0-11 zero-filled (a sound disk with an empty OS area) | Every sector reads, so the ROM loads zeros and jumps to `$800E`. `00 00` is `NEG <$00` with DP=`$E8`, i.e. read-modify-write on the WD1772 command register; the CPU runs through zeroed RAM, hits illegal opcodes (8 by 10 s, counted and skipped) and ends up in the ROM's read loop waiting in `SYNC` at `$F00C`. The host keeps running. | `boot: a disk with a zero-filled OS area ...` |
| Noise (450,560 bytes from a fixed-seed LCG) | Loads noise and jumps to `$800E`. The CPU keeps executing garbage: 369,208 illegal opcodes by 10 s, still rising, many distinct PCs. | `boot: a disk of LCG noise ...` |
| All `$FF` | `$800E` is `STU $FFFF`, repeated through RAM into I/O and ROM. The CPU falls into the Read Sector primitive `$F000`, issues `$88` and waits in `SYNC` at `$F00C` for good: the motor is off (PB4=1 since the hand-off), so the disk never turns and no DRQ arrives, and the ROM has no time-out (§5.1). 8 illegal opcodes on the way. Hardware would hang the same way. | `boot: an all-$FF disk ...` |

None of the asset images is a real sound-only disk with an unreadable OS area:
every `.img` under the asset root has a JMP table at `$8008` (factory OS,
Soundprocess OS, Sound Cells, MirImage `FMT-1`) except the EDM2IMG sample
`M_13.img`, which holds a `6D B6` fill pattern.

### 13.3 With the DOC 5503 fitted (Task 10)

Measured with the DOC model (`engine/src/doc5503.cpp`; A/D still a constant
`$FF`) and scratch probes over `Machine`.

| Observation | Evidence |
|---|---|
| The boot timeline of §13.1 is unchanged: `$800E` at 6.25 s, the slow `$F571` calibration, display `DB 06` and 155 sectors at about 98 s, no illegal opcodes. No DOC interrupt is taken during the boot (`$8871` never entered). | probe, 120 s; `mirage_cli run --seconds 15` gives the same `display: 00 00`, `sectors: 22` as before |
| The ROM fills sample RAM with `$7F` before its DOC clear (`$F197`), so while the cleared oscillators run (control 0, FC 0) they read `$7F`: no zero halt and no completed table during the boot. Control becomes `$63` at `$F1D1`. | probe: osc 5 data `$80` → `$7F` at cycle 1,706,997, control `$63` at 1,708,410 |
| The OS never rewrites the oscillator enable: OER stays at the ROM's `$FF`, i.e. 32 oscillators, scan rate 8 MHz / (8 × 34) = 29,411.76 Hz. | probe: `$ECE1` reads `$FF` throughout |
| A MIDI note (`90 3C 64` after boot) starts oscillators 0-3 as two pairs: even oscillators in swap mode with IE (`$0E`, attack table `$A800`, 2 KB, res 3), odd ones one-shot, halted, IE (`$0B`, `$B000`, 4 KB, res 4). FC `$01FF` / `$0200` (a detuned pair). When an even oscillator ends its table the DOC halts it and starts its partner; the OS IRQ handler reads the OIR (`$45` = osc 2, then `$41` = osc 0) and reprograms the even oscillators. | probe: register changes every 100 µs, `$8871` entries with B = OIR |
| The odd oscillators then halt on the wavesample's zero end marker (16 zero bytes at `$B3F0`; the lower sound has such markers at the end of several pages, e.g. `$A7F0`, `$D7F0`, `$FFF0`), interrupt (`$47` = osc 3) and the OS halts the whole voice (control `$03`). Output lasted about 0.1 s of the 1 s held note. Whether this program should loop (i.e. whether the OS should have re-pointed the oscillator) is not settled here; it is a check for the first audio task. | probe |

### 13.4 With the filters and the A/D (Task 11)

Measured with the filter bank, the CV path and the A/D mux (`docs/hardware/filters.md`, which
has the details and evidence).

| Observation | Evidence |
|---|---|
| SND-A01: the 8 calls of `$F571` end at 6.390, 6.525, 6.667, 6.810, 6.946, 7.085, 7.223 and 7.358 s, each with B = `$7A` and Y = `$82`/`$83` (converged); the stored offsets `$B068 + $4D * n` are all 0. `$B935` (pitch-wheel centre, `$900A` = `$60`), then the main loop `$B900` at 7.36 s. | `tests/test_boot.cpp` (`$F5AC` trace), trace |
| The display lights at the main loop (7.37 s), is dark while the OS loads the lower sound (sectors 23-155, until about 35 s), and is steady at `DB 06` from about 35 s. The same as §13.1 shifted 63 s earlier. (Timings before the cumulative FDC sector layout; see §13.6 for the current ones.) | `mirage_cli run --seconds T` |
| `EnsoniqOS/os32` (the same OS as SND-A01) and `masos` (calibration loop at `$BDC0`) give the same calibration times and results; `masos` settles at `DB 06`, 155 sectors. The Soundprocess OS disk (loop at `$9630`) calibrates the same way, settles at `3F 06` with 142 sectors at about 34 s. | `boot:` tests for masos and the Soundprocess OS |

### 13.5 Boot completion and the first note (Task 12)

**When is the OS ready to play?** There is no panel key to press: the OS loads the first sound
(the lower one) by itself at boot (§13.1). The audio tests wait for a condition instead of a fixed
time (`tests/test_audio.cpp`, `bootedSnapshot`): the display lit (either digit non-zero, sampled
every 10 ms) for 1 s running, no FDC sector read for 2 s running, and the drive motor off
(`Machine::driveActive()` false), capped at 90 s of emulated time. SND-A01 meets it at **37.2 s**
(the display is steady from about 35 s, the 2 s of FDC quiet follows; with the cumulative sector
layout it is **14.6 s**, §13.6). The test boots once per
process, stores `saveState()` and every section loads that copy (a boot is about 5 s of wall
time in Debug).

**What the first note looks like in the DOC** (probe: `Doc5503::read` of `$00-$DF` every
millisecond after `90 nn 64`; sound 1 of SND-A01, wheels at rest, so FC includes no bend):

| Keys | What the OS programs |
|---|---|
| 44-56 | Oscillators 0-3 on DOC channel 0 (osc 0/1 and the detuned osc 2/3, FC differs by 1), wave at `$4800`: FC `$01A6` (44) ... `$034C` (56), exactly 2:1 per octave, 1.0595 per semitone. The OS plays the sample as a chain of power-of-two tables, one per interrupt: 2 KB at `$4800` (swap, IE), 4 KB at `$5000`, 8 KB at `$6000` (one-shot); about 0.47 s at key 48 and 0.6 s at key 44. The volume register falls by 1 about every 15 ms from the key-on. |
| 57-60 | `$8000` (57), `$A800`/`$B000` (58-60): 3 KB, FC `$01E2` (58), `$01FF` (59 and 60). The second oscillator is one-shot, not swap, so the note ends when it reaches the zero marker at `$B3F0`: 0.2 s. Keys 59 and 60 have the same FC (a split boundary of the sound, not a pitch error in the model). |
| 61-63, 64+, 72 ... | Other samples (`$B600`, `$D800`, upper-bank `$0000`/`$8000` for 72). Key 72 plays a 32 KB table and lasts 1.3 s, decaying. |

No key of sound 1 sustains while held: the last segment of every chain is a one-shot (`ctl` `$0A`/`$0B`),
so the OS has no loop to re-point it to. This is the answer to the question left open in §13.3
(the voice halting at the zero marker is the OS's own programming, not a DOC fault). Whether the
factory sound really has its loop switch off is not checked here (the sound-parameter block of the
disk image is not decoded).

Output level (`Machine::render`, gain 0.5, vel 100): key 44 peaks 0.26 (RMS 0.045 over the held
second), key 60 peaks 0.12 for 0.2 s. During the boot calibration (§13.4, 6.26-7.57 s) the eight
self-oscillating filters are audible: RMS 0.13, peak 0.29.

### 13.6 With the cumulative FDC sector layout (Plan 2, Task 1)

The WD1772 model first spread the six sector slots evenly (k/6 of a revolution, 1041 bytes
apart). A 1024-byte sector takes 1074 byte times, so after every big sector the next ID field had
just passed and the OS waited almost a revolution per sector. The slots now sit at cumulative byte
offsets (gap 1 = 16, gap 3 = 50 bytes; fdc.md "Track layout"), which leaves the OS 1.6 ms between
the end of one sector and the next ID field. Nothing in the OS or ROM changed; the sound loads in
seconds because the controller no longer misses every second window. Measured with
`mirage_cli run --disk SND-A01.img --seconds T` and a trace of `$800E`, `$F5AC`, `$B900` and
`$9F6F`:

| Time | What happens |
|---|---|
| 4.06 s | `JMP $800E`: the ROM has loaded the OS (22 sectors; 21 at 4 s, 22 at 5 s) |
| 4.20-5.17 s | The 8 filter calibrations end at 4.200, 4.337, 4.474, 4.611, 4.748, 4.884, 5.026 and 5.166 s (the same 0.14 s each, B = `$7A`, Y = `$81`-`$83`) |
| 5.17 s | Main loop `$B900`; the display lights for 30 ms, then goes dark while the OS loads the lower sound (38 sectors at 7.5 s, 91 at 10 s, 140 at 12 s) |
| 12.55 s | Sector 155 (the last) is read |
| 12.56 s | `$9F6F` re-init, the display is steady at `DB 06` |
| 14.56 s | Settled by the `bootedSnapshot` rule (lit for 1 s, no sector for 2 s, motor off) |

| Quantity | Before (k/6 layout) | After (cumulative layout) |
|---|---|---|
| `$800E` hand-off | 6.25 s | 4.06 s |
| Last calibration / main loop | 7.36 s | 5.17 s |
| Last sector (155) read | about 35 s | 12.55 s |
| SND-A01 settled (`bootedSnapshot`) | 37.2 s | 14.56 s |
| `masos` settled (`boot()` rule, no motor test) | about 37 s | 14.58 s |
| Soundprocess OS disk settled | about 34 s | 13.28 s |

The lower sound's 133 sectors take 7.4 s (18 sectors/s overall, including track steps and the
OS's own work), against 28 s before. The 200-cycle INTRQ-to-next-command latency of the
back-to-back test is far inside gap 3. The settle time is still dominated by the ROM's own load
(4 s, mostly seeks between the one-sector-per-track reads of tracks 2-10) and the fixed 3 s the
`bootedSnapshot` rule adds, not by the FDC layout. Tests: `boot: SND-A01 settles with its lower
sound loaded in under 15 s`, `wd1772 reads sectors 0-5 of a track in under two revolutions back to
back`.
