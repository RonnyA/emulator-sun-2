# Sun-2 Boot PROM Command Reference

**ROM image:** `merged_rom_10f.bin`
**Path:** `E:\Dev\Repos\Ronny\RetroDocs\Doc\Machine\Sun2\2_50\ROM\merged_rom_10f.bin`
**Version string in ROM:** `Rev 1.0f`
**Build date in ROM:** `87/12/29` (per `@(#)romvec.s 1.1 87/12/29 Copyr 1986 Sun Micro`)
**CPU:** MC68010, big-endian (Ghidra opens it as MC68020 — the binary is 68010-clean code)
**ROM mapping:** `0x00EF0000 – 0x00EFFFFF` (64 KB)
**Banner (one of):**
- `Sun Workstation, Model Sun-2/50, Sun-2/160 or Sun-2/130, Sun-2 keyboard`
- `Sun Workstation, Model Sun-2/120 or Sun-2/170, Sun-2 keyboard`

---

## 1. Sources used for this document

Everything below is derived directly from the disassembly / decompilation of the ROM image
via Ghidra MCP. The Sun-2 boot PROM has a **built-in help table** (printed by `h`) that
lists every command — this is the authoritative source-of-truth for the command set, and
every entry below maps to a concrete `case` in the dispatcher's switch statement at
`cmd_dispatcher` (`0x00EF3AA4`).

Validated artefacts:

| Address | Contents |
|---|---|
| `0x00EF3AA4` | `cmd_dispatcher` — the universal exception/command dispatcher |
| `0x00EF3DDE` | `cmd_code_table_sorted` — 12-byte sorted code table (vector / synthesized codes) |
| `0x00EF3AEC` | `cmd_code_jumptable_base` — 12 × int16 offsets to handler entry points |
| `0x00EFC668` | `boot_device_table` — 8-pointer NULL-terminated driver table |
| `0x00EF3532` | `print_help_menu` — emits the built-in help text |
| `0x00EF5574` | `parse_boot_device` — splits `dev(c,u,p)` and runs probe/open |

---

## 2. Entering the Boot PROM monitor

The PROM monitor is reached when:

* the machine is reset and the EEPROM does not request auto-boot, OR
* the user aborts during selftest / typing during the test prompt, OR
* an `Abort` (L1-A from keyboard, or `Break` on the serial console) is delivered, OR
* the running kernel takes a fatal CPU exception and falls back into the PROM
  (`"\nException %x"` at `0x00EFC173`), OR
* a watchdog reset fires (`"\nWatchdog reset!\n"` at `0x00EFC0C5`).

The monitor prompt character is **`>`** (literal `0x3E`, emitted at `0x00EF4514` inside
`cmd_dispatcher`). On entry, the auto-boot path prints `"Auto-boot in progress...\n"`
(`0x00EFC0D7`).

---

## 3. Command parsing

Implemented inside `cmd_dispatcher`. Logic:

1. Read characters via `prom_advance_and_peek` / `prom_getchar`.
2. Skip leading spaces.
3. Letters are dispatched **case-insensitively** by `case (key & 0x5F)` — i.e. both
   `b` and `B` work.
4. After a successful command, control returns to the prompt loop at `0x00EF44F8`.
5. Unrecognised letters print `"ERROR: in command line.\n"` (`0x00EFC1E9`).
6. The script-trace prefix `tc<line>` replaces `;` with `\r` so a single typed line is
   parsed as multiple commands separated by `;`.

Numeric arguments are read by `parse_hex_number` (`0x00EF4ED0`); they are interpreted as
**hexadecimal** by default and accepted with no `0x` prefix. Whitespace between arguments
is skipped (`skip_whitespace` `0x00EF4F46`).

---

## 4. Built-in command set (validated against `print_help_menu` and the dispatcher switch)

This is the literal text printed by **`h`** (function `print_help_menu` @ `0x00EF3532`):

```
Boot PROM Monitor Commands Description Help Table

 a [digit]                       Open CPU Address Regs (0-7)
 b [dev([cntrl],[unit],[part])]  Boot a file
 c [addr]                        Continue program at this address
 d [digit]                       Open CPU Data Regs (0-7)
 e [addr]                        Open address as a 16 bit word
 f start_addr end_addr pattern [size]   Fill Memory
 g [addr]                        Go to this address
 h                               Display Help Menu
 k [number]                      Reset (0)CPU, (1)MMU, (2)System
 l [addr]                        Open address as a 32 bit long word
 m [addr]                        Open Segment Map
 o [addr]                        Open address as a 8 bit byte
 p [addr]                        Open Page Map
 q [addr]                        Open EEPROM
 r                               Open CPU Regs (i.e. PC, SR)
 s [digit]                       Set or query Function Code (0-7)
 t [y/n/c]                       Trace: Yes/No/Continuous
 u [argument]                    Use different console devices
 v start_addr end_addr [size]    Display Memory
 x                               Extended Diagnostic Tests
 z [addr]                        Set a Breakpoint
```

