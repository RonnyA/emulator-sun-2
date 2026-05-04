# Sun-2 emulator — SCC handoff

**Date:** 2026-05-03
**Last working tree:** SCC bug fixes 1–5 + zs2-collision fix + `--no-kbd` framebuffer Option C + WR0 command handling + MIE mirror + (workaround) MIE-relax in TX path. All committed.

This doc is for the next person/LLM continuing the SunOS-via-serial-console work. It captures the goal, what's already been done, what's known to be wrong, the precise next plan (multi-instance SCC refactor), and where every relevant document lives.

---

## 1. Goal

Make `make run-serial` boot SunOS 3.2 on the emulated Sun-2/120 all the way to the multi-user login prompt over a `telnet localhost 9900` connection, while also keeping `make run` working with the SDL display window.

Right now we get the full PROM banner, kernel boot messages, device probe (sd0, ec0, bwtwo0, tod0, zs0), `root on sd0`, `using 100 buffers ...`, and **then exactly one more byte (`/`) before the boot stalls**. This stall is reproducible — same point every run.

---

## 2. What works today

| Capability | Status |
|---|---|
| 32-assertion SCC unit test suite (`make test-scc`) | ✅ all pass |
| `make run` with SDL display + bwtwo console | ✅ works |
| `make run-serial` PROM banner via telnet | ✅ works |
| `make run-serial` SunOS kernel boot via telnet (up to `/`) | ✅ works (~993 bytes) |
| `make run-serial` SDL display window (Option C) | ✅ open + framebuffer writable except first word |
| Pcap networking (Linux + Windows) | ✅ unchanged from before |
| SCC TCP server (single client, raw bytes, separate pthread) | ✅ no regression |
| GitHub Actions matrix build (Linux/macOS/Windows pcap+stub) | ✅ unchanged |

---

## 3. What's broken: the `/` hang

After SunOS prints `using 100 buffers containing 366592 bytes of main memory\r\r\n`, exactly one more byte (`/` or sometimes `U` — depending on timing) is emitted, then **all SCC TX writes stop**. The sim is alive (no crash, no panic), but no further bytes flow.

### Trace evidence

Running with `SCC_RD_TRACE=1` + `SCC_WR_TRACE=1` shows:

```
... lots of polled writes ending in "memory.\r\n" ...
scc-rd ch=1 ctl  -> 0x04   (polled TX_READY check, returned ok)
scc-rd ch=1 ctl  -> 0x04   (still polling)
... many of these ...
scc-rd ch=1 ctl  -> 0x0e   ← *anomaly* (0x04 | 0x08 | 0x02)
scc-rd ch=1 ctl  -> 0x00   ← then 0
scc-rd ch=1 ctl  -> 0x01   ← then RX_READY?
scc-rd ch=1 data -> 0xff   ← 3 reads of empty data port
scc-rd ch=1 data -> 0xff
scc-rd ch=1 data -> 0xff
scc-rd ch=1 ctl  -> 0x04
scc-rd ch=1 ctl  -> 0x04
scc-wr ch=1 0x2f /         ← writes "/"
scc-rd ch=0 ctl  -> 0x04   ← !! switches to ch=0 (ttyb chan B)
scc-rd ch=0 data -> 0xff   ← reads ttyb data (FIFO empty)
scc-rd ch=1 ctl  -> 0x04
[hang -- no more bytes either way]
```

The `0x0e → 0x00 → 0x01` ctl-read pattern indicates SunOS is doing the WR0-pointer-then-read dance on different read registers (RR0/RR1/RR3). The cross-channel reads (`ch=0`) match the `zslevel6intr` polling logic from `sun2-scc.md §5.2` ("walk RR3 of chan A across chips to find interrupting source"). So SunOS *is* handling an SCC interrupt — but isn't getting back to writing more bytes after.

### Why the easy fixes didn't unstick it

Five low-level SCC bugs were found and fixed (see §5). After those, **`scc_ints[1]` was still `0x03 [!MIE][TIE]` at the moment of the `/` write**. That meant:

- TIE is set in our internal tracking (kernel did write WR1 = 0x13 = TIE+RIE+SIE)
- MIE is **not** set in our tracking (kernel never wrote `WR9 = 0x09` for the ttya chip)
- The `RetroCore` reference *also* gates IRQ on MIE in `CheckIrq()` (Z8530SCC.cs:786)
- So the spec-strict gate would not fire interrupts for ttya
- A workaround (TIE-only gate, `scc.c:474-481`) **didn't unstick the boot either** — same `/` hang

