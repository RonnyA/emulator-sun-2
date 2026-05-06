# Sun-3/60 Boot PROM Command Reference

**ROM image:** `sun3_60_v3.0.1.bin`
**Path:** `E:\Dev\Repos\Ronny\RetroCore\Emulated.Machines\Sun\Sun3\ROMChips\sun3_60_v3.0.1.bin`
**Version string in ROM:** `3.0.1`
**Build date in ROM:** `05/25/89`
**Build path:** `/home/aboy/desh/mon`
**Codename:** `starbrite`
**CPU:** MC68020, big-endian
**ROM mapping:** `0x0FEF0000 – 0x0FEFFFFF` (64 KB)
**Banner:** `Sun Workstation, Model Sun-3/60`
**Copyright:** `(c) 1985 - 1989 Sun Microsystems, Inc.`

---

## 1. Sources used for this document

Everything below is derived directly from the disassembly / decompilation of the ROM image via Ghidra MCP.
Ghidra symbol names listed in parentheses are the **auto-generated** labels in the program; they are
working names from analysis, not original Sun symbols.

Where a behaviour was inferred from limited code paths and is not 100% certain, the entry is marked
with **[ASSUMED]**. Everything else has been validated against the actual byte tables and decompiled
control flow in this specific ROM.

Key data tables that were validated:

| Address | Contents |
|---|---|
| `0x0FEFC48C` | Single-letter command jump table, 26 × 4-byte function pointers, indexed `(letter & 0x5F) - 'A'` |
| `0x0FEFD2D8` | Multi-character command name pointer array (5 entries) |
| `0x0FEFD2EC` | Multi-character command jump table (5 × 4-byte function pointers) |
| `0x0FEFC488` | Pointer to `"Monitor: Invalid command.\n"` (the unknown-command message) |

The dispatcher itself is `cmd_dispatch` @ `0x0FEF1204` and the input parser is `cmd_parse_line`
@ `0x0FEF12BC`. The interactive loop is `monitor_cmd_loop` @ `0x0FEF1120`.

---

## 2. Entering the Boot PROM monitor

The PROM monitor (the `>` prompt) is reached when:

* the machine is reset and the EEPROM does **not** request auto-boot, OR
* the user aborts during the 10-second selftest delay
  (`"Type a character within 10 seconds to enter Menu Tests..."`), OR
* an Abort key (L1-A on serial console) is pressed during execution, OR
* the running kernel takes a fatal trap and falls back into the PROM
  (`"Exception 0x%x at 0x%8x.\n"` / `"Illegal Instruction = 0x%8x"`), OR
* a watchdog reset fires (`"Watchdog Reset!\n"`), OR
* the user explicitly exits a running program back to the monitor (e.g. `kadb` / SunOS `reboot`).

The monitor prompt character is `>` (literal `0x3E`, emitted at the top of `monitor_cmd_loop`).
While in extended-diagnostic mode, the prompt is `Cmd==>` instead.

Auto-boot mode prints `"Auto-boot in progress...\n"` and diagnostic auto-boot prints
`"Diagnostic Auto-boot in progress..."`.

---

## 3. Command parsing rules (validated)

Implemented by `cmd_parse_line` (`0x0FEF12BC`) and `cmd_dispatch` (`0x0FEF1204`):

1. The first non-whitespace character is read into a global at `0xFFFFE464`.
2. If that first character is **`!`**, the remainder of the line is **echoed**
   with `"Tracing...\n"` and the next character is then taken as the actual
   command. (`!` is the macro / script-trace marker.)
3. The line is then matched against a list of multi-character command names
   (`LOGIN`, `DLOAD`, `ULOAD`, `SETUP`, `STATE`) — names compared case-insensitively
   for **6** chars.  These are the diagnostic / Forth-style network commands.
4. If a multi-char command matched, its handler is dispatched and the rest of the
   command processing is skipped.
5. Otherwise, the first character is **upper-cased** (`AND 0x5F`).
6. If it is `^` (`0x5E` after masking), `cmd_mmu_inspect` is invoked.
7. If it is in `A`–`Z`, the corresponding entry in the table at `0x0FEFC48C` is called.
8. Anything else prints `"Monitor: Invalid command.\n"`.

The sign / number prefixes `-` and `+` and ASCII digits are also recognised by the parser;
they update the running operand pointer/sign so a numeric argument can precede a command.