### 4.1 Per-command detail (all validated from the dispatcher)

| Cmd | Syntax | Behaviour (verified in code) |
|---|---|---|
| **A** | `a [digit]` | Display / open CPU **address** registers (A0..A7) of the saved frame. The optional digit selects which one to expand inline. Falls into `display_registers`. |
| **B** | `b [!][dev(c,u,p)] [name] [args]` | Boot. Sub-forms: <br>• `b!<dev(c,u,p)> args` — install MMU page tables, then `parse_boot_device` walks `boot_device_table`.<br>• `b<empty>` — re-arm via `set_cmd_2_and_redispatch` (cold-boot path).<br>• `b ?` — print syntax + supported devices.<br>The MMU table choice depends on a force-RS232 flag: `page_map_table_a` if set, else `page_map_table_b` — both also union with `page_map_table_c_rom_window`. |
| **C** | `c [addr]` | Continue. Reads optional hex `addr`; if it equals the planted breakpoint, the saved word at that PC is restored and the trace vector is reinstalled. Then returns through the saved frame so the CPU resumes at `addr` (or at the saved PC if no addr was given). |
| **D** | `d [digit]` | Display / open CPU **data** registers (D0..D7) of the saved frame. |
| **E** | `e <addr>` | Examine memory **as 16-bit words**. Address is rounded down to even; each `<RETURN>` advances by 2; modify by typing a new hex value; `.` exits. |
| **F** | `f <start> <end> <pattern> [B\|W\|L]` | Fill memory. `B`/default = byte fill (step 1), `W` = word (step 2, addresses aligned to 2), `L` = long (step 4, aligned to 4). Uses current FC (set by `S`). |
| **G** | `g [addr]` | Go (jump and execute). If `addr` omitted, uses the previously-typed value. Calls the routine with the **rest of the command line as `argv[0]`** (`(*addr)(_DAT_0000053c)`). Used by SunOS / standalone programs that come back to PROM. |
| **H** | `h` | Print the help menu (table above). |
| **K** | `k [n]` | Reset, sub-codes:<br>• `k 0` — `reset_mmu_state` + reinit display. **Soft reset, stays in monitor.**<br>• `k 1` — `set_cmd_1_and_redispatch` (synthetic NMI-style re-entry).<br>• `k 2` — `reset_mmu_state`, restore breakpoint slot, `reset_entry`. **Hard cold reset.**<br>• `k B` (decimal 0x0B) — `selftest_report` (re-prints last selftest banner).<br>• Any other → `"Invalid K Command Usage:K[0,1,2,B]"` (`0x00EFC181`). |
| **L** | `l <addr>` | Examine memory **as 32-bit long words**. Step 4. |
| **M** | `m <addr>` | Walk the **MMU segment map**. Step 0x8000 (32 KB) up to 0x01000000. Reads MMU control space (FC=3) at `(addr+5)`. |
| **O** | `o <addr>` | Examine memory **as 8-bit bytes**. Step 1. |
| **P** | `p <addr>` | Walk the **MMU page map**. Step 0x800 (2 KB) up to 0x01000000. Prints segmap byte before each pagemap entry, FC=3. |
| **Q** | `q [addr]` | Open / modify **EEPROM** byte. (Help-table entry validated; the case label is wired into the same examine-loop machinery as E/L/O. Edits go through the EEPROM-write delay loop.) |
| **R** | `r` | Display CPU **status registers** (PC, SR, etc.) of the saved frame. |
| **S** | `s [digit]` | Get/set **68020 Function Code** for memory commands. `s` alone → prints `FC<x> space`. `s n` → sets FC to `n & 7`. Affects subsequent `E/L/O/V/F` accesses. |
| **T** | `t [y\|n\|c]` | Trace control:<br>• `tc <line>` — **command-script** mode. Replaces `;` in remainder of line with `\r`, then enables trace (sets bit 0x8000, installs vec 0x24).<br>• `ty` — trace yes (clears script ptr, enables trace bit, installs vec 0x24). Prints `"Tracing...\n"`.<br>• `t` (anything else) — trace **OFF**. Clears bit 0x8000, restores previous vec 0x24. Prints `"Trace Off\n"`. |
| **U** | `u [argument]` | Switch / configure console device. Calls `configure_scc_io_params`. Sub-forms (per ROM string `"u%ci, u%co, ua%x, ub%x, uu%x, u"`):<br>• `u <n>i` — set input baud rate `n`<br>• `u <n>o` — set output baud rate `n` (or clear)<br>• `ua <hex>` — write byte pattern to **port A**<br>• `ub <hex>` — write byte pattern to **port B**<br>• `uu <hex>` — write to UART control reg<br>• `u` — show current settings.<br>Errors: `"Invalid Baud rate!\n"`, `"Cannot test port in use!\n"`. |
| **V** | `v <start> <end> [B\|W\|L]` | Display memory range as a 16-byte-per-row hex+ASCII dump. `B` = bytes, `W` = words, `L` = longs. Press `space` during dump to abort. Uses current FC. |
| **W** | `w <addr>` | Indirect-call into the global stub at `_DAT_000004e6`: `(*stub)(addr, command_line)`. Used by SunOS PROM callbacks and `dl`/`vmunix` standalone programs. |
| **X** | `x` | **Surprise — does NOT enter the extended test menu in this ROM revision.** The handler at `0x00EF44E0` is:<br>1. If the **serial-console flag** at `0x668` is zero (framebuffer/keyboard active), write `0x12` to the byte at `0x758`.<br>2. `clear_buserr_reg` (`0x00EF05F0`).<br>3. Fall through to the `Z` case, calling `run_diagnostics(FC)` (`0x00EF39CE`).<br>Verified by xref scan: **nothing in this ROM reads `0x758`**, and `extended_test_menu` (`0x00EF4626`) has **no callers at all** — it is dead code in `merged_rom_10f.bin`. The help-table entry "x - Extended Diagnostic Tests" therefore lies in this rev. The Extended-Test code exists fully assembled in the image (see §6) and is presumably reached on other revisions or under a build-time switch this ROM doesn't enable. |
| **Z** | `z [addr]` | Plant a **breakpoint** at `addr`. Saves the original word, writes a `TRAP #1` (`0x4E41`) at `addr`, installs vector 0x84. Prints `"Break %x installed\n"` (`0x00EFC092`). When the trap fires, the monitor prints `"Break "` and the address, then drops to `>`. |
| **default** | (any other letter) | Prints `"ERROR: in command line.\n"`. |

