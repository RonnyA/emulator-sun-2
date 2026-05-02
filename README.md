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

For ethernet on Windows, also install the
[Npcap runtime](https://npcap.com/) (with "WinPcap API compatible"
mode enabled) and the Npcap SDK, then build with:

```sh
NPCAP_SDK=/path/to/npcap-sdk make NET_BACKEND=pcap
```

Without the SDK the build defaults to `NET_BACKEND=stub` (the 3C400
appears to the guest but no packets flow).

## Networking

The 3C400 ethernet card is wired to a backend abstraction
(`sim/net.h`) with three implementations:

| Backend | Default on | Host requirement |
|---|---|---|
| `bpf`   | macOS / *BSD       | `/dev/bpf*` access |
| `pcap`  | Linux / Windows    | libpcap / Npcap    |
| `stub`  | (manual override)  | none — no networking |

Override per-build:

```sh
make NET_BACKEND=bpf       # force BPF
make NET_BACKEND=pcap      # force libpcap / Npcap
make NET_BACKEND=stub      # disable networking entirely
```

Caveats from the original 3C400 driver still apply: it sets
promiscuous mode on the host interface, the MAC and host interface
name are configured at the top of `sim/3c400.c`, and the alpha-quality
warning stands. Don't run the emulator as root.

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