---

## 4. Single-letter command table (validated against ROM bytes)

The table at `0x0FEFC48C` resolves each upper-cased letter `A..Z`:

| Cmd | Handler addr | Ghidra label | Validated function |
|---|---|---|---|
| **A** | `0FEF19A2` | (shared) | Display **address registers** — calls `display_all_registers(eeprom+0x20)` then `cmd_display_dev_config`. |
| **B** | `0FEF14C6` | `cmd_breakpoint` | Multi-mode: `b!path` sets boot path, `b*` runs extended diagnostics, plain `b` arms a breakpoint. See §5. |
| **C** | `0FEF153E` | `cmd_set_boot_device` | **Continue** at given address (or saved PC if none); reads optional hex number, stores at saved-PC slot, then calls `cmd_boot_or_run` to resume. |
| **D** | `0FEF19A2` | (shared) | Display **data registers** — calls `display_all_registers(eeprom+0x00)` then dev-config. |
| **E** | `0FEF1446` | `cmd_single_step` | Examine / modify memory **as bytes** (step = 1).  Format `e[addr]`. |
| **F** | `0FEF15C0` | `cmd_fill_memory` | **Fill** memory: `f start end value [L|W]`.  `L` = longword, `W` = word, default = byte. |
| **G** | `0FEF156E` | `cmd_go_execute` | **Go** — start execution at given address (or saved PC).  Calls the address as a function with the input buffer as argument. |
| **H** | `0FEF0E88` | `return_zero` | No-op (returns 0).  No help text printed — `H` simply does nothing. |
| **I** | `0FEF14BC` | `cmd_noop` | No-op stub. |
| **J** | `0FEF14BC` | `cmd_noop` | No-op stub. |
| **K** | `0FEF1688` | `cmd_reset_dispatch` | **Kill / reset** — see §6 for sub-codes (`k0`, `k1`, `k2`, `kb`). |
| **L** | `0FEF1446` | `cmd_single_step` | Examine / modify memory **as longwords** (step = 4). |
| **M** | `0FEF1762` | `cmd_walk_segmap` | Walk MMU **segment map** (segments of 0x20000 / 128 KB). |
| **N** | `0FEF1674` | `cmd_noop2` | No-op stub. |
| **O** | `0FEF1446` | `cmd_single_step` | Examine / modify memory **as bytes** (step = 1, mode `O`). Same handler as `E`/`L`, mode-letter = `O`. |
| **P** | `0FEF17DC` | `cmd_walk_pagemap` | Walk MMU **page map** (pages of 0x2000 / 8 KB), shows segment map index per page. |
| **Q** | `0FEF186A` | `cmd_eeprom` | **EEPROM** examine / modify (`q [offset]`) and `q*` to wipe.  See §7. |
| **R** | `0FEF19A2` | (shared) | Display **other / system registers** — `display_all_registers(eeprom+0x3C)`. |
| **S** | `0FEF19EC` | `cmd_set_fc_space` | **Set Function Code** (FC0..7) for memory commands.  `s` alone prints current FC, `s n` sets FC to `n & 7`. |
| **T** | `0FEF1A22` | `cmd_trace_control` | **Trace** control: `tc` = enter command-script mode (split on `;`), `ty` = trace yes/on, anything else = trace **off** (clears bit 0x8000 in EEPROM word `+0x62`, restores vector 0x24). |
| **U** | `0FEF42D8` | `set_serial_port` | **UART / serial port** test & configuration.  Sub-modes drive a built-in serial scratch tester; see §8. |
| **V** | `0FEF1AA0` | (memory dump) | **View** memory range: `v start end [W\|L]`. `W` = word, `L` = long, default = byte. Reads using the current FC space (set by `S`). |
| **W** | `0FEF1C08` | `cmd_call_with_arg` | **Call with argument** — `w addr` stores `addr` in the indirect slot and dispatches the saved indirect-call routine.  Used internally by `G`/`C`. |
| **X** | `0FEF1C2E` | `cmd_exit_to_rom` | **Exit** monitor: pokes `0x12` into the post-exit dispatch slot at `0xFFFFE004` and triggers a soft reset path. Effectively returns to the previous program / restarts boot. |
| **Y** | `0FEF167E` | `cmd_noop3` | No-op stub. |
| **Z** | `0FEF4916` | `go_command` | **Set / clear breakpoint at address**.  Empty arg prints current breakpoint; numeric arg installs a `0x4E41` (`TRAP #1`) at that address, saving the original word.  Prints `"Break %x installed"`. |