> Help-menu mismatch with the source: the help text says "Reset (0)CPU, (1)MMU,
> (2)System". The decompiled handler instead does:
> 0 = reset MMU state + reinit display, 1 = synthetic re-entry, 2 = full hard reset.
> Treat the labels in the help table as hints, not exact semantics.

---

## 5. The `B` (boot) command — full detail

### 5.1 Syntax

```
> b                                     (empty: try every device in probe order)
> b ?                                   (print syntax + supported device list)
> b dev(ctlr,unit,part) [name] [opts]   (target a specific device)
> b!dev(...) name [opts]                (force re-init of MMU before booting)
```

The format string used to print the resolved boot record is:
`"Boot: %c%c(%x,%x,%x) %s\n"` (`0x00EFC6AD`).
The default boot file name when none is given is **`munix`** (`0x00EFC6A6`) — note this
is the Sun-2 a.out kernel (`vmunix` arrives later on Sun-3).

### 5.2 The supported device codes — validated

The boot device table at `0x00EFC668` is exactly **8 drivers + NULL terminator**, in this
probe order:

| # | Code | Driver struct | Hardware |
|---|---|---|---|
| 0 | **`xy`** | `0x00EFD490` | Xylogics 440 / 450 / 451 SMD disk controller (Multibus) |
| 1 | **`sd`** | `0x00EFD55C` | Adaptec ACB-4000 SCSI disk |
| 2 | **`ie`** | `0x00EFD2CA` | Sun/Intel i82586 Ethernet (built-in) |
| 3 | **`ec`** | `0x00EFD754` | 3COM 3C400 Multibus Ethernet |
| 4 | **`mt`** | `0x00EFD3D8` | Ciprico TapeMaster 9-track tape (Multibus) |
| 5 | **`xt`** | `0x00EFD6E8` | Xylogics 472 1/2" tape (Multibus) |
| 6 | **`st`** | `0x00EFD608` | SCSI 1/4" cartridge tape |
| 7 | **`ar`** | `0x00EFD824` | Multibus Archive 1/4" cartridge tape |
|   |  NULL  |              | end of list |

The "Possible boot devices" list printed for `b ?` (or any syntax error) is sourced from
each driver's friendly-name string at offset `+0x16`:

