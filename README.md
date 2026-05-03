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

WSL2 has limited raw-socket support compared to a real Linux box.
Some WSL2 distributions cannot open `eth0` for raw I/O at all; in
that case build with `NET_BACKEND=stub`.

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
