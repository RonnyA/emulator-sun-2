# emulator-sun-2

Emulator for the Sun-2 workstation. Boots SunOS 2.0 / 3.2 / 3.5 and
emulates the bwtwo display, keyboard, SCSI, and 3C400 ethernet of an
original Sun-2.

This started as Brad Parker's project to learn the MMU and SCSI well
enough to eventually build an FPGA Sun-2. SDL2 frontend and an
alpha-quality 3C400 ethernet driver were added by Sigurbjorn B.
Larusson.

It now boots SunOS 2.0, 3.2, and 3.5 cleanly. SunOS 2.0 boots
multiuser. SCSI tape emulation is still shaky.

Tape boot through the standalone copy program is silent (no `st: short transfer`
warnings) — see `Docs/sun-scsi-tpboot.md` for the controller-side rules.

## Quick start

```sh
make run                  # build, stage SunOS 3.2 disk + tape, launch
make run RUN_VERSION=20   # SunOS 2.0
make run RUN_VERSION=35   # SunOS 3.5
```

After the PROM banner appears in the SDL window, type a boot command
at the `>` prompt — for example `b sd(0,0,0)vmunix` or just
`b vmunix`.

Other targets:

```sh
make            # build only
make sunos32    # stage SunOS 3.2 disk + tape (no run)
make sunos20    # stage SunOS 2.0 disk + tape (no run)
make sunos35    # stage SunOS 3.5 disk (no run)
make run-trace  # run with full bus-error / vector trace (debugging only)
make clean      # remove build artifacts
make help       # show every target
```

## Platforms

The build runs on Linux, macOS, NetBSD/FreeBSD/OpenBSD, and Windows
(via MSYS2 MinGW-w64 or w64devkit). Pre-built release binaries for
Linux x64, macOS arm64, and Windows x64 are produced by GitHub Actions
on every tag push.

### Linux

```sh
sudo apt install build-essential pkg-config libsdl2-dev libpcap-dev
make
```

### macOS

```sh
brew install sdl2
make
```

### Windows

Two supported toolchains.

**MSYS2 MinGW-w64** (recommended):

```sh
pacman -S base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 \
         mingw-w64-x86_64-pkgconf
make
```

**w64devkit** (no MSYS2 install needed):

```sh
make fetch-sdl2     # downloads SDL2 MinGW devel into external/SDL2/
make
```

