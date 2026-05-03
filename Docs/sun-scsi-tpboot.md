# Sun-2 tpboot ↔ SCSI Tape Reference

**Scope:** how the SunOS 3.x **tpboot** standalone (the program loaded by
`b st()` from tape file 1) talks to the on-board Sun-2 SCSI controller
during a multi-stage boot, what the standalone expects from the
controller and the device, and which messages it prints (`st: sense
error`, `st: short transfer`, `st: error %x`, etc.) — and most
importantly, how to make those messages quiet.

This is the standalone-side companion to `Docs/sun-scsi-tape.md` (which
covers the PROM's tape driver). All address references are runtime
addresses for tpboot loaded at its relocation target, not file offsets.

---

## 1. Where tpboot lives

- **Source:** SunOS 3.2 install tape, **file 1** (`media/tape/tape3.2/01`,
  30720 bytes).
- **Loaded by:** PROM `b st()` → record-read loop → bcopy into RAM → jump.
- **Self-relocates** at entry to RAM **`0xa0000`** (see file offset
  `0x0` — `move SR,#$2700`; `lea -6(PC),A0`; `lea $a0000,A1`; copy loop;
  jmp into relocated region).
- After relocation, all tpboot code/data lives at `0xa0000..0xa77ff`
  (≈ 30 KB).

| Region | Runtime address |
|--------|----------------|
| tpboot code | `0xa0000` – `0xa6700` (approx) |
| message strings | `0xa6738` – `0xa67ff` |
| tape error strings | `0xa6bc4` – `0xa6c20` |

---

## 2. Architecture at a glance

```mermaid
flowchart TD
    A["b st() loads file 1<br/>tpboot relocates to 0xa0000"] --> B[user prompt:<br/>typical entry: 'st(0,0,N)']
    B --> C[parse spec → unit/lun/N]
    C --> D[st_open: own copy of PROM's<br/>scsi_state init]
    D --> E[mode-2 promotion probe<br/>REQUEST_SENSE alloc=11]
    E --> F[REWIND + N×SPACE FILEMARK]
    F --> G[record-read loop:<br/>READ chunks, bcopy to load addr]
    G --> H{short transfer?}
    H -->|no| G
    H -->|yes, suppress flag set| I[silent EOF]
    H -->|yes, suppress flag clear| J["print 'st: short transfer'<br/>0xa3c2c"]
    I --> K[jump to entry of loaded program]
    J --> K
```

tpboot **does not** call back into the PROM monitor for SCSI; it has its
own copy of the PROM's tape driver, talking directly to the Sun-2 SC
controller MMIO. So the same DMA-count-register conventions documented
in `sun-scsi-tape.md` §8 apply here verbatim.

---

## 3. The error-message print sites in tpboot

All four `st:` messages live as PC-relative `pea string.l` calls into a
single `printf`-shaped function at `0xfef0084` (in PROM ROM). The
print sites are clustered together in tpboot's tape-driver post-transfer
analysis function:

| Runtime addr | Bytes | What it prints |
|--------------|-------|----------------|
| `0xa3b10` | `48 79 00 0a 6b c4` | `pea $a6bc4` → `"st: sense error"` |
| `0xa3b76` | `48 79 00 0a 6b d5` | `pea $a6bd5` → `"st: error:"` |
| `0xa3bfe` | `48 79 00 0a 6c 00` | `pea $a6c00` → `"st: error %x"` |
| `0xa3c2c` | `48 79 00 0a 6c 0e` | `pea $a6c0e` → `"st: short transfer"` |

---

## 4. The "st: short transfer" check (THE important one)

This is the single most-often-printed message during multi-stage tape
boot. The check sits at the tail of tpboot's `scsi_command_with_check`
(roughly `0xa3804..0xa3c46`). The relevant fragment, post-transfer,
after `scsi_transfer` has returned `D7 = bytes_transferred`:

```
0xa3c10:  cmp.l   (-$1c,A6), D7        ; D7  = bytes_transferred
                                         ; -$1c,A6 = requested_length
0xa3c14:  blt     0xa3c26               ; if bytes_transferred < requested -> short
0xa3c1c:  move.l  (-$1c,A6), D0         ; full transfer: return requested
0xa3c20:  bra     0xa3c3e               ; (return path)

;------ short-transfer path ------
0xa3c26:  tst.l   ($10,A6)              ; ENABLE-PRINT flag (3rd arg)
0xa3c2a:  beq     0xa3c3c               ; if 0, skip print (silent EOF)
0xa3c2c:  pea     $a6c0e                ; "st: short transfer"
0xa3c32:  movea.l $0fef0084, A0         ; printf
0xa3c38:  jsr     (A0)
0xa3c3a:  addq.w  #4, A7
0xa3c3c:  move.l  D7, D0                ; return D7 (partial count)
```

### 4.1 The print is unconditional on residual

There is **no REQUEST_SENSE consult, no FILEMARK silent-exit path** in
this branch. The decision is purely:

> If the controller reports `bytes_transferred < requested_length`,
> **AND** the per-call enable-print flag (3rd argument, at `A6+0x10`)
> is non-zero, **print the message**.

This is fundamentally different from the PROM's analogous code path,
which has a mode-2 + FILEMARK silent exit (`Docs/sun-scsi-tape.md` §4).
tpboot has no equivalent silent path — your only options are:
(a) report no residual, or (b) hope the caller passed enable-print=0.

### 4.2 The 3rd-argument flag is per-call

Different callers in tpboot pass different values. Example: the
record-read loop's READ caller (around `0xa37d8`) hardcodes
`pea $1` → the read-loop *always* enables the short-transfer print:

```
0xa37d8:  pea     $1.w                  ; enable-print = 1
0xa37dc:  pea     (A5)                  ; state ptr
0xa37de:  cmpi.l  #$2, ($c,A6)          ; mode 2?
0xa37e6:  bne     0xa37ec
0xa37e8:  moveq   #$a, D0               ;  -> WRITE(0x0a)
0xa37ea:  bra     0xa37ee
0xa37ec:  moveq   #$8, D0               ; mode 1 -> READ(0x08)
0xa37ee:  move.l  D0, -(A7)             ; cmd
0xa37f0:  jsr     0xa3804               ; scsi_command_with_check
0xa37f6:  lea     ($c,A7), A7           ; pop 12 bytes (3 args)
```

**Implication:** every short transfer the standalone READ encounters
prints `st: short transfer`. The only way to make the read loop quiet
is to make the controller report no residual.

---

## 5. The DMA-count register — same protocol as PROM

tpboot's `scsi_transfer` uses the identical DMA-count protocol as the
PROM (see `sun-scsi-tape.md` §8):

1. Host writes `~N` to controller offset `0x0c` (DMA count register).
2. Controller increments register by **1 per byte** transferred to memory.
3. After transfer, host reads register; `residual = ~register`.
4. `bytes_transferred = N - residual`.

For an odd-length transfer with `ODD_LENGTH` set in `sc_icr`, the host
performs the trailing-byte fixup (read `sc_data` holding byte, write to
end of buffer, increment `dma_count` by 1) — exactly as the PROM does.

So the controller-side rules for keeping tpboot quiet are the same as
those for keeping the PROM quiet: tick `sc_dma_count` by 1 for every
byte the host's DMA actually consumes, and don't tick for bytes parked
in the holding register.

---

## 6. How to make tpboot's tape EOF silent

Recall the goal: tpboot reads tape file N in chunks; eventually the file
ends; we want **no** `st: short transfer` print at the boundary.

Given §4.1 (no FILEMARK silent exit; check is unconditional on residual)
and §5 (DMA-count protocol shared with PROM), the only way is:

> **Make the controller report a full-count transfer even when the
> tape file actually ended early. Signal EOF through `CHECK CONDITION`
> status + sense FILEMARK instead of through DMA residual.**

In emulator terms: on a tape short read, **zero-pad** the host buffer
to the full requested length, **tick the DMA count register all the
way** (so `residual = 0`), then set `status = CHECK CONDITION` and
prepare a sense response with the FILEMARK bit.

The standalone's flow then becomes:

1. READ N bytes — controller reports N transferred (no residual).
2. `bytes_transferred (D7) == requested_length` → no short-transfer print.
3. Status byte returned by command = `0x02` (CHECK CONDITION).
4. tpboot issues REQUEST_SENSE; sense byte 2 bit 7 (FILEMARK) is set.
5. tpboot recognises FILEMARK and silently stops reading the file.

This is what RetroCore's `SunSCSIController` does, and it's why the C#
emulator boots without any `st: ...` noise.

The trade-off vs. silently auto-advancing on short read (the trap that
caught earlier emulator versions): we **must not** auto-advance the
file index until tpboot has explicitly consumed the FILEMARK via
REQUEST_SENSE — otherwise the next READ pulls bytes from file N+1 into
the same buffer and the loaded program decodes garbage into a memory
range it doesn't own (Sun-2/120: bus error at virt `0xc0000` inside a
PROM-bcopy loop, the original "FC=5 SPACE" symptom).