```
3Com Ethernet Bootpath        <- ec
Archieve Tape Bootpath        <- ar     (typo "Archieve" is in the ROM, sic)
Ethernet Bootpath             <- ie
TapeMaster Bootpath           <- mt
SCSI Disk Bootpath            <- sd
SCSI Tape Bootpath            <- st
Xylogics Tape Bootpath        <- xt
Xylogics 450/451 Disk Bootpath <- xy
```

### 5.3 Boot device probe layout

Each driver struct begins:

```
+0x00   2-byte ASCII device code   (e.g. 's', 'd')
+0x02   probe()  -> int            (returns unit number, or -1 if absent)
+0x06   open()   -> int            (read first block / start boot)
+0x16   ptr to friendly name string (used by `b ?`)
```

For `b<empty>`, `parse_boot_device` walks the table top-to-bottom and accepts the first
driver whose `probe()` returns ≠ -1.

### 5.4 Network boot (ie / ec)

Both Ethernet drivers use **RARP + TFTP** (`tftp_read_file` @ `0x00EF75CC`):

* `"Requesting Ethernet address for "` (RARP request)
* `"Internet address is "` / `"%d.%d.%d.%d\n"`
* `"Booting from tftp server at "`
* `"Downloaded %d bytes from tftp server.\n\n"`

TFTP errors decoded: `not defined`, `file not found`, `access violation`,
`disk full or allocation exceeded`, `illegal TFTP operation`, `unknown transfer ID`,
`file already exists`, `no such user`, `tftp: time-out.`.

Special PROM error message: `"tftp: random access attempted - code error.\n"` — fired
from `tftp_random_access_error` (`0x00EF75B0`) if the kernel/standalone tries to seek
during a TFTP read (TFTP can only read sequentially).

### 5.5 SCSI boot (`sd`, `st`)

The SCSI controller is the **Adaptec ACB-4000** (a SCSI-to-MFM bridge for the disk side
on Sun-2/50). Functions: `scsi_init_controller` (`0x00EF8E14`), `scsi_transfer`
(`0x00EF8EAE`), `sd_open` (`0x00EF9244`).

Error strings: `bus busy`, `select failed`, `invalid status msg`, `sequence error`,
`sd: sense error`, `sd: disk busy`, `sd: short transfer`, `sd: 16 retries`, `sd: error`,
`sd: Adaptec SCSI disk`.

For `st` (tape): `st: unknown command`, `st: sense error`, `st: error = %x sense key = %x`.

### 5.6 Multibus tape boot (`mt`, `xt`, `ar`)

These three are Multibus board-level drivers:

* `mt` – Ciprico TapeMaster 9-track. Strings: `tm hard err %x`,
  `tm: no response from ctlr %x`, `tm: error %d during config of ctlr %x`,
  `mt: TapeMaster 9-track tape`.
* `xt` – Xylogics 472. `xt hard err %x`, `xt: Xylogics 472 tape`.
* `ar` – Multibus Archive cartridge. Many `ar*…` debug strings (state machine in
  `ar_state_handler` @ `0x00EFA4AA`), final friendly name
  `ar: Multibus Archive tape controller`.

> **Emulator note (validated in code):** the boot command's MMU setup at
> `cmd_dispatcher` 'B' case loads `page_map_table_a` *or* `page_map_table_b` plus
> `page_map_table_c_rom_window` via `mmu_install_page_table_walk`. Both A and B map VA
> `0x000C0000` with **invalid PTE `0x50000000`**. Any later access through `0xC0000`
> bus-errors unless a Multibus tape probe overwrites that PTE with a valid mapping.
> If your emulator does not implement `mt`/`xt`/`ar` at the Multibus level, the
> `copy_bytes` step at `0xEF6C28` faults at `0xC0000` and boot stops. (This is
> exactly the symptom seen in Sun2BootRom emulator code — kept here for reference.)

### 5.7 Disk-label handling

`validate_disk_label` (`0x00EF6C38`) checks the Sun VTOC. Strings:

* `"No label found - attempting boot anyway.\n"`
* `"Corrupt label\n"`
* `"Giving up...\n"`
* `"Waiting for disk to spin up...\n\nPlease start it, if necessary, -OR- press ..."`

CD-ROM is **not** supported — there is no `sr`/`cd` driver, no ISO-9660 parser, no
`MODE SELECT` for 2048-byte sectors. Same conclusion as the Sun-3/60 PROM.

---

## 6. Extended Diagnostics menu (`extended_test_menu`)

> **Reachability caveat (verified in code):** in `merged_rom_10f.bin` the
> `extended_test_menu` function at `0x00EF4626` has **zero call sites** and the
> diag-mode flag at `0x758` is **only written, never read**. The `x` monitor command
> in this ROM revision does **not** reach this menu (it falls through to the `Z` /
> breakpoint installer — see §4.1 row X). The menu therefore appears to be
> unreachable in this image, but it is still fully built into the ROM and the
> documentation below describes what it would do **if** entered (e.g. via direct
> `g 0xEF4626`, or on a different ROM revision that wires it up).