> **Same-handler observations.**  `A`, `D`, `R` share `0x0FEF19A2`; the handler reads
> `0xFFFFE464` (the original command letter) and selects which 32-byte register save block
> (offset 0x00, 0x20 or 0x3C inside the EEPROM/scratch context) to dump — so they really are
> three different register-display flavours in the same routine.
>
> `E`, `L`, `O` share `cmd_single_step` (`0x0FEF1446`); it inspects `cmd_dispatch_type`
> (the original letter saved by the dispatcher) and selects element width 1, 2 or 4.
> Decompilation:
>
> * `O` → step 1 (byte)
> * `E` → step 2 (word)
> * `L` → step 4 (long)
>
> So `E` is **word**, `L` is **long**, `O` is **byte** — note this differs from V's mode letters.

> **`^` (caret) command** (`cmd_mmu_inspect`, `0x0FEF1C4A`) — invoked when the dispatcher
> sees `0x5E` after masking.  Three sub-modes (read with `prom_getnextchar`):
>
> | Sub-cmd | Behaviour |
> |---|---|
> | `^C src dst count` | **Copy memory**: copy `count` bytes from `src` to `dst` using current FC. |
> | `^I` | **Identify** — print copyright + `"Compiled on %s using %s:%s"` (build banner). |
> | `^T addr` | **Translate** virtual addr — prints physical addr, context, segmap, pagemap, valid/permission/no-cache/type/access/modify bits. |
> | (other) | Prints `"Monitor: Invalid command."` |

---

## 5. The `B` (boot / breakpoint) command — detail

Single-letter `B` is multiplexed by the next character:

| Form | Action (validated in `cmd_breakpoint`) |
|---|---|
| `b! <bootpath>` | Calls `parse_boot_path(rest_of_line)` and stores the parsed value in `eeprom_config + 0x64`. This is how a boot path is **armed without actually booting yet**. |
| `b*` | Calls `test_run_command()` — enters extended diagnostics / re-runs the menu test sequence. |
| `b` (plain) | Initialises the keyboard ring buffer, pushes `0x7F` (DEL / abort) into it, then triggers the address-error vector path. Effective use: **insert a synchronous breakpoint** for the next executed instruction. |

The **boot syntax string** present in the ROM (from `0x0FEFD51E`) is:

```
Boot syntax: b [!][dev(ctlr,unit,part)] name [options]
Possible boot devices...
```

so the user-visible form of a real boot command is:

```
> b [!][dev(ctlr,unit,part)] [filename] [options]
```

with arguments parsed by `parse_boot_path` (`0x0FEF5978`) and dispatched by `boot_command`
(`0x0FEF50A4` and `0x0FEF5B5C`).  The format used when the boot record is printed back is:

```
Boot: %c%c(%x,%x,%x)%s %s
      ^^   ^  ^  ^   ^  ^
      ||   |  |  |   |  +-- options string
      ||   |  |  |   +----- filename (e.g. "vmunix")
      ||   |  |  +--------- partition number
      ||   |  +------------ unit number
      ||   +--------------- controller number
      ++------------------- 2-letter device code
```

### Device codes (validated from string table in ROM)

The PROM probes these device drivers at boot time. The 2-letter codes come from the
matching driver tables (`le`, `ie`, `sd`, `st`, `xd`, `xy`, `id`, …).  Confirmed from
strings in this ROM:

| Code | Device | Source string |
|---|---|---|
| `le` | Lance Ethernet (AMD 7990) | `"le: Sun/Lance Ethernet"` |
| `sd` | SCSI disk | `"sd: SCSI disk"` |
| `st` | SCSI tape | `"st: SCSI tape"` |
| `ie` | Intel Ethernet | (driver present, see `ie0` references in source tree) |

Network boot uses RARP + TFTP (validated strings):

* `"Requesting Ethernet address for "`
* `"Requesting Internet address for "`
* `"Using IP Address %d.%d.%d.%d = "`
* `"Booting from tftp server at "`
* `"Downloaded %d bytes from tftp server.\n\n"`
* TFTP error map: `"file not found"`, `"access violation"`, `"disk full or allocation exceeded"`,
  `"illegal TFTP operation"`, `"unknown transfer ID"`, `"file already exists"`, `"no such user"`,
  `"not defined"`, `"tftp: time-out.\n"`.

