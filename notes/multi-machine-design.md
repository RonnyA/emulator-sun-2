# Design: multi-machine support (Sun-2/120 + Sun-3/60 in one binary)

## Goal

One `sim` binary that can be told at the command line which machine to
emulate, with its own ROM, MMU, bus map, and devices.

```
sim/sim --machine=sun2/120 --prom=media/rom/sun2-multi-rev-10F.bin --disk=...
sim/sim --machine=sun2/120 --prom=...                              --monitor=1024x1024
sim/sim --machine=sun3/60  --prom=media/rom/sun3_60_v3.0.1.bin     --disk=...
```

## Decisions (locked in)

1. **Two machines only**: `sun2/120` (Multibus) and `sun3/60` (P4
   bwtwo).  We are *not* doing Sun-2/50 (VME) — keep its enum entry
   for now but not actively maintained.
2. **Monitor as separate `--monitor=` flag**, default `1152x900`.
   The bwtwo `JUMPER_HIRES` bit is driven by this.  Avoids the
   confusing `sun2/120-hi` "machine" naming.  The current `--mode=`
   becomes a back-compat alias: `--mode=2/120-hi` →
   `--machine=sun2/120 --monitor=1024x1024`.
3. **ROM files stay flat** in `media/rom/` — no per-machine subdir.
   PROM file is selected entirely by `--prom=`; the machine type byte
   in the IDPROM is set by `--machine=`.
4. **Reuse the existing Sun-2 SCSI host adapter (`sim/sc.c`,
   `sim/scsi.c`) for Sun-3** too.  Don't port NCR 5380 from RetroCore
   — just bind the existing HBA at OBIO `0x140000` for Sun-3 vs
   MBMEM `0x80000` for Sun-2.  Quick-and-dirty but works.
5. **Order of work — Option C (hybrid)**: tiny Phase 0 first
   (extract just OBMEM/OBIO/MBMEM/MBIO dispatch into function
   pointers) → Sun-3 skeleton next, drive the rest of the abstraction
   from what Sun-3 actually needs.

## What changes between Sun-2/120 and Sun-3/60

| | **Sun-2/120 (have)** | **Sun-3/60 (new)** |
|---|---|---|
| CPU | M68010 | M68020 |
| Optional FPU | none | 68881 |
| MMU | Sun-2 custom (sim68k.c, 9-bit segment, 4-bit page, 11-bit offset, 4096 PMEGs, 24-bit VA → 23-bit PA) | Sun-3 custom (28-bit VA, 8 KB page, 16 pages/PMEG, 2048 segments/context, different PTE bit layout) |
| FC=3 control | sun2_pgmap/segmap/context/idprom/diag/buserr/sysen at offsets 0..0xF | upper-4-bits-select: IDPROM, page-map, segment-map, context, system-enable, bus-error, diag, VAC, UDVMA, UART-bypass |
| Boot PROM | 0xEF0000 / 64 KB | 0x0FEF0000 / 64 KB (different VA) |
| Boot/cold path | system-enable EN_BOOTN | system-enable bit 7 = NOTBOOT |
| RAM | 1–8 MB at 0x00000000+ | 4–24 MB at 0x00000000+ |
| Framebuffer | bwtwo Multibus, 1152×900, FB at 0x700000, CSR at 0x781800 | bwtwo P4, 1152×900, FB+P4-reg at 0xFF000000 (with P4 type-ID register at offset 0x300000 and monitor-sense at 0x1C0000) |
| Keyboard SCC | Z8530 at OBMEM 0x780000 | Z8530 at OBIO 0x000000 (SCC1) |
| Console SCC | Z8530 at OBIO 0x782000 | Z8530 at OBIO 0x020000 (SCC0) |
| Timer | AM9513 at OBIO 0x002800 | Intel 8254 at OBIO 0x0A0000 (interrupt reg) — different chip |
| RTC | MM58167B at OBIO 0x003800 | Intersil ICM7170 at OBIO 0x060000 — different chip |
| SCSI | Sun-2 SCSI host adapter (multibus) | NCR 5380 (SI board) at OBIO 0x140000 |
| Ethernet | 3Com 3C400 (multibus) | AMD LANCE at OBIO 0x120000 |
| EEPROM/NVRAM | none (config in IDPROM only) | 2 KB at OBIO 0x040000 |
| IDPROM machine type byte | 0x01 (2/120 multi) / 0x02 (2/50 VME) | 0x17 (3/60) |