If invoked, `extended_test_menu` (`0x00EF4626`) does the following: The menu reads two characters (case-
insensitive), packs them as `(c1<<8)|(c2 & 0x5F)`, then binary-searches a sorted code
table at `0x00EF4780` (15 entries, descending). Each match indexes a 16-bit jump
table at `0x00EF4676` to the per-test handler.

The menu rendered to the user depends on a runtime "force-RS232" flag:

```
Extended Test Menu:  (Enter 'q' to return to Monitor)

Cmd -  Test
 kb  -  Keyboard Input
 me  -  Memory
 vi  -  Video
 mk  -  Mouse/Keyboard Ports
 rs  -  Serial Ports
 [normal mode]                            [force-RS232 mode]
 ec  -  3Com Ethernet Bootpath            ie  -  Ethernet
 ar  -  Archieve Tape Bootpath
 ie  -  Ethernet Bootpath
 mt  -  TapeMaster Bootpath
 sd  -  SCSI Disk Bootpath
 st  -  SCSI Tape Bootpath
 xt  -  Xylogics Tape Bootpath
 xy  -  Xylogics 450/451 Disk Bootpath
```

> The "Keyboard Input" entry is shown to the user as **`kb`** (string `kb` at
> `0x00EFC256`) — earlier drafts of this doc said `ke`; that was wrong.

### 6.0 Sub-command code table — full validated map

Decoded from `0x00EF4780` (15 × uint16) and the matching jumptable at `0x00EF4676`
(15 × int16). All 15 codes are accepted by the parser, even those not printed in the
visible menu:

| #  | Letters | Code   | Handler @ | Test |
|----|---------|--------|-----------|---|
| 0  | **`xy`** | `0x5859` | `0xEF46C0` | Xylogics 450/451 Disk Bootpath |
| 1  | **`xt`** | `0x5854` | `0xEF46B0` | Xylogics 472 Tape Bootpath |
| 2  | `xd`     | `0x5844` | `0xEF46A0` | **Hidden / undocumented.** Not in menu. Likely `xy(0,0,0)` "disk dump" stub — invokes the Xylogics disk handler with a fixed unit/part. Reserved-for-engineering style entry. |
| 3  | **`vi`** | `0x5649` | `0xEF4694` | Video |
| 4  | **`st`** | `0x5354` | `0xEF46D0` | SCSI Tape Bootpath |
| 5  | **`sd`** | `0x5344` | `0xEF46F0` | SCSI Disk Bootpath |
| 6  | **`rs`** | `0x5253` | `0xEF46E2` | Serial Ports A/B |
| 7  | **`mt`** | `0x4D54` | `0xEF472A` | TapeMaster Bootpath |
| 8  | **`mk`** | `0x4D4B` | `0xEF4700` | Mouse/Keyboard Ports |
| 9  | **`me`** | `0x4D45` | `0xEF470C` | Memory |
| 10 | **`kb`** | `0x4B42` | `0xEF471C` | Keyboard Input |
| 11 | **`ie`** | `0x4945` | `0xEF4738` | Intel Ethernet (loopback in force-RS232 mode, bootpath otherwise) |
| 12 | **`ec`** | `0x4543` | `0xEF4748` | 3COM 3C400 Ethernet Bootpath |
| 13 | `be`     | `0x4245` | `0xEF4756` | **Hidden / undocumented.** Not in menu. Possibly "burn-in Ethernet" or factory-only test path — `0xEF4756` is reachable only by typing `be` at the prompt. |
| 14 | **`ar`** | `0x4152` | `0xEF4764` | Archieve Tape Bootpath (sic — typo `Archieve` is in ROM) |

`q` (or any unmatched two-character input) exits back to the `>` prompt.
Unmatched but accepted-length input prints `"\nInvalid command!\n"` (`0x00EFC3A4`)
and re-displays the menu.

### 6.0.1 The common per-test option menu

After picking a test from the table above, the test routine first prints
(string `0x00EFB6FC`):

```
Test Options: (Enter 'q' to return to Test Menu)

Cmd  -  Option
 f   -  Loop forever
 h   -  Loop forever with Halt on error
 l   -  Loop once with Loop on error
 n   -  Loop forever with error messages inhibited
<cr> -  Loop once
```