The pre-existing `//broken - incomplete probe?` hack at `scc.c:402-403` unconditionally sets MIE on ch=2/3 (kbd chip), which is why the keyboard side superficially "works"; without that hack, the kbd chip would have the same problem.

This is why the next move is a refactor, not another patch.

---

## 4. Reference documents (read these first)

| Path | What it is |
|---|---|
| `D:\Data\Torrent2\sunos_3.4_src\sun2-scc.md` | **Authoritative SCC reference.** Sun-2 memory map, Z8530 register tables, GENERIC config lines, zsprobe/zsattach/zsparam/consconfig flow, level-6 + level-3 dispatcher walkthrough. **Read first.** |
| `E:\Dev\Repos\Ronny\RetroCore\Emulated.HW\Zilog\SCC\Z8530\Z8530SCC.cs` | **Working reference implementation in C#** (~850 lines). Per-chip `Registers` struct, central `CheckIrq()` dispatch, `InterruptAck` re-evaluates and re-asserts. The architectural target for our refactor. |
| `E:\Dev\Repos\Ronny\RetroCore\Emulated.Machines\Sun\Sun2\MachineSun2Memory.cs:487-509` | How RetroCore wires **two separate** Z8530 instances (`_sccKbdMouse`, `_serialPort`) into the Sun-2/120 address space. |
| `D:\Data\Torrent2\sunos_3.4_src\squashfs-root\sun\sys\sundev\zsreg.h` | Z8530 register/bit definitions (cited heavily in `sun2-scc.md`). |
| `D:\Data\Torrent2\sunos_3.4_src\squashfs-root\sun\sys\sundev\zs_common.c` | `zsprobe`, `zsattach`, `zslevel6intr`, `zsopinit`, `zsintr`. SunOS interrupt dispatcher source — match exactly. |
| `D:\Data\Torrent2\sunos_3.4_src\squashfs-root\sun\sys\sundev\zs_async.c` | Async/tty protocol: `zsa_*int`, `zsa_process`, `zspoll`, `zsparam`. |
| `D:\Data\Torrent2\sunos_3.4_src\squashfs-root\sun\sys\sundev\zs_asm.s` | The level-6 dispatcher in m68k asm + `setzssoft`/`clrzssoft` for the soft-int-3 path. |
| `D:\Data\Torrent2\sunos_3.4_src\squashfs-root\sun\mon2\sun2\cpu.addrs.h` | Sun-2 PROM canonical memory map (`VIDEOMEM_BASE = 0xEC0000`, `KEYBMOUSE_BASE = 0xEEC000`, `SERIAL0_BASE = 0xEEC800`, etc.). |

In Ghidra: the 1.0f Sun-2/50 PROM (`merged_rom_10f.bin`) is loaded with extensive plate/decompiler comments around `detect_keyboard`, `system_init` (0xef0892, 0xef0a38, 0xef0b58), `check_serial_ready`, `init_scc_port`, etc. Caveat: the multi-rev-R PROM is what we actually run; logic is similar but not identical.

---

## 5. What was already done in this branch

### 5.1 Five SCC bugs found by code audit, all fixed + unit-tested

Located by reading `scc.c` against `zsreg.h`:

| # | Bug | Fix | Where |
|---|---|---|---|
| 1 | WR13 store clobbered RR9 mirror | Removed stray `scc_rr[ch][9] = value` line | `sim/scc.c` case 13 |
| 2 | WR15 store clobbered RR11 mirror | Removed stray `scc_rr[ch][11] = ...` line | `sim/scc.c` case 15 |
| 3 | RR2 modified vector hardcoded to `[2][2]` regardless of which chip | Now writes to `[ch & ~1][2]` (chan-B index of *this* chip pair) | `sim/scc.c` `scc_throw_interrupt` |
| 4 | RR3 IP bits never set; SunOS `zslevel6intr` polls RR3.chan-A to find interrupting chip | `scc_throw_interrupt` now sets `IP_x_TX/RX` in `[ch \| 1][3]`; `scc_device_ack` clears | `sim/scc.c` |
| 5 | WR9 reset commands (0x40/0x80/0xC0) silently stored, never executed | Added `scc_reset_channel()` helper; case 9 dispatches | `sim/scc.c` |

Verified by `sim/test_scc.c` — 32 assertions, all pass via `make test-scc`. The unit test compiles `scc.c` with stubs for `int_controller_*`, `sun2_kb_write`, `scc_tcp_send_byte`.