The C# RetroCore is the working reference for both.  Primary spec:

- **`/Users/ronny/rh/RetroCore/sun-machines.md`** — comprehensive
  Sun-machines hardware reference.
  - §1 — bwtwo display family (all 5 variants, including the
    Multibus variant we already implement and the P4 variant we need
    for Sun-3/60).
  - §2.1 — Sun-2 family (CPU, MMU, OBMEM/OBIO maps, IRQ levels).
  - §2.2 — Sun-3 family (CPU, MMU, OBMEM at `0x0FEF0000` PROM,
    OBIO map for Sun-3/60 specifically).

- **`/Users/ronny/rh/RetroCore/Emulated.HW/Sun/MMU/Sun3/Sun3MMU.cs`**
  — port the Sun-3 MMU translation from here.

- **`/Users/ronny/rh/RetroCore/Emulated.Machines/Sun/Sun3/MachineSun3Memory.cs`**
  — Sun-3 bus wiring (which device at which OBIO address).

- **`/Users/ronny/rh/RetroCore/Emulated.Machines/Sun/Sun3/ROMChips/sun3_60_v3.0.1.bin`**
  — Sun-3/60 boot PROM (extract using the same byte-array → binary
  pattern we used for `sun2_multi_rev_10f`).

## Minimum viable Sun-3/60

For a first cut we want only what's needed for the PROM monitor to
come up to a prompt.  That means:

- M68020 CPU (already in our musashi core via `M68K_CPU_TYPE_68020`)
- Sun-3 MMU translation (read/write of pgmap/segmap/context, plus the
  upper-4-bit FC=3 dispatch)
- Boot PROM mapped at `0x0FEF0000` with the same "PROM-magic" fallback
  we already do for Sun-2 (when MMU yields PA=0 in OBIO, redirect to
  the PROM)
- IDPROM with machine type 0x17, valid checksum
- Z8530 SCC kbd/mouse at OBIO 0x000000 (we already have a Z8530
  driver — `sim/scc.c`, just needs a different bus base)
- Z8530 SCC console at OBIO 0x020000
- bwtwo P4 framebuffer (we already have the bwtwo CSR / FB code in
  `sim/sun2.c` — needs to be parameterised to handle the P4 layout)
- Memory enable register / system enable register at FC=3 control
  space — different bit layout from Sun-2
- Stub everything else (timer, RTC, SCSI, LANCE Ethernet) to return
  0xFF / accept-and-ignore until we know what the PROM actually
  probes

This gets us a banner & prompt.  Disk and net come later.

## Refactor plan — three phases

### Phase 0 — parameterise without breaking anything (no Sun-3 yet)

Goal: today's `sim` builds, today's Sun-2 modes still work, but the
hardcoded paths in `sim68k.c` are replaced with function tables that
will later hold Sun-3 implementations.

New files:

- `sim/machine.h` — public interface
- `sim/machine.c` — registry / `--machine=` dispatch
- `sim/machine_sun2.c` — moves the existing Sun-2 wiring out of
  `sim68k.c` into a struct that fills the interface

Public interface (sketch):