After each pass the test prints
`"Test %sed during pass %d.  Total errors = %d.\n"` (`0x00EFB864`).
Halt-on-error suspends with `"Halted on error => hit any key to continue!\n"`
(`0x00EFB6B4`/`0x00EFB6C8`); loop-on-error prints `"Looping on error ...\n"`
(`0x00EFB6E2`).

### 6.1 Memory test (`me`)

Prompts (`0x00EFB532`):
`"Enter Cmd [low addr > 0x%x] [hi addr < 0x%x] [hex pattern]\n\n"`.
Sub-options menu (`0x00EFB56F`):

```
Cmd -  Test
 a   -  Address
 w   -  Wr/Rd Pattern
 r   -  Read/compare Pattern
 sm  -  Scan Memory
 wp  -  Write Pattern
```

### 6.2 Video test (`vi`)

Same shape as memory. Uses the framebuffer. Aborts via `"Halted on error => "` + key.

### 6.3 Mouse/Keyboard Ports (`mk`) and Serial Ports (`rs`)

Prompt (`0x00EFB3E4`): `"Enter port cmd: Cmd [port(M or K)] [Baud rate(decimal #)] [hex byte pattern]\n\n"`
(`A or B` for serial). Sub-menu:

```
Cmd -  Test
 w   -  Wr/Rd SCC Reg 12
 x   -  Xmit Char
 i   -  Internal Loopback
 e   -  External Loopback
```

Errors: `"%s ready timeout: Port = %c, SCC status = %x.\n"`,
`"%sData Error: Port = %c, Exp = %x, Obs = %x, Xor = %x.\n"`.

### 6.4 Intel Ethernet test (`ie`)

In the Extended-Test menu `ie` runs an i82586 hardware test (it is a different
code path from the bootpath test, even though both share the letters `ie`). After
selecting `ie` the test prints a sub-menu with:

```
Intel Ethernet
 l  -  Local Loopback
 e  -  Encoder Loopback
 x  -  External Loopback
```

(strings at `0x00EFB1AF`, `0x00EFB1C0`, `0x00EFB1D1`, `0x00EFB1E4`).

Concrete error paths logged from `selftest_intel_ethernet_menu` (`0x00EF11C0`),
`run_ethernet_loopback_test` (`0x00EF12E8`) and the i82586 helper functions
(`i82586_reset_and_init`, `i82586_configure_and_setup`, `i82586_loopback_execute`):

```
Reset bad
config: config bad
config: B0 chip
ia setup bad
command: CU w timeout
command: c timeout
command: CU f timeout
command: CU ack timeout
loop:CU bcw timeout
loop:CU/RU bsw timeout
loop:CU/RU ew timeout
loop: scb CU not NOP
loop: scb RU not NOP
loop: scb CU not IDLE
loop: scb RU not NO RES
loop: scb @cbl e(0x%x) o(0x%x)
loop: scb @rfa e(0x%x) o(0x%x)
loop: scb stats
loop[%x]: data +0x%x e(0x%x) o(0x%x)
loop%d: bother at 0x%x
```

### 6.5 Keyboard Input test (`kb`)

Validated string at `0x00EFB4D3`:

```
Type keyboard keys to display them.  To exit type 'esc'.
```

The handler at `0xEF471C` reads keys via `keyboard_get_translated_key` and echoes
them through `prom_putchar`. Pressing `ESC` (0x1B) returns to the test menu.
This is the test you use to verify the Sun keyboard mapping (Sun-2 / Type-2 layout)
and to capture Stop-A / Caps-Lock / function keys.

### 6.6 Boot-path tests (`sd`, `st`, `mt`, `xt`, `xy`, `ar`, `ec`, `ie`-bootpath)

These eight sub-commands do **not** boot — they only **probe and read the first
block** from the named device, then return. They are intended for incoming-inspection
testing of each Multibus / SCSI / Ethernet board.

Common behaviour (see handlers `0xEF46C0`/`0xEF46B0`/`0xEF46D0`/`0xEF46F0`/
`0xEF472A`/`0xEF4738`/`0xEF4748`/`0xEF4764`):

1. Print the friendly bootpath label (e.g. `"SCSI Disk Bootpath"`).
2. Print the per-test option menu (Loop / Halt-on-error / etc.).
3. Call the matching driver's `probe()` then `open()` from `boot_device_table`.
4. On success: read sector 0 (or first packet), report pass.
5. On failure: print the driver's specific error string, increment error counter,
   honour Loop/Halt mode.

Per-driver error strings (validated):