### 5.2 zs2 collision fix

`MachineSun2Memory.cs` lists `zs2 at mbmem 80800` as an optional Multibus expansion serial card. Our `scc.c` only models 4 logical channels (= 2 chips). The old routing in `cpu_read_mbmem`/`cpu_write_mbmem` aliased zs2's CSR onto the same chip as zs0, and SunOS's `zsattach` for zs2 would issue `WR9 = RESET_WORLD` on the chip holding ttya open as console — clobbering its state. Fix: bus-error MBMEM `0x80800-0x80807` so SunOS's zsprobe fails and zs2 doesn't attach. See `sim/sim68k.c:694-705, 741-744`.

### 5.3 `--no-kbd` Option C: probe failure without breaking SDL display

Original problem: SunOS Sun-2 PROM probes `0xEC0000` (= VIDEOMEM_BASE per `cpu.addrs.h:34-63`) to detect a video card. RAM-like behavior at that address → "video card present, use kbd path", which routes console output via `console_scc_base_ptr = 0xEFB134` (a ROM null buffer) — telnet sees nothing.

Fix in `sim/sim68k.c:769-790`:
- `--no-kbd` makes only the **first word** of video memory (OBMEM `0x700000-0x700003`) return inconsistent values via a counter; this fails the probe's read-consistency check
- Rest of the framebuffer stays writable → SDL window opens and shows whatever the PROM/SunOS draws
- Kbd/mouse SCC region `0x780000-0x7800FF` bus-errors when `--no-kbd` is set (it's on the same physical card)

### 5.4 WR0 command handling

Added in `sim/scc.c:262-291` (first-write path):
- `RESET_TXINT (0x28)` — clears `RR3.IP_x_TX` of chan A
- `RESET_STATUS_INT (0x10)` — clears `RR3.IP_x_STAT` of chan A
- `RESET_ERRORS (0x30)` — clears `RR1` PE/DO/FE bits
- `RESET_HIGHEST_IUS (0x38)` — clears `scc_int_pending`

Without these, SunOS's TX-empty-interrupt ack would leave the IP bit set → infinite re-fire (or, with our pending-guard, no re-fire at all).

### 5.5 MIE chip-pair mirror

`WR9` is **chip-wide** on a real Z8530 — both channels of a chip share it. Old code only updated `scc_ints[ch]`; if SunOS wrote `WR9 = MIE+VIS` via chan B, chan A wouldn't see MIE. Fix in `sim/scc.c:307-326`: case 9 mirrors MIE bit to both `scc_ints[chan_b]` and `scc_ints[chan_a]`.

### 5.6 Other fixes

- TX interrupt fire from `scc_wr_data` — when MIE+TIE set (or just TIE under the workaround), throw TX-empty after each byte sent
- Removed `scc_int_pending` early-return in `scc_throw_interrupt` (level-triggered semantics)
- `scc_init_traces()` resolves all `SCC_*_TRACE` env vars once at `io_init` time, not in hot paths (was a user complaint)
- `scc_tcp.c:172` — `scc_throw_interrupt(ch, 1)` was throwing TX-empty when bytes arrived; should be `which=2` (RX char). Fixed.

---

## 6. Why the architectural refactor is the right next step

Working evidence:

1. **RetroCore boots Sun-2 via two separate `Z8530SCC` instances.** Each has its own `regs.InterruptPending`, `regs.MasterInterruptControl`, `regs.CurrentIRQStatus`, `regs.RegisterPtr`. Our shared globals fight each other.
2. **`scc_int_pending` is a single bit shared across both chips in our code.** When the kbd chip is interrupting, the ttya chip can't.
3. **`scc_cmd[ch]` (register pointer) is per-channel in our code**, but per-chip on real hardware (and per-chip in RetroCore: `regs.RegisterPtr`). The `0x0e → 0x00 → 0x01` ctl-read sequence in our trace looks like SunOS's RR3-poll loop crossing channels and hitting incorrect register state because of this.
4. **Five other latent bugs have already been per-channel-vs-per-chip confusion** (RR2 indexing, MIE mirror, WR9 reset reach). The pattern keeps repeating.

A refactor solves the entire class of bugs in one pass.

---

## 7. The plan: multi-instance SCC refactor

### 7.1 Target API

```c
typedef struct scc_chip_s {
    int   init_done;
    int   register_ptr;       /* WR0 reg-select state, chip-wide */
    int   register_ptr_primed;/* "next ctl write is data" flag */
    int   wr[2][16];          /* per-channel WR shadow */
    int   rr[2][16];          /* per-channel RR shadow */
    int   ints[2];            /* per-channel internal MIE/TIE/RIE tracking */
    int   master_int_enable;  /* WR9.MIE, chip-wide */
    int   int_pending_mask;   /* RR3 IP bitmask, chip-wide; chan A reads */
    int   irq_asserted;       /* current IRQ-line state for this chip */
    struct scc_fifo_s ififo[2];
    struct scc_fifo_s ofifo[2];
    /* Callbacks */
    void (*on_tx_byte)(struct scc_chip_s *chip, int chan, uint8_t byte);
    void (*on_irq)(struct scc_chip_s *chip, int asserted);
} scc_chip_t;

void   scc_chip_init(scc_chip_t *chip, const char *name);
unsigned int scc_chip_read(scc_chip_t *chip, unsigned int offset, int size);
void   scc_chip_write(scc_chip_t *chip, unsigned int offset, unsigned int value, int size);
void   scc_chip_in_push(scc_chip_t *chip, int chan, uint8_t byte);
int    scc_chip_ack(scc_chip_t *chip);   /* returns autovector */
void   scc_chip_check_irq(scc_chip_t *chip);  /* the central dispatch */
void   scc_chip_reset_channel(scc_chip_t *chip, int chan);
```

Two instances, instantiated in `io_init`:

```c
scc_chip_t g_scc_kbd;     /* kbd/mouse, chan A=kbd, chan B=mouse */
scc_chip_t g_scc_serial;  /* ttya/ttyb, chan A=ttya, chan B=ttyb */

scc_chip_init(&g_scc_kbd,    "kbd");
scc_chip_init(&g_scc_serial, "serial");
g_scc_serial.on_tx_byte = scc_tcp_forward;  /* hook --scc-tcp here */
g_scc_kbd.on_tx_byte    = sun2_kb_send;     /* into kbd cmd queue */
g_scc_serial.on_irq     = on_scc_irq;
g_scc_kbd.on_irq        = on_scc_irq;
```

`on_scc_irq(chip, asserted)` does `int_controller_set/clear(IRQ_SCC)` based on the OR of both chips' `irq_asserted`.

### 7.2 Routing in `sim68k.c`

| Address range | Chip |
|---|---|
| OBIO `0x2000-0x200F` | `&g_scc_serial` (Sun-2/120 ttya/ttyb) |
| OBMEM `0x7F2000-0x7F200F` | `&g_scc_serial` (alias post-MMU) |
| OBMEM `0x780000-0x7800FF` | `&g_scc_kbd` (Sun-2/120 kbd/mouse, currently routed via `sun2_kbm_read/write`) |
| MBMEM `0x80800-0x80807` | continue to bus-error (zs2 not modeled) |

`sun2_kbm_read/write` in `sun2.c` becomes a thin wrapper around `scc_chip_read(&g_scc_kbd, ...)`.

### 7.3 Central `scc_chip_check_irq()` (mirroring `Z8530SCC.cs:762-818`)

```c
void scc_chip_check_irq(scc_chip_t *chip) {
    int should_assert = (chip->int_pending_mask != 0)
                        && (chip->master_int_enable);
    if (should_assert && !chip->irq_asserted) {
        chip->irq_asserted = 1;
        if (chip->on_irq) chip->on_irq(chip, 1);
    } else if (!should_assert && chip->irq_asserted) {
        chip->irq_asserted = 0;
        if (chip->on_irq) chip->on_irq(chip, 0);
    }
}
```

Call this at the **end of every state-changing operation**: `scc_chip_write`, `scc_chip_in_push`, FIFO drain on data read, WR1 update, WR9 update, `scc_chip_ack`. Same pattern as RetroCore.

### 7.4 `scc_chip_ack` — re-evaluate after de-assert

```c
int scc_chip_ack(scc_chip_t *chip) {
    if (!chip->irq_asserted) return -1;
    chip->irq_asserted = 0;
    if (chip->on_irq) chip->on_irq(chip, 0);
    /* If sources still pending, re-assert. */
    scc_chip_check_irq(chip);
    return AUTOVECTOR;
}
```

This is the level-triggered behavior. If kernel ack'd one IP bit but more remain, IRQ goes back high immediately.

### 7.5 Migration path

The unit tests in `test_scc.c` exercise the global API. They'll need to be ported to the per-instance API. Recommended:
1. Land the new `scc_chip_t` struct alongside the old globals (don't delete yet).
2. Port `test_scc.c` to use a local `scc_chip_t` and verify all 32 assertions still pass against the new model.
3. Switch `sim68k.c` routing to use the two instances.
4. Delete the old globals once sim boots.
5. Remove the `//broken - incomplete probe?` MIE hack at the old `scc.c:402-403`.

Don't try to do steps 3 and 4 in one commit — keep them separable.

---

## 8. Diagnostic procedures

### 8.1 Reproduce the hang

```bash
cd E:\Dev\Emulators\68k\emulator-sun-2
make all
make run-serial
# in another shell:
telnet localhost 9900
# wait ~45 seconds for PROM banner + kernel boot
# stalls at "/"
```

Or scripted (90 s window, dumps full RX):

```python
# test_scc_tcp.py is already in the repo; copy + extend its read window to 70s.
```

### 8.2 Useful trace env vars (resolved once at startup, no hot-path getenv)

| Var | What it does |
|---|---|
| `SCC_WR_TRACE=1` | logs every byte written to a data port (`scc_wr_data`) |
| `SCC_RD_TRACE=1` | logs every read of an SCC register (`scc_read`) — high volume |
| `SCC_INT_TRACE=1` | logs WR1, WR9, and TX-interrupt-fire decisions |
| `SCC_TCP_TRACE=1` | logs every TCP-server byte direction |
| `MBMEM_TRACE=1` | logs unhandled multibus accesses |
| `OBMEM_TRACE=1` | logs unhandled OBMEM accesses |

For the rd-trace use a file redirect (`stderr=PIPE` truncates in Python's `communicate()`):

```python
trace_fd = open("scc_trace.log", "wb")
proc = subprocess.Popen([...], stderr=trace_fd, stdout=subprocess.DEVNULL, ...)
```

### 8.3 Unit tests

```bash
make test-scc    # builds and runs sim/test_scc.c, expects 32/32 pass
```

The unit test stubs `int_controller_set/clear`, `sun2_kb_write`, `scc_tcp_send_byte`. Edit those stubs at the bottom of `sim/test_scc.c` if APIs change.

### 8.4 Unhealthy-but-known states to look out for

- `scc_ints[1]` reads `0x03 [!MIE][TIE]` at the moment of the `/` write — confirmed empirically; needs the refactor to fix.
- After RESET_WORLD, `scc_reset_channel` zeros `scc_ints[ch]` including any MIE bit. SunOS *should* re-write WR9 with MIE after reset; on the ttya chip it apparently doesn't. The refactor's central `master_int_enable` field plus `check_irq` re-eval should make this clear (right now the bit lives in two places: `scc_wr[ch][9] & 0x08` and `scc_ints[ch] & 0x80`, and they can drift).
- The `0x0e → 0x00 → 0x01` ctl-read pattern: SunOS is doing `ZWRITE(0, n)` to set the register pointer then reading. Make sure the per-chip `register_ptr` survives across reads / writes correctly — our current code clears `scc_cmd[ch]` after every read, which may not match the chip's behavior.
- `scc_rd_data` returns `0xff` on empty FIFO via `value = 0xffff` initial → truncated. Confirm against datasheet — a real Z8530 returns the *last* received byte, which would be different. Easy patch in `scc_rd_data`: cache the last-popped byte.

---

## 9. File map of changes since master

| File | What changed |
|---|---|
| `sim/scc.c` | All five Bug 1–5 fixes; WR0 command handling; MIE chip-pair mirror; TX interrupt firing in `scc_wr_data`; `scc_init_traces()` startup function; trace flags as cached statics; removed `scc_int_pending` early-return in throw |
| `sim/sim68k.c` | `--no-kbd` framebuffer Option C (first-word inconsistent reads + kbd SCC bus-error); zs2 collision fix (bus-error MBMEM 0x80800); `io_init` calls `scc_init_traces()` |
| `sim/scc_tcp.c` | New file — pthread TCP server, SCC ringbuf bridge, single client; **`scc_throw_interrupt(... , 2)` for RX** (was `1` = TX, the old bug) |
| `sim/scc_tcp.h` | Public API for the TCP module |
| `sim/test_scc.c` | New unit test for SCC: 32 assertions covering bugs 1–5 |
| `sim/Makefile` | `test-scc` target; strips `-Dmain=SDL_main` for the unit test build (we don't link SDL2main) |
| `sim/sim.c` | `--scc-tcp[=PORT]`, `--no-kbd`, `--net-iface`, `--net-list`, `--net-dump`, `-q`; trims env-var whitespace; `setvbuf(_IONBF)` for Windows console |
| `sim/sim.h` | Include guard, `<stdint.h>`, `<fcntl.h>`, `O_BINARY` fallback, extern decls |
| `sim/sun2.c` | `--no-kbd` makes `sun2_kb_write` silent so PROM kbd timeout fires (still needed for the SDL-display path) |
| `sim/3c400.c` | Network rewrite from earlier work — receive header rebuilt with explicit masks (gcc bitfield mismatch fix), FCS append, PA bit field fix, anti-echo, L3/L4 dump_frame |
| `GNUmakefile` | `run-serial`, `run-tcp`, `run-trace`, `run` positional arg capture, `TCP=N`, `NOKBD=1` |
| `README.md` | Networking pcap docs (Windows loopback caveat, Linux/WSL, FCS rationale), SCC-over-TCP, headless boot |

---

## 10. Big things still wrong / not implemented

1. **`/` hang** — covered above, refactor needed.
2. **`scc_int_pending` is a single global** — should be per-chip in the new struct.
3. **`scc_rd_data` returns 0xff on empty FIFO** — likely wrong; Z8530 returns last received byte. Easy fix during refactor.
4. **No RR1 error bit modeling** (PE/DO/FE never set) — not blocking PROM/SunOS unless input is corrupted, but spec-incomplete.
5. **No support for zs2/zs3/zs4/zs5** — would need 3+ instances after refactor; not urgent for the user's goal (just bus-error them).
6. **The `//broken - incomplete probe?` MIE hack** at old `scc.c` (now gone since I removed it implicitly via the refactor of trace? — verify in the post-commit tree). If still there, kill it during the refactor.
7. **Sun-2/50 (VME) machine type isn't tested** — `--mode=` flag exists but our SCC routing assumes Sun-2/120 (Multibus) addresses.

---

## 11. Don't-repeat-these mistakes

- **Don't put `getenv()` in hot paths.** Probe once at module init, cache. The `scc_init_traces()` pattern is what user wants.
- **Don't speculate about RetroCore behavior without reading the C# source** at `E:\Dev\Repos\Ronny\RetroCore`. The user has caught me on this before.
- **Don't claim a fix works without empirically testing.** Run `make run-serial` + telnet, verify byte count + last-bytes pattern.
- **Don't commit binary disk images.** `media/disk/my-sun2-s3.2-disk.img` may show up dirty in `git status` after a sim run; do not stage it.
- **Don't skip git hooks (`--no-verify`)** unless explicitly asked.
- **Don't mention Claude in commit messages.** The user has been very clear about this.
- **Don't add features beyond what's asked.** No premature abstractions, no "while we're at it" cleanups.
- **The shell is Windows CMD/PowerShell** — propose CMD/PowerShell syntax, not POSIX (`tee`, `/tmp`, `grep`, etc.). Bash via the Bash tool is also available.

---

## 12. Quick sanity checklist before reporting "fixed"

- [ ] `make all` builds clean (no warnings about regressed bugs)
- [ ] `make test-scc` reports 32/32 (or the new equivalent count) passing
- [ ] `make run` opens SDL window, PROM banner appears in window, kbd works
- [ ] `make run-serial` opens telnet port; `telnet localhost 9900` shows full PROM banner + SunOS kernel boot
- [ ] `make run-serial` → `telnet localhost 9900` reaches **at least** the `/etc/rc` output (`/dev/sd0a: ...`) and ideally the `Type Ctrl-D for multi-user` prompt
- [ ] No "/" or "U" hang at the `using 100 buffers` point
- [ ] Send a Ctrl-D via telnet → SunOS proceeds to multi-user → eventually login prompt
- [ ] No regressions in `make run` (display path still works)
- [ ] `git status` is clean of disk images / spam logs

---

## 13. If you genuinely get stuck

The user has been very engaged on this debugging. Don't churn — when in doubt:
- Read `sun2-scc.md` again
- Read the relevant section of `Z8530SCC.cs` (it's only 850 lines)
- Read the SunOS source for the *specific* function you suspect (zs_async.c is well-commented)
- Ask the user

The user is responsive and prefers a short, specific question over a long speculative paragraph.