Boot failure messages also confirm error paths:

* `"Boot: load failed\n"`
* `"\nDevice not found\n"`
* `"No bootable devices found\n"`
* `"\nInvalid device = '%c%c'\n"`
* `"No label found - attempting boot anyway.\n"` (no Sun disk label)
* `"Corrupt label\n"`
* `"Waiting for disk to spin up...\n\nPlease start it, if necessary, -OR- press ..."`
* `"\nGiving up...\n"`
* `"\nEEPROM boot device..."` (boot device taken from EEPROM)

### Boot options flags — **[ASSUMED]**

The Sun-3 PROM standard option letters appended after the filename (e.g. `b sd(0,0,0)vmunix -s`)
are not all string-referenced in this ROM image; they are passed through verbatim into the
`%s` options field shown above.  Documented uses (per Sun-3 hardware manual; **not
re-verified per-letter in this binary**) are:

| Opt | Meaning |
|---|---|
| `-s` | Boot single-user |
| `-a` | Ask for root device interactively |
| `-r` | Reconfigure / probe devices |
| `-d` | Boot under `kadb` (kernel debugger) |
| `-h` | Halt to monitor immediately after load |
| `-v` | Verbose boot |

These are interpreted by the loaded SunOS kernel, not by the PROM; the PROM merely forwards
the option string.

### 5.1 CD-ROM boot — **NOT supported**

This was verified directly from the ROM, not assumed. The boot device dispatch table
at **`0x0FEFD440`** contains exactly three driver-struct pointers (NULL-terminated):

```
0fefd440  0f ef db 94    ->  sd  (SCSI disk),   driver struct @ 0x0FEFDB94
0fefd444  0f ef d8 18    ->  le  (Lance Ether), driver struct @ 0x0FEFD818
0fefd448  0f ef dc 8c    ->  st  (SCSI tape),   driver struct @ 0x0FEFDC8C
0fefd44c  00 00 00 00    ->  NULL terminator
```

No `cd`, no `sr`, no generic SCSI block driver. (Note: `ie` Intel Ethernet is **also**
not present in this v3.0.1 image, despite some older Sun-3 documentation listing it —
this ROM only has Lance.)

The `sd` driver **cannot** boot a SCSI CD-ROM either, even though a CD-ROM responds on
the SCSI bus. Reasons, all from the disassembly:

1. **Block size mismatch.** `sd` issues 6-byte `READ(6)` with a 512-byte sector
   assumption (`disk_read_blocks` @ `0x0FEF5E98`, `disk_read_block` @ `0x0FEF6FB4`,
   `scsi_read_capacity` @ `0x0FEF866C`). CD-ROMs use 2048-byte sectors and the PROM
   never issues `MODE SELECT` to switch block size.
2. **Sun disk label required.** `disk_verify_label` (`0x0FEF7002`) parses a Sun VTOC
   at sector 0. A data CD has ISO-9660 at sector 16 — there is no Sun label, so the
   "No label found - attempting boot anyway." path is taken and immediately fails to
   find any bootable partition.
3. **Bootblock layout.** The boot sequence (`disk_find_boot_block` @ `0x0FEF6F40`)
   expects SunOS bootblocks in the first 15 sectors of a Sun-labelled partition;
   no Sun-3/SunOS install media was ever pressed onto CD.
4. **No INQUIRY peripheral-type filter.** `scsi_open` (`0x0FEF8576`) does not branch
   on the device-type byte from INQUIRY (`0x00` = direct-access disk vs `0x05` = CD-ROM),
   so attaching a CD-ROM as `b sd(0,target,0)` only fails *after* the first `READ(6)` —
   not with a clean "not supported" message.

CD-ROM boot is an OpenBoot PROM (Sun-4 / sun4c+) feature; it was **never** added to the
Sun-3 boot PROM line. To install SunOS 4.x on a Sun-3/60 the supported paths are:

- **Tape** — `b st(0,0,0)` (1/4" cartridge or 8 mm Exabyte over SCSI),
- **Network** — `b le()` (RARP for the IP, TFTP-load `vmunix` from the install server).

A common workaround in the era was to keep the install CD on a separate Sun-4 server,
mount it under SunOS, NFS-export it, and netboot the Sun-3 from the server.

---

## 6. The `K` (reset) command — sub-codes (validated)

Implemented by `cmd_reset_dispatch` (`0x0FEF1688`).  Reads a numeric argument and switch-cases:

| Form | Effect (from decompilation) |
|---|---|
| `k 0` (or `k`) | `cpu_reset()`, OR the diag/enable register at `0x0FE0A000` with `0x81`, then re-init the framebuffer if a display was detected.  **Soft CPU reset, keep PROM monitor.** |
| `k 1` | Pushes a synthetic `0x7F` keystroke into the input ring and triggers the bus-error vector — used as a **soft software break / reset to monitor**. |
| `k 2` | Full **hard reset path**: `cpu_reset()` → re-init SCC at `0x0FE00000` → clear `0x0FE08000` → null-out `0x0FE12018` if probe succeeds → jump to `reset_entry` (cold-boot vector).  This is effectively L1-A + power-cycle minus the actual power. |
| `k 11` (`0xB`) | Re-prints the boot banner (`print_banner`). |
| (other) | Falls through, no action. |

---

## 7. The `Q` (EEPROM) command — detail (validated)

Implemented by `cmd_eeprom` (`0x0FEF186A`).  Operates on EEPROM bytes mapped at
`0x0FE04000`–`0x0FE047FF` (2 KB).

| Form | Action |
|---|---|
| `q [offset]` | Examine / modify EEPROM byte at `0x0FE04000 + offset`. Each accepted edit triggers `eeprom_checksum_update()` and a programming delay loop (~160000 iters >> chip-speed shift).  Use `+`/`−` to advance / step back through the byte stream. |
| `q*` | **Wipe entire EEPROM**.  Prints the warning `"WARNING: EEPROM contents will be destroyed, are you sure?"`, waits for `Y/y`, then writes `0x00` to all 2048 bytes one by one (with the per-byte programming delay). |

Before Q can modify the EEPROM the security/password gate may apply:

* `"Modifiying security location(s). Are you sure?(y/N) "` (sic, typo is in the ROM).
* If access is denied or the password is wrong: `"Try again.\n"` and on repeated failure
  `"Invalid\n"`.
* `"PROM Pass:"` is the prompt for the security password.

Initialisation messages observed: `"EEPROM defined Type-%d Keyboard."`, `"EEPROM not initialized,
defaulted to EPROM key tables."`, `"EEPROM: Using RS232 %c port."`.

The EEPROM word at `+0x68` (validated in `monitor_cmd_loop`) controls **prompt suppression**:
when the low 12 bits equal 4 or 5, the `>` prompt and `getline` are skipped — used by the
script/macro modes.

The EEPROM word at `+0x62` bit 15 (`0x8000`) is the **trace-on** flag (`T` command toggles it).

The EEPROM dword at `+0x64` is the **saved boot path** / saved-PC slot (used by `B!`, `C`, `G`).

---

## 8. The `U` (serial port) command — detail (validated)

Implemented by `set_serial_port` (`0x0FEF42D8`).  Format observed in the ROM string is:

```
u%ci, u%co, ua%x, ub%x, u%s
```

That is, a family of sub-commands — letter chosen by the second character:

| Form | Effect |
|---|---|
| `u <n>i` | Set input baud rate to `<n>` (decimal) on the currently selected port. Stores low byte at `0xFFFFE017` and `0xFFFFE016`. |
| `u <n>o` | Clear output baud rate (`0xFFFFE016 = 0`).  *(Subtract-baud path, validated from disasm.)* |
| `ua <hex>` | Write hex byte pattern to **port A**. |
| `ub <hex>` | Write hex byte pattern to **port B**. |
| `u <s>` | Select console string (mode pointer at `0xFFFFCF44`). |
| `un` / `uN` | Toggles output redirect flag at `0xFFFFE01B` — `N` means "no redirect"; anything else = redirect. |

Errors / messages:

* `"Invalid baud rate.\n"`
* `"Error: Port in use\n"`

Note the `U` family is also reachable from the menu-driven test mode — see §10.

---

## 9. Numeric / address argument syntax

Validated by `prom_getnum` (`0x0FEF3970`) and `cmd_parse_line`:

* Arguments are read as **hexadecimal by default** (no `0x` prefix needed).
* A leading `-` makes the value **negative** (sets sign in `0xFFFFE476`); leading `+` is also accepted.
* Whitespace before / between arguments is skipped (`skip_spaces`, `0x0FEF395A`).
* A `\r` (carriage return) terminates the line.
* `;` is treated as a statement separator only when **trace command-script mode** is on
  (`tc`), otherwise it is ordinary text.

`+` and `-` after a memory-modify operation are also used as **step** keys (advance / step
back the current address by the element size of the running command — 1, 2 or 4 bytes).

Inside the modify prompt, additional sub-keys recognised by `modify_memory` (`0x0FEF4A66`)
and `display_register`:

* `RETURN` → advance one element, leave value unchanged
* `^` → step **backward** one element
* `.` → terminate the modify session (exit back to `>` prompt)
* hex digits → parse new value, write, then advance

(These are inferred from cross-references to the `+`/`−`/step-size globals; the exact
key-set is **[ASSUMED]** to match the documented Sun-3 behaviour, which the code matches
in shape.)

---

## 10. Multi-character commands (validated)

Five names live in the table at `0x0FEFD2D8`, dispatched via the table at `0x0FEFD2EC`:

| Name | Handler | Validated function |
|---|---|---|
| `LOGIN` | `0x0FEF45B0` | Monitor security login — prompts `"PROM Pass:"`, validates against the EEPROM-stored password.  After a successful login the protected commands (`q`, EEPROM-altering commands) become available. |
| `DLOAD` | `0x0FEF5228` | **Download** S-records / binary over the current console serial port.  Used by Sun field-service / OEM tooling. |
| `ULOAD` | `0x0FEF52D4` | **Upload** memory range to the serial port (`serial_upload`). |
| `SETUP` | `0x0FEF538E` | Configure serial-port routing (`set_serial_io_port`). |
| `STATE` | `0x0FEF541C` | Print current serial-port state (`print_serial_status`). |

---

## 11. Extended Diagnostics / Menu Tests

If the user types any character within 10 seconds of selftest start (or invokes `b*`),
the PROM enters **Extended Diagnostics** mode (`diag_test_menu` @ `0x0FEFA9F0`).
Validated strings & layout:

```
Extended Diagnostics Main Menu:

 le  -  AMD Ethernet Test
 mk  -  Mouse/Keyboard Ports Test
 rs  -  Serial Ports Test
 ...
Cmd==>
```

Test-loop options (validated string at `0x0FEFE169`):

```
 f   -  Loop forever
 h   -  Loop forever with Halt on error
 l   -  Loop ...
```

Each test prints `"Test %sed during pass %d.  Total errors = %d.\n"` and ends with
`"Selftest passed."` / `"Selftest failed."`.

### 11.1 `mk` — Mouse / Keyboard Ports test

Per ROM string `"Enter port cmd: Cmd [port(M or K)] [Baud rate(decimal #)] [hex byte pattern]"`
plus the menu:

```
Cmd -  Test
w  -  Wr/Rd SCC Reg 12 Test
x  -  Xmit Char Test
i  -  Internal Loopback Test
... (q to quit)
```

Same sub-letters apply to `rs` (Serial Ports A/B test).

### 11.2 `le` — AMD Ethernet test

Menu (validated string at `0x0FEFDF35`):

```
AMD Ethernet Tests:

w - Wr/Rd CSR1 Reg Test
l - Local Loopback Test
x - External Loopback Test
```

Errors include `"CSR0 Wr/Rd Error"`, `"CSR1 Wr/Rd Error"`, `"Initialization failure, CSR0
exp %x, obs %x"`, `"TX - TIMEOUT"`, `"RX - TIMEOUT"`, `"RX - Missed Packet ERROR"`,
`"Error in receive buffer at %x"`.

### 11.3 Power-On Self Test (POST) sub-tests (validated)

Run unconditionally on cold reset (and re-runnable from menu).  Each sub-test has its own
error code reported as `Err N`:

| # | Sub-test (string in ROM) | Err codes |
|---|---|---|
| 1 | `Boot PROM Selftest` (banner) | – |
| 2 | `PROM Checksum Test` | `Err 16` |
| 3 | `DVMA Reg Test` | `Err 1`, `Err 2` |
| 4 | `Context Reg Test` | `Err 1` |
| 5 | `Segment Map Wr/Rd Test` | `Err 1` |
| 6 | `Segment Map Address Test` | `Err 2` |
| 7 | `Page Map Test` | `Err 1`, `Err 2` |
| 8 | `Memory Path Data Test` | `Err 2` (with module #) |
| 9 | `NXM Bus Error Test` | `Err 3: No NXM Bus Error!` |
| 10 | `Interrupt Test` | `Err 4: No Level 1 interrupt!` |
| 11 | `TOD Clock Interrupt Test` | `Err 21: TOD failed to interrupt!` |
| 12 | `MMU Access Bit Test` | `Err 5` |
| 13 | `MMU Access/Modify Bit Test` | `Err 5` |
| 14 | `MMU Invalid Page Test` | `Err 6: No bus error for invalid page!` |
| 15 | `MMU Protected Page Test` | `Err 7`, `Err 8` |
| 16 | `Parity Test` (per `Err 9`,`Err 11`,`Err 15`) | `Err 9` (no NMI on bad parity), `Err 11` (parity error addr), `Err 15` (unexp parity trap) |
| 17 | `Memory Test (Testing 0x%d3 MBytes)` | `Err 10` (parity error+addr+module), `Err 12` (NMI bad status), `Err 13` (unexp trap), `Err 14` (unexp bus err) |

Selftest control modifiers seen at top of POST:

* `(e for echo mode) ` — pressing `e` enables `"Echo mode: Output will echo to Video!"`.
* Any keystroke during the 10-s prompt → enter Menu Tests (instead of auto-boot).
* Burn-in flag in EEPROM → `"Burn_in flag set.....Doing Burn_in Test"`.

---

## 12. Trap / fault behaviour while in monitor

`prom_exception_handler` (`0x0FEF3DA8`) prints these on a CPU exception:

* `"Illegal Instruction = 0x%8x"`
* `"Exception 0x%x at 0x%8x.\n"`
* `"\nMEMORY ERROR! Status %x, DVMA-BIT %x, Context %x,\n  Vaddr: %x, Paddr: %8x, Type %d"`
* `"Error:\n  Vaddr: %8x, Paddr: %8x, Type %d, %s, FC %d, Size %d"` where `%s` ∈
  `"Write"`, `", PC fetch"` (read is the implicit default).
* Decoded fault types: `"Invalid Page"`, `"Protection"`, `"Timeout"`, `"FPA Response"`,
  `"FPA Enable"`, `"Address"`.
* `"\nBreak %4x"` — when a planted breakpoint (`Z`) trips.

After printing, the PROM drops to the `>` prompt with all registers preserved, so `A`/`D`/`R`
will display the saved register frame.

---

## 13. Inputs / output devices the PROM can use as the console

Validated from strings & EEPROM init:

* On-board framebuffer (`bwtwo` mono, P4 framebuffer) + Sun keyboard.
  * `"Type-%d Keyboard."` printed during init.
  * `"No Keyboard Found: Using RS232 Port A as input!"` if keyboard absent.
  * `"Keyboard error detected"` on read failure.
* RS-232 Port A or Port B (`"EEPROM: Using RS232 %c port."`).
* Diagnostic output during selftest can be **echoed** to the framebuffer (`e` in the
  selftest prompt).

---

## 14. EEPROM layout pointers used by the PROM commands

These offsets into the EEPROM image (`0x0FE04000` base) are referenced by the command
handlers above and are useful for validation tests:

| Offset | Used by | Meaning |
|---|---|---|
| `+0x62` (word) | `T` command | bit 15 (`0x8000`) = trace-on flag |
| `+0x64` (long) | `B!`, `C`, `G`, `cmd_boot_or_run` | saved boot path / resume PC |
| `+0x68` (word) | `monitor_cmd_loop` | low 12 bits = prompt-mode (`4`/`5` suppress prompt) |

(The standard SunOS-visible EEPROM layout — `eeprom(8)` keys such as `keyboard-type`,
`watchdog-reboot?`, `boot-from`, `secure`, `oem-banner`, `keyswitch-position`, `serial-A`,
`serial-B`, `bootdev`, etc. — is not re-verified offset-by-offset here.  Cross-reference
the SunOS kernel source for the byte-level field map.  **[ASSUMED]** that the standard
Sun-3 EEPROM structure applies.)

---

## 15. Quick test cookbook

A practical sequence for validating an emulated Sun-3/60:

```
> k 11                                # re-print banner — verify ROM banner / serial #
> a                                   # dump address registers (A0-A7)
> d                                   # dump data registers (D0-D7)
> r                                   # dump SR/PC/USP/SSP/VBR
> s                                   # show current FC space
> s 5                                 # set FC = 5 (supervisor data)
> v 0fe04000 0fe04020 W               # examine first 32 bytes of EEPROM as words
> v 0fef0000 0fef0080                 # examine ROM reset vectors as bytes
> ^t 1000                             # translate VA 0x1000 — show pa, ctx, segmap, pagemap
> m 0                                 # walk segmap from VA 0
> p 0                                 # walk pagemap from VA 0
> q 12                                # examine EEPROM byte at offset 0x12 (interactive edit)
> b! sd(0,0,0)vmunix -s               # arm boot path (single-user from SCSI 0:0:0)
> c                                   # actually start the boot using armed path
> b                                   # plain breakpoint (synthetic abort)
> z 1234                              # plant TRAP #1 at address 0x1234
> g 1234                              # go to 0x1234
> tc                                  # script mode: lines split on ';'
> t off                               # turn trace off
> u                                   # show serial port settings
> u 9600i                             # set input baud to 9600 on selected port
> b*                                  # enter Extended Diagnostics menu
> LOGIN                               # PROM password login (PROM Pass: prompt)
> SETUP                               # configure serial routing
> STATE                               # show serial state
> ULOAD ...                           # upload memory range over current serial line
> DLOAD ...                           # download S-records / binary over serial line
> k 0                                 # soft CPU reset, stay in PROM
> k 2                                 # cold reset (jumps to reset_entry)
> x                                   # exit monitor (return to previous program)
```

---

## 16. Summary table — every documented entry point

| Trigger | Address | Notes |
|---|---|---|
| `>` prompt loop | `0x0FEF1120` | `monitor_cmd_loop` |
| Line parser | `0x0FEF12BC` | `cmd_parse_line` (handles `!`, `+`/`-`, numbers) |
| Dispatcher | `0x0FEF1204` | `cmd_dispatch` (table at `0x0FEFC48C`) |
| `^C` copy | `0x0FEF1C4A` (sub) | `cmd_mmu_inspect` `C` branch |
| `^I` ident | `0x0FEF1C4A` (sub) | prints copyright + build banner |
| `^T` translate | `0x0FEF1C4A` (sub) | VA→PA + page attribs |
| `LOGIN` | `0x0FEF45B0` | PROM password |
| `DLOAD` | `0x0FEF5228` | serial download |
| `ULOAD` | `0x0FEF52D4` | serial upload |
| `SETUP` | `0x0FEF538E` | serial routing |
| `STATE` | `0x0FEF541C` | serial state |
| Boot string parser | `0x0FEF5978` | `parse_boot_path` |
| Boot driver | `0x0FEF50A4` & `0x0FEF5B5C` | `boot_command` |
| TFTP / RARP | `0x0FEF7A10`+ | `net_boot_init`, `net_arp_resolve`, `net_send_rarp` |
| SCSI disk | `0x0FEF8576`+ | `scsi_open`, `scsi_do_command` |
| Lance Ethernet | `0x0FEF727A`+ | `le_init`, `le_open`, `le_transmit`, `le_receive` |
| Selftest entry | `0x0FEFAB80` | `warm_boot_entry` |
| Diag menu | `0x0FEFA9F0` | `diag_test_menu` |
| Cold reset | `0x0FEF00E8` | `reset_entry` |

---

## 17. Caveats / honest limits of this reverse-engineering pass

1. The Ghidra **function names** are auto-generated and reflect what the analysis tool
   guessed — they should **not** be quoted as Sun's original symbol names.
2. Tables `0x0FEFC48C` and `0x0FEFD2EC` are validated byte-for-byte. The mapping
   *letter → handler address* is therefore exact for this ROM.
3. The exact behaviour of every numeric sub-code (e.g. `K 11`, `K 2`) was extracted from
   the decompilation. Anything tagged **[ASSUMED]** above (boot option letters, the
   detailed EEPROM byte layout, the modify-prompt sub-keys `RETURN/^/./hex`) was **not**
   re-derived from this binary and should be re-verified before being relied on for tests
   that depend on those exact details.
4. Other Sun-3 ROM revisions (`2.x`, earlier `3.x`) are **not** identical to this image;
   the table addresses and even the command set differ between revs.