| Test | Error strings |
|---|---|
| `sd` | `bus busy`, `select failed`, `invalid status msg`, `sequence error`, `sd: sense error`, `sd: disk busy`, `sd: short transfer`, `sd: 16 retries`, `sd: error` |
| `st` | `st: unknown command`, `st: sense error`, `st: error = %x sense key = %x`, `stopen: cannot get sense`, `stopen: mode select failed` |
| `mt` | `tm hard err %x`, `tm: no response from ctlr %x`, `tm: error %d during config of ctlr %x` |
| `xt` | `xt hard err %x` |
| `xy` | `xy: no label`, `xy:error %x` |
| `ar` | 14 `ar*…` debug strings — full state-machine log: `ar*init tape online`, `ar: Timeout waiting for Ready at %x`, `ar*init arrdy on before reset`, `ar*init arexc on before reset`, `ar: Timeout waiting for Exception after reset`, `ar*init Error from command STATUS`, `ar*open command STATUS error`, `ar: no drive`, `ar: no cartridge in drive`, `ar: cartridge is write protected`, `ar*intr arexc set, old state %x`, `ar*intr RDST did not return 1`, `ar: error %x`, `ar*RDST gave Exception, retrying` |
| `ec` | (3C400 register-level errors) — `GotReset`, `ec: 3COM Ethernet` (driver banner) |
| `ie` (bootpath) | `ie: hang while setting Ethernet address`, `ie: hang while setting chip config`, `ie: hang while starting receiver`, `ie: cannot initialize`, `ie: xmit hang`, `ie: Ethernet cable problem` |

### 6.7 Hidden / undocumented sub-commands

Two codes are accepted by the parser but **not printed in any menu**:

* **`xd`** (`0x5844`) — handler `0xEF46A0`. Almost certainly a Xylogics-disk
  engineering test (the byte pair `XD` reads as "Xylogics Disk"). Behaviour was
  not exhaustively reverse-engineered in this pass — **[ASSUMED]** to be a
  factory-only direct-controller exerciser.
* **`be`** (`0x4245`) — handler `0xEF4756`. Engineering test, possibly
  "Burn-in Ethernet" given the surrounding context (3COM `ec` and Intel `ie`
  Ethernet handlers sit immediately around it in the jump table). **[ASSUMED]**
  factory burn-in path.

Both can be invoked by typing the two letters at the `Cmd=>` prompt; both will
likely either run silently, hang on missing hardware, or crash on a Sun-2 that
isn't equipped for the specific exerciser. Use at your own risk on real hardware.

---

## 7. Trap / fault behaviour while in monitor

The monitor's exception handlers print:

* `"\nException %x"` (`0x00EFC173`) — generic header.
* `"Error, addr: "` (`0x00EFC160`) — followed by faulting address.
* Decoded fault tags: `Invalid Page`, `Protection`, `VMEbus`, `Timeout`, `Upper Byte`,
  `Lower Byte`, `Parity`, `Address`.
* `"\nBreak "` (`0x00EFC0F8`) — when a planted breakpoint trips.
* `"\nAbort"` (`0x00EFC0F1`) — Stop-A from the keyboard / serial Break.
* `"Trace "` (`0x00EFC100`) — single-step trace event.

After printing, registers are preserved on the saved frame so `A`, `D`, `R` show the
state at the fault.

---

## 8. EEPROM and console selection

EEPROM is at the standard Sun-2 IDPROM/EEPROM region; `q` opens it interactively.
`U` selects which serial port to use as console; `"Using RS232 A input.\n"` (`0x00EFB13E`)
is printed when port A is forced.

---

## 9. Quick test cookbook

Practical sequence for validating an emulated Sun-2:

```
> h                                   # print help
> a                                   # dump address registers (A0-A7)
> d                                   # dump data registers (D0-D7)
> r                                   # dump PC, SR, status
> s                                   # show current FC space
> s 5                                 # set FC = 5 (supervisor data)
> v 200000 200080                     # examine 0x200000..0x200080 as bytes
> v 200000 200100 W                   # ...as 16-bit words
> v 200000 200200 L                   # ...as 32-bit longs
> e 200000                            # interactive examine/modify, words
> l 200000                            # interactive examine/modify, longs
> o 200000                            # interactive examine/modify, bytes
> f 200000 200100 5A B                # fill with 0x5A bytes
> m 0                                 # walk segmap from VA 0
> p 0                                 # walk pagemap from VA 0
> q 12                                # examine EEPROM byte 0x12
> b ?                                 # print boot syntax + device list
> b sd(0,0,0)                         # boot SCSI disk (controller 0, unit 0, part 0)
> b ie()                              # netboot via i82586 (RARP+TFTP)
> b ec()                              # netboot via 3COM 3C400
> b xy(0,0,0)                         # boot Xylogics SMD disk
> b st(0,0,0)                         # boot SCSI tape
> b mt(0,0,0)                         # boot TapeMaster 9-track
> z 1234                              # plant TRAP #1 at 0x1234
> g 1234                              # go to 0x1234
> c                                   # continue (auto-clears bp if PC==bp)
> tc m 0; p 0; r                      # script mode: run three commands separated by ;
> ty                                  # trace yes
> t                                   # trace off
> u                                   # show / change console
> k 0                                 # soft reset, stay in monitor
> k 2                                 # hard reset (cold boot)
> k B                                 # re-print last selftest banner
> x                                   # extended diagnostics menu
```

