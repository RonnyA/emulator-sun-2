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

## SCC console over TCP

The SCC channel-A serial console (SunOS `/dev/console`) can be
exposed on a TCP port so you can interact with it from a terminal
client instead of (or alongside) the SDL window:

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

### Headless boot via TCP (recommended for `make run-serial`)

The Sun-2's default console is the **bwtwo framebuffer + keyboard**.
PROM banner, "Self Test PASSED", `>` prompt and SunOS kernel messages
all go to the framebuffer (the SDL window) by default — `--scc-tcp`
on its own only sees data after SunOS specifically writes to
`/dev/ttya` from a shell.

Pass **`--no-kbd`** to make the emulated keyboard look unattached.
The PROM probes the keyboard on SCC channel 3 at boot, gets no reply,
gives up, and falls back to ttya (= SCC channel 0) for both input
and output. From that point on every byte the PROM and SunOS print
goes through the TCP port:

```sh
./sim/sim --scc-tcp --no-kbd ...
make run-serial               # convenience wrapper for the above
```

With this combo, `telnet localhost 9900` (or `nc localhost 9900`) is
your full Sun-2 console session — banner, boot prompt, login. The
SDL window still appears with the bwtwo display but you don't need
to interact with it.

The server is raw bytes both ways — no telnet IAC negotiation. When
using `telnet`, the client emits a few IAC option bytes at startup;
SunOS's tty discipline mostly ignores them. `nc` is cleaner.

Single client at a time; new connections wait until the previous
one disconnects. Bytes you type are pushed into SCC channel 0 input
(SunOS sees them as console keystrokes); bytes SunOS writes to
channel 0 (and channel 1) are forwarded out. Default port: **9900**.
Implemented in `sim/scc_tcp.c` — pthread-based server, ringbuf'd in
both directions so the emulator main loop never blocks on the
network.

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