---

## 7. The other three messages (cosmetic)

For completeness, here's what the remaining three tpboot tape messages
mean:

### 7.1 `st: sense error` (`0xa3b10`)
Fired when **scsi_transfer returns -1** (the controller's bus state
machine signalled an error before the data phase completed, e.g. a
phase-mismatch waiting for `MSG_IN`). Occurs on bus-protocol failures,
not on residual counts. With a sane controller emulation it should
never fire.

### 7.2 `st: error:` (`0xa3b76`)
Header for the structured error-key dump (`sense key is %x`, `error is
%x`) printed when CHECK CONDITION came back with a non-FILEMARK,
non-EOM sense key. tpboot's interpretation of mode-1 vs mode-2 sense
mirrors the PROM's (see `sun-scsi-tape.md` §4 mode-2 path), so a
correctly-formed FILEMARK sense (byte 2 bit 7 set, sense key NO_SENSE
in low nibble) suppresses this print.

### 7.3 `st: error %x` (`0xa3bfe`)
The mode-1 status-word fallback, identical in shape to the PROM's `st:
error 80` (see `sun-scsi-tape.md` §12). Triggered by the same condition:
mode-2 promotion gate failed in **tpboot's own** st_open probe →
subsequent commands decode the response data through the mode-1 status
word at `default_buffer[+4..+5]`. Avoiding this requires the same
controller fix as the PROM's mode-2 promotion: deliver exactly 11 bytes
when tpboot does the alloc=11 REQUEST_SENSE probe, and have the
controller's DMA count register reach `0xFFFF` after.

---

## 8. tpboot's command set vs. PROM's command set

tpboot uses the same SCSI-1 group-0 6-byte CDB form as the PROM, with
the same per-command rules. The internal `cmd_op` namespace is similar
but not identical; in particular, tpboot speaks SCSI cmd codes
directly (no `0x81/0x83/0x84` rewrites the PROM does):

| SCSI opcode | Used by tpboot | Notes |
|-------------|---------------|-------|
| `0x00` TEST UNIT READY | yes | startup probe |
| `0x01` REWIND | yes | between stages |
| `0x03` REQUEST_SENSE | yes | mode-2 probe (alloc 11) and EOF detection |
| `0x08` READ(6) | yes | record-read loop |
| `0x0a` WRITE(6) | yes (mode 2) | for tape-write standalone tools |
| `0x11` SPACE | yes | filemark advance for `st(0,0,N)` |
| `0x12` INQUIRY | yes | drive identification |
| `0x15` MODE_SELECT(6) | yes | density 0x84 (mode 2 confirmed) |
| `0x1b` LOAD/UNLOAD | rarely | cleanup |

The SPACE handler advances by `cmd[4]` filemarks. After advancing,
subsequent READs pull from the new tape file naturally — the host
doesn't issue a separate "open file N" request.

---

## 9. Cross-reference: tpboot symbols (annotated by reverse engineering)

| Runtime addr | Role |
|--------------|------|
| `0xa0000` | tpboot entry / relocation target |
| `0xa3804` | `scsi_command_with_check` (the function with the print sites) |
| `0xa3b10` | `pea s_st_sense_error` |
| `0xa3b76` | `pea s_st_error_colon` |
| `0xa3bfe` | `pea s_st_error_word` (mode-1 fallback) |
| `0xa3c2c` | `pea s_st_short_transfer` (the residual-check print) |
| `0xa3c10..0xa3c46` | post-transfer analysis (residual check + return) |
| `0xa37d8` | example READ(6) caller — passes enable-print=1 |
| `0xa6bc4` | string `"st: sense error\n"` |
| `0xa6bd5` | string `"st: error:\n"` |
| `0xa6c00` | string `"st: error %x\n"` |
| `0xa6c0e` | string `"st: short transfer\n"` |
| `0xa6c20` | string `"st: SCSI tape\0"` |
| `0x0fef0084` | (PROM) printf-shaped function pointer used by tpboot |

---

## 10. Bottom line for the emulator

The standalone tpboot is **stricter** than the PROM about residual
reporting:

- The PROM prints `st: sense error` on every short transfer too, but
  has a documented mode-2 + FILEMARK **silent exit** (see
  `sun-scsi-tape.md` §4 mode-2 branch). In mode 2 with proper FILEMARK
  sense, the PROM is silent.
- tpboot's READ-loop short-transfer check has **no silent path**: it
  prints whenever the residual is non-zero and the per-call enable-print
  flag is set. The READ caller hardcodes the flag to 1.

So the **only** way to keep tpboot quiet at tape file boundaries is
controller-side: make the residual zero, signal EOF via CHECK
CONDITION + FILEMARK in sense, and let tpboot consume the FILEMARK
via REQUEST_SENSE before any auto-advance to the next tape file.

---

## 11. MODE SELECT (CDB 0x15) — DATA OUT direction

The `b st()` → `tpboot` → `st(0,0,N)` flow used to print two
`st: short transfer` lines around the `Standalone Copy` banner. The
trigger at `0xa3c2c` showed `D7 = 12, requested = 13, CDB = 15 …` —
i.e. tpboot's MODE SELECT(6) confirm of mode 2 (parameter list = 13
bytes, density 0x84).

Two controller-side bugs combined to produce a 1-byte residual:

1. The dispatcher in `sim/scsi.c` routed `0x15` through the DATA-IN
   helper (`sc_dma_read_data`), which uses the `ODD_LENGTH` holding
   register protocol. MODE SELECT is **DATA OUT** — host RAM → device
   — and must use `sc_dma_write_data`, which ticks `sc_dma_count` for
   every byte.
2. `sc_dma_write_data` was setting `SC_ICR_ODD_LENGTH` for odd-length
   transfers. For OUT transfers the host clocks every byte through
   the controller (nothing is parked); leaving the flag set causes
   tpboot's post-write fixup to subtract 1 (the inverse of the IN
   direction's +1 fixup) and produces a spurious 1-byte residual.

The fix is two short edits in `sim/scsi.c` (route MODE SELECT through
`sc_dma_write_data`, and call `sc_reset_odd_len()` at the top of
`case 1:` to mirror `case 0:`) and `sim/sc.c` (always clear
`SC_ICR_ODD_LENGTH` at the end of `sc_dma_write_data`).

If the regression hook in `sim/sim68k.c` at `PC == 0xa3c2c` ever fires
again, the most-recent CDB in `scsi_cmd_buf[]` and the requested length
at `-0x1c(A6)` (per §4 above) identify which command's residual went
non-zero.