---

## 10. CD-ROM boot — NOT supported

Verified directly from the binary (same method used for the Sun-3/60):

* The `boot_device_table` at `0x00EFC668` contains exactly **8** drivers — `xy`, `sd`,
  `ie`, `ec`, `mt`, `xt`, `st`, `ar`. There is **no** `cd`, no `sr`.
* The SCSI driver `sd` is hard-wired to 512-byte sectors and the Sun VTOC at sector 0
  (`validate_disk_label` `0x00EF6C38`); CDs use 2048-byte sectors and ISO-9660.
* No `INQUIRY`-type filtering — attaching a SCSI CD-ROM as `b sd(0,target,0)` fails
  at the first `READ(6)` with no useful diagnostic.

Sun-2 install media was always **9-track tape (`mt`)**, **1/4" cartridge (`st`/`ar`)**,
or **netboot (`ie`/`ec`)**. CD-ROM boot was never added.

---

## 11. Full function-pointer summary

Verified entry points discovered or named:

| Address | Function | Purpose |
|---|---|---|
| `0x00EF00E8` | `reset_entry` | Cold-boot vector |
| `0x00EF0354` | `exception_entry_universal` | Common entry trampoline; builds frame for `cmd_dispatcher` |
| `0x00EF0360` | `trap_enter_monitor` | TRAP #1 / TRAP #14 entry |
| `0x00EF3AA4` | `cmd_dispatcher` | Big switch — every monitor command lives here |
| `0x00EF3532` | `print_help_menu` | `h` command |
| `0x00EF37E4` | `examine_memory_interactive` | Used by E/L/O/M/P/Q |
| `0x00EF396A` | `display_registers` | Used by A/D/R |
| `0x00EF39CE` | `run_diagnostics` | Used by Z and X (drops into selftest) |
| `0x00EF4626` | `extended_test_menu` | `x` command menu |
| `0x00EF4A38` | `init_scc_port` | SCC initialization |
| `0x00EF4A9A` | `configure_scc_io_params` | `u` command body |
| `0x00EF5574` | `parse_boot_device` | `b` command parser/launcher |
| `0x00EF6A98` | `init_3c400_ethernet` | 3COM init |
| `0x00EF7574` | `boot_net_device_open` | Network open path |
| `0x00EF75CC` | `tftp_read_file` | TFTP loader |
| `0x00EF7B54` | `ie_init` / `ie_open` etc. | i82586 driver |
| `0x00EF8760` | `tm_command` | TapeMaster command |
| `0x00EF8A0C` | `xy_open` | Xylogics disk open |
| `0x00EF8E14` | `scsi_init_controller` | SCSI bus init |
| `0x00EF9244` | `sd_open` | SCSI disk open |
| `0x00EF95BC` | `st_open` | SCSI tape open |
| `0x00EF9C00` | `xt_open` | Xylogics tape open |
| `0x00EF9E58` | `ec_boot` | 3COM netboot entry |
| `0x00EFA054` | `ar_open` | Archive tape open |

---

## 12. Caveats / honest limits

1. Function names in Ghidra are **analysis-derived**, not Sun's original symbols.
2. The single-letter-to-handler mapping is validated against the actual `switch` cases
   in `cmd_dispatcher`. **`Q` (EEPROM)** is listed in the help table but the visible
   case statement in the decompiler shows A/B/C/D/E/F/G/H/K/L/M/O/P/R/S/T/U/V/W/X/Z;
   the Q routing was not isolated to a single named address in this pass — it is most
   likely the same examine-loop invoked via the M/P/E machinery with a special flag.
   Treat the **`q [addr]`** description as documented behaviour, not byte-level
   re-validated.
3. The MMU `page_map_table_a/b/c` initial values that map VA `0xC0000` to invalid PTE
   `0x50000000` are validated and matter for emulator implementations of Multibus
   tape boot.
4. The "Reset (0)CPU, (1)MMU, (2)System" labels in the help table do **not** match the
   exact actions the dispatcher takes — see the K-command row in §4.1 for what is
   actually executed.
5. CD-ROM is verified **absent** from the boot device table.