```c
typedef struct sun_machine {
    const char *name;          /* "sun2/120", "sun2/50", "sun3/60", ... */
    int cpu_type;              /* M68K_CPU_TYPE_68010 / _68020 */
    uint32_t default_ram_bytes;
    uint32_t prom_load_pa;     /* 0x00ef0000 / 0x0fef0000 / etc. */
    uint32_t prom_size;
    /* MMU translation. */
    uint32_t (*translate)(uint32_t va, uint fc, int write,
                          uint *mtype, uint *fault, uint *pte);
    /* FC=3 control space r/w. */
    uint32_t (*ctl_read )(uint32_t addr, int size);
    void     (*ctl_write)(uint32_t addr, uint32_t value, int size);
    /* OBMEM / OBIO / MBMEM / MBIO read/write — one function per
       page-type, dispatched after MMU translation tells us which. */
    uint32_t (*pa_read [4])(uint32_t pa, int size);
    void     (*pa_write[4])(uint32_t pa, uint32_t value, int size);
    /* IDPROM bytes (32) — built at init from machine_type via the
       existing idprom_setup() with a machine-supplied template. */
    void (*idprom_init)(unsigned char idprom[32]);
    /* Frame-error / abort / device hooks. */
    void (*on_reset)(void);
    void (*on_idle)(void);
} sun_machine_t;

extern const sun_machine_t *g_machine;

const sun_machine_t *machine_lookup(const char *name);   /* sim.c */
void machine_print_list(FILE *f);
```

The existing Sun-2 modes (2/120, 2/120-hi, 2/50) become entries in
`sun_machine_t`.

Refactor steps inside Phase 0 (each landable on its own commit, none
of which changes runtime behaviour):

1. Pull the OBMEM dispatch into `machine_sun2.c` as `sun2_obmem_read`
   / `sun2_obmem_write`.  Have `sim68k.c` call them through
   `g_machine->pa_read[OBMEM]` etc.
2. Same for OBIO, MBMEM, MBIO.
3. Pull the FC=3 control-space dispatch (mmu_read / mmu_write) into
   `sun2_ctl_read` / `sun2_ctl_write`.
4. Pull `cpu_map_address` into `sun2_translate`.
5. Pull `idprom_setup` template into `machine_sun2.c`.
6. Add `--machine=NAME` to `sim.c`.  Keep `--mode=` for sub-variants
   inside the chosen machine.

After Phase 0: `git diff` is large, runtime trace identical.  The
user can already pick `--machine=sun2/120` and it does what `sim`
does today.

### Phase 1 — Sun-3 skeleton

`sim/machine_sun3.c` is added.  It declares `sun3/60` and:

- Sets `cpu_type = M68K_CPU_TYPE_68020`.
- `prom_load_pa = 0x0FEF0000`.
- IDPROM template with machine type 0x17.
- All `pa_read`/`pa_write` initially return 0xFF / no-op (graceful
  default; logs the unhandled access at `trace_cpu_io`).
- `translate` is a stub that uses identity mapping for low memory and
  the PROM-magic redirect for unmapped OBIO.

Goal: `sim --machine=sun3/60 --prom=sun3_60.bin` doesn't crash and
runs the M68020 CPU enough to see what the PROM probes first.  Build
fails cleanly if the user passes a Sun-2 PROM to `--machine=sun3/60`
(verify by IDPROM machine-type byte mismatch — warn + continue).

### Phase 2 — Sun-3 functional bring-up

Iteratively, exactly like the Sun-2 1.0F bring-up we just did:

1. Sun-3 MMU translate proper (port from C# `Sun3MMU.cs`).  Use
   `trace_mmu_rw` to verify it matches RetroCore's translation table
   for the same PROM memory map.
2. Sun-3 control-space FC=3 dispatch (different from Sun-2; it
   multiplexes by the upper 4 bits of the address).
3. Z8530 SCC kbd at OBIO 0x000000 and SCC console at 0x020000 — we
   reuse `sim/scc.c`; just need a thin per-machine OBIO dispatch
   case.
4. Intersil ICM7170 RTC stub (can return 0 / current time).
5. NMI / timer / interrupt routing — similar shape to what we just
   rebuilt for Sun-2 1.0F.
6. bwtwo P4 framebuffer — extend `sim/sun2.c` (or rename to
   `sim/bwtwo.c`) so the same renderer handles both Multibus and P4
   variants by having the machine pass an offset for FB / CSR / P4
   register.  CSR/JUMPER bits already abstracted via `g_mode` in
   `sun2.c`.
7. RAM size: M68020 + Sun-3 expects up to 24 MB.  Bump `MAX_RAM`
   handling to be runtime-driven from `g_machine->default_ram_bytes`,
   but allocate the static `g_ram[]` to `MAX_RAM = 24 MB` upfront so
   we never have to malloc late.  (Or switch to malloc-on-init.)

### Phase 3 — disks, ethernet, polishing

NCR 5380 SCSI for Sun-3, AMD LANCE Ethernet, Sun-3 NVRAM at OBIO
0x040000, etc.  Same incremental approach.

## Files to modify in Phase 0 (concrete)

| File | What |
|---|---|
| `sim/machine.h` (new) | the `sun_machine_t` interface, `g_machine` extern, registry function signatures |
| `sim/machine.c` (new) | machine registry; calls `machine_sun2_register()` and (later) `machine_sun3_register()`; `--machine=` lookup |
| `sim/machine_sun2.c` (new) | every Sun-2 wiring; absorbs the existing `sun2_modes[]`, the OBIO dispatch from `sim68k.c:io_read`, etc. |
| `sim/sim.c` | parse `--machine=` and `--mode=`; default `--machine=sun2/120` |
| `sim/sim.h` | declare `g_machine`; keep `g_mode` since modes are sub-variants of a machine |
| `sim/sim68k.c` | call `g_machine->pa_read[mtype](pa, size)` instead of hardcoded `cpu_read_obmem` etc.; same for ctl-space |
| `sim/sun2.c` | rename the file or split: split bwtwo display logic into `bwtwo.c` (re-usable for Sun-3 P4) and Sun-2-specific bits stay |
| `sim/Makefile` | new objects |

## What we DON'T change in Phase 0

- The MMU implementation — it's still hard-coded Sun-2 in `sim68k.c`.
  We just expose it through a function pointer so Phase 1 can swap in
  a Sun-3 implementation.
- The static `g_ram[MAX_RAM+1]` and `g_rom[MAX_ROM+1]` arrays.  We can
  move to dynamic later; not required to refactor the dispatch.
- The musashi core.  The same musashi handles 68010 and 68020;
  switching is a single `M68K_CPU_TYPE_*` value at init.

## Resolved questions

- **Monitor selection**: separate `--monitor=1152x900|1024x1024` flag
  (default `1152x900`).  Drives bwtwo `JUMPER_HIRES`.
- **ROM layout**: flat in `media/rom/`, selected purely by `--prom=`.
- **SCSI**: reuse existing `sim/sc.c` for both Sun-2 and Sun-3.
- **CPU-type switching**: confirmed — musashi `m68k_set_cpu_type()`
  at `m68k/m68kcpu.c:569` accepts `M68K_CPU_TYPE_68020` at runtime.
  Call it once at init based on `g_machine->cpu_type`.

## Open follow-ups

- Throwaway dasm helper `m68k/dasm_main.c` lives in the working tree
  but isn't in any commit.  Either move to `utils/` and commit, or
  delete after Sun-3 bring-up is done.

## Verification per phase

- **Phase 0**: rev-R 6 s + rev-10F 8 s diff against today's trace —
  must be byte-identical (modulo nondeterministic timestamps).  Type
  `x` in 1.0F monitor — same abort behaviour as today.
- **Phase 1**: `sim --machine=sun3/60 --prom=...` runs M68020
  instructions, doesn't crash, prints what it probes.  No banner yet.
- **Phase 2**: Sun-3/60 reaches PROM monitor banner.  Same level of
  smoke-test we used for Sun-2 1.0F.
- **Phase 3**: SunOS 4.0/4.1 boots from disk on Sun-3/60.

## Out of scope for this work

- Sun-4 / SPARC.
- Multi-CPU.
- Cleaning up `sim68k.c`'s sprawling include of m68k internals — that
  belongs in a separate refactor.
- Writing a new MMU — we port the algorithm from C# RetroCore
  `Sun3MMU.cs` directly, since it's already verified working.