For ethernet on Windows, see [Networking on Windows](#networking-on-windows)
below. The short version:

```sh
# install Npcap runtime once: https://npcap.com/  (enable WinPcap API compat)
make fetch-npcap-sdk     # downloads Npcap SDK into external/npcap-sdk/
make                     # auto-detects the SDK and builds with NET_BACKEND=pcap
```

## Networking

The 3C400 ethernet card is wired to a backend abstraction
(`sim/net.h`) with three implementations:

| Backend | Default on        | Host requirement       |
|---|---|---|
| `bpf`   | macOS / *BSD      | `/dev/bpf*` access     |
| `pcap`  | Linux / WSL / Windows | libpcap / Npcap    |
| `stub`  | (manual override) | none — no networking   |

Override per-build:

```sh
make NET_BACKEND=bpf       # force BPF
make NET_BACKEND=pcap      # force libpcap / Npcap
make NET_BACKEND=stub      # disable networking entirely
```

### Picking a host interface

The emulated 3C400 has to be bound to one of your physical or virtual
host interfaces. Four ways to choose:

```sh
make net-list                          # list available interfaces
./sim/sim --net-list                   # same, ran directly
./sim/sim --net-iface=eth0  ...        # CLI flag, literal name
./sim/sim --net-iface=7     ...        # CLI flag, index from --net-list
SUN2_NET_IFACE=7 make run              # env var (name or index both work)
```

`--net-iface` accepts either:

- a literal interface name (Linux: `eth0`, `wlan0`; macOS: `en0`;
  Windows: `\Device\NPF_{4B014404-...}` from `--net-list`)
- a positive integer N — uses the Nth entry from the `--net-list`
  output, 1-based. Much friendlier than copy-pasting GUIDs.

If neither flag nor env var is given, the active backend auto-picks:
`pcap` takes the first non-loopback adapter; `bpf` falls back to
`en0`.

`--net-list` shows the IPv4/IPv6 addresses bound to each interface,
so you can match a Windows GUID to a real network:

```
  7. \Device\NPF_{286B7E72-117D-4187-9C6D-C05A5240173B}
     Microsoft Corporation
     IPv6: fe80::84fc:ecef:f1e5:578d
     IPv4: 192.168.1.180/24  (network 192.168.1.0/24)
```

Then either: `--net-iface=7` or `--net-iface=\Device\NPF_{...}`.

### Networking on Windows

Two pieces are needed:

1. **Npcap runtime** — the user-visible driver and `wpcap.dll`. Install
   from [https://npcap.com/#download](https://npcap.com/#download).
   During install, **check "Install Npcap in WinPcap API-compatible
   mode"** so the standard `wpcap.dll` exports are available. This
   is what makes ethernet actually work at runtime.

2. **Npcap SDK** — `pcap.h` and `wpcap.lib`, only needed at
   *compile* time. The simplest path:

   ```sh
   make fetch-npcap-sdk
   ```

   That downloads the official SDK zip from npcap.com into
   `external/npcap-sdk/` and the Makefile picks it up automatically.
   Alternatively, install the Npcap SDK manually anywhere and point
   `NPCAP_SDK=/path/to/sdk` before `make`.

If the runtime is missing at run time, `sim.exe` will fail to start
with "wpcap.dll not found". If the SDK is missing at build time, the
Makefile defaults to `NET_BACKEND=stub` so the build still succeeds —
just without working ethernet.

The pre-built Windows release on GitHub ships **two** zip artifacts:

- `emulator-sun-2-windows-x64-pcap.zip` — full ethernet via Npcap.
  Requires the Npcap runtime to be installed on the user's machine.
- `emulator-sun-2-windows-x64-stub.zip` — no ethernet, runs anywhere.

#### Local-machine connections to the emulator (Windows pcap caveat)

When you bind pcap to your physical NIC, the emulator can talk to
**any other machine on your LAN** in both directions — ping, telnet,
FTP, SunOS network installs, etc. The Sun's frames are injected onto
the wire by `pcap_sendpacket()` and travel normally through the
switch / router back to the destination.

The one case that **doesn't work out of the box**: opening a
connection from the same Windows machine that's running the
emulator. For example `ping 192.168.1.167` or `telnet 192.168.1.167`
typed at the Windows host where `sim.exe` is running.

The reason: when `pcap_sendpacket()` injects the Sun's reply onto the
physical NIC, the frame goes out the wire, but the **local Windows
TCP/IP stack doesn't loop frames back from a local pcap process**.
So Windows never sees Sun's ARP reply or ICMP echo-reply, can't
update its ARP cache, and the connection times out. (Conversely, a
*different* machine on the LAN sees Sun's reply via the normal
"from the wire" receive path and works fine.)

Three ways around it, easiest first:

1. **Just use another machine on the LAN.** Telnet / ping from any
   other host on the same network to the emulator; this just works.

2. **Re-install Npcap with "Support loopback traffic" enabled.**
   Run the Npcap installer (or its uninstall+reinstall) and tick
   the *"Support loopback traffic"* checkbox. With that, frames a
   local pcap process injects do reach the local Windows TCP/IP
   stack, and `ping` / `telnet` from the same Windows host work.
   You also need *"WinPcap API-compatible mode"* checked, as before.

3. **Bind to a dedicated virtual NIC.** Install VirtualBox just for
   its host-only adapter, or set up a Hyper-V virtual switch of
   *Internal* type, give that adapter an IP in some private subnet,
   then `--net-iface=` to it. The Sun and the Windows host share an
   isolated subnet; ARP works normally; no loopback weirdness. This
   is also the cleanest setup if you don't want SunOS sniffing your
   real LAN.

The original 3C400 author's notes (from the comment block in the
source) recommend option 3 as well — they used VMware's `vmnet8` /
`vmnet1` for development. Linux users typically don't run into this
because Linux raw sockets do loop back to the local stack.

#### What pcap delivers, and why the chip needs to fake the FCS

Documented behaviour of libpcap and Npcap (per
[tcpdump.org](https://www.tcpdump.org/manpages/pcap.3pcap.html) and
[Wireshark wiki](https://wiki.wireshark.org/Ethernet)):

- The 4-byte Ethernet **FCS / CRC32 is stripped** by the NIC driver
  before pcap sees it. Pretty much always — exposing the FCS
  requires `ethtool -K eth0 rx-fcs on` on Linux with a supporting
  driver, or specific NDIS settings on Windows. Don't rely on it.
- pcap **does not deliver runt or bad-FCS frames** by default; the
  NIC drops them upstream, so we never see them.
- Frames arrive starting at the destination MAC byte; **no preamble
  / SFD** is included.

Real 3C400 hardware **does** put the FCS in the receive ring buffer,
and SunOS's `if_ec` driver validates it. So `sim/3c400.c` recomputes
a CRC32 over the frame data on every receive and appends it where
SunOS expects to find it — without that step every received frame
shows up in the SunOS console as `ec0: garbled packet`. If you ever
port this code to a different driver/OS that doesn't want the FCS,
the relevant code is the `crc32()` append in `e3c400_update()` and
the `firstfree += 6` doff calculation (= 2-byte status word + 4-byte
FCS).

See [Picking a host interface](#picking-a-host-interface) above for
how to use `make net-list` and `--net-iface=N` to choose which
adapter the Sun talks through. Windows interface names are opaque
GUIDs (`\Device\NPF_{...}`) so picking by index is much easier.

### Networking on Linux / WSL

```sh
sudo apt install libpcap-dev
make
```

libpcap usually needs `CAP_NET_RAW` to open raw sockets. Either run
the emulator with `sudo`, or grant the capability once:

```sh
sudo setcap cap_net_raw,cap_net_admin=eip ./sim/sim
```

Use `make net-list` (or `sim/sim --net-list`) to see available
interfaces with IPs, then `--net-iface=N`.

Unlike Windows, **Linux raw sockets do loop back to the local TCP/IP
stack** — frames the emulator injects via libpcap reach the local
Linux kernel as if they came in off the wire, so `ping 192.168.1.167`
and `telnet 192.168.1.167` from the same Linux host that's running
`sim` work without any extra setup. The Windows-only loopback caveat
described above doesn't apply here.

WSL2 has limited raw-socket support compared to a real Linux box.
Some WSL2 distributions cannot open `eth0` for raw I/O at all; in
that case build with `NET_BACKEND=stub`.

## Mouse

The emulator implements the MouseSystems 5-byte serial mouse protocol on
SCC channel A (port at 0x780000) -- the same protocol used by the
original Sun-2 optical mouse.

Press **Right Alt** (PC) / **Right Option** (Mac) to capture the host
mouse; the window title shows capture state. Movement and the three
buttons (left/middle/right) are sent to SunOS as MouseSystems packets.
Press the same key again to release.

When the mouse is **not** captured, right-clicking the window pastes the
host clipboard into the keyboard channel (channel B): ASCII characters
are mapped to Sun-2 scancodes and drip-fed through the SCC FIFO with
throttling so it doesn't overflow.

## SCC consoles over TCP

The Sun-2's serial chips (Z8530 SCCs) can be exposed on a TCP port so
you can drive them from a terminal client. One TCP listener serves
all configured ttys; on connect you get a menu and pick which one to
attach to.

```sh
./sim/sim --scc-tcp        --prom=... --disk=... --tape=...   # listens on 9900
./sim/sim --scc-tcp=9912   --prom=... --disk=... --tape=...   # listens on 9912
```

Or via the Makefile shortcut:

```sh
make run-tcp                # build + run + scc-tcp on 9900
make run TCP=9912 7         # plain run, scc-tcp on 9912, --net-iface=7
make run-serial             # TCP=9900 + --no-kbd (headless, PROM uses ttya)
```

Connect with `telnet` or `nc`:

```sh
telnet localhost 9900
nc     localhost 9900
```

### Connection menu

On connect the server greets you with:

```
Sun-2 SCC console -- pick a tty:
  [a] ttya  (idle)
  [b] ttyb  (idle)
  [Enter] pick first idle
  [q]     disconnect
> 
```

- A single keypress (`a`, `b`, …) picks that tty.
- `Enter` picks the first idle tty.
- `q` closes the connection.
- Picking a busy tty prints `ttyX busy, try another` and re-shows the
  menu.  The busy line shows the connected client's address so you can
  see who has it.

Each tty allows one client at a time.  Multiple clients can be
connected simultaneously to **different** ttys.

The server speaks telnet IAC: on connect it sends `IAC WILL ECHO` +
`IAC WILL/DO SUPPRESS_GO_AHEAD` so a vanilla `telnet host port`
switches to remote-echo character-at-a-time mode.  Inbound IAC
sequences (option negotiation, subnegotiation) are parsed and
swallowed before they reach SunOS's tty discipline, so you don't
see garbage characters at login.  `nc` users see the IAC bytes as
opaque data; if that bothers you, `telnet` is cleaner.

### Adding more ttys -- `--scc-boards=N`

The on-board hardware has two Z8530 chips: zs0 (ttya / ttyb) and zs1
(keyboard / mouse).  Sun-2 GENERIC SunOS supports up to four extra
**Multibus expansion** SCC cards (zs2..zs5).  Enable them with:

```sh
./sim/sim --scc-tcp --scc-boards=N ...
```

| `--scc-boards=` | Adds                | Total ttys                       |
|----------------:|---------------------|----------------------------------|
| `0` (default)   | nothing             | ttya, ttyb                       |
| `1`             | zs2 @ MBMEM 0x80800 | + ttye, ttyf                     |
| `2`             | + zs3 @ 0x81000     | + ttyg, ttyh                     |
| `3`             | + zs4 @ 0x84800     | + ttyi, ttyj                     |
| `4`             | + zs5 @ 0x85000     | + ttyk, ttyl  (10 ttys total)    |

**There is no `ttyc` or `ttyd` on a Sun-2.**  SunOS minor-numbers
strictly by chip-unit, so the kbd/mouse chip (zs1) eats those names
even though kbd/mouse aren't text terminals.  The menu reflects this:

```
Sun-2 SCC console -- pick a tty:
  [a] ttya  (idle)
  [b] ttyb  (idle)
  [e] ttye  (idle)        <- with --scc-boards=2 ...
  [f] ttyf  (idle)
  [g] ttyg  (idle)        <- ... up to here
  [h] ttyh  (idle)
  [Enter] pick first idle
  [q]     disconnect
> 
```

When `--scc-boards>=N` is set, SunOS's autoconf probes the matching
MBMEM addresses and reports the chips at boot:

```
zs0 at obio 2000 pri 3 
zs2 at mbmem 80800 pri 3 
zs3 at mbmem 81000 pri 3 
```

Below the threshold those addresses bus-error so SunOS's `zsprobe`
correctly fails for the missing slots.

### Getting a login prompt on ttya/ttyb/...

Detecting the chips is only half the job — SunOS's stock `/etc/ttytab`
has every serial line marked **`off`**, so `init` never spawns `getty`
on them.  A `telnet` client that picks `ttya` from the menu will
attach successfully but see nothing until something inside SunOS
opens `/dev/ttya` and writes to it.

The **headless** case (`make run-serial` = `--scc-tcp + --no-kbd`)
works around this: with `--no-kbd` the PROM detects no keyboard,
declares ttya the system console, so `getty`-on-console runs there
automatically and login prompts fly out the TCP port without any
disk-image edits.

For everything else — booting with the SDL window AND wanting login
prompts on telnet, or wanting prompts on ttye/f/g/h — you have to
edit SunOS's `/etc/ttytab` once, inside the running system:

```sh
# from the SunOS shell (root):
#   keep `console` as-is; turn the ttys you want on:

cat > /etc/ttytab.new <<'EOF'
console "/etc/getty std.9600"  sun           on  secure
ttya    "/etc/getty std.9600"  unknown       on
ttyb    "/etc/getty std.9600"  unknown       on
ttye    "/etc/getty std.9600"  unknown       on
ttyf    "/etc/getty std.9600"  unknown       on
ttyg    "/etc/getty std.9600"  unknown       on
ttyh    "/etc/getty std.9600"  unknown       on
ttyi    "/etc/getty std.9600"  unknown       off
ttyj    "/etc/getty std.9600"  unknown       off
ttyk    "/etc/getty std.9600"  unknown       off
ttyl    "/etc/getty std.9600"  unknown       off
EOF
mv /etc/ttytab.new /etc/ttytab
kill -HUP 1                       # tell init to re-read ttytab
```

After `init` re-reads, every `on` tty has a `getty` and the next
client that picks it from the menu lands at a `noname login:`.

Field reference (SunOS 3.2 `ttytab(5)`):
- column 1: device name in `/dev`
- column 2: command + argv (`-` to disable explicitly)
- column 3: terminal type, looked up in `/etc/termcap`
- column 4: `on` runs the command, `off` doesn't; `secure` allows root
  login, no-`secure` doesn't.

The disk image ships with all serials `off` because real Sun-2 owners
typically didn't have anything plugged into them and an unloaded
`getty` waiting on a non-existent line is wasted process slots.

### Caveats (from the original 3C400 driver)

The driver puts the host interface in promiscuous mode and was
written as alpha-quality. Don't run the emulator as root: on macOS
add yourself to the group that owns `/dev/bpf*`, on Linux use
`setcap` as above, on Windows just install Npcap as a normal user
and the runtime handles privilege.

## Files and layout

```
m68k/        Musashi 68010 core + opcode generator
sim/         Sun-2 simulator (CPU glue, MMU, devices, SDL frontend)
  net.h            packet I/O abstraction
  net_bpf.c        BPF backend
  net_pcap.c       libpcap / Npcap backend
  net_stub.c       null backend
  sim.rc           Windows resource file (embeds sun.ico)
  sun.ico          taskbar / Alt-Tab / Explorer icon (Windows)
  icon_data.h      runtime SDL_SetWindowIcon source (all platforms)
media/
  rom/             Sun-2 boot PROM
  disk/            SunOS disk images (2.0 / 3.2 / 3.5)
  tape/            tape images per SunOS version
scripts/
  fetch-sdl2.sh    download SDL2 MinGW devel for w64devkit
  gen-icon.py      regenerate icon_data.h + sun.ico from a source PNG
.github/workflows/
  release.yml      Linux / macOS / Windows build + tagged release
```

## Releases

Push a `v*` tag to trigger CI builds for all three platforms and
publish a GitHub Release with the artifacts attached:

```sh
git tag v0.1.0
git push origin v0.1.0
```

The workflow can also be triggered manually from the Actions tab to
run a build without publishing a Release.

## Some changes by sigurbjornl (original alpha-quality ethernet)

Original notes preserved verbatim:

> You need to configure your ethernet interface near the top of
> 3c400.c (under sim).
> The driver sets promisicous mode, and can sniff all traffic on the
> interface you select to run it on, if you don't like this, don't
> use it!
> Don't (ever!) run the emulator as root, you can chmod the BPF
> device files to 660 (if they aren't already) and add your user to
> the group set on the device to get access.
> Your success with communicating with the host that the emulator is
> running on will vary.
> It's best if you can use a dummy bridge interface to bind to, I
> used the vmnet interfaces from VMWare with a high degree of
> success, both in NAT and Bridged mode.
> If you want to route you'll need to setup proxy-arp since the Sun
> will ARP for any entry regardless of gateway settings, I tested
> parpd (https://github.com/rsmarples/parpd), and it works fine.
