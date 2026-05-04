# emulator-sun-2
Emulator for Sun-2 workstation

This is a emulator for the Sun Sun-2 Workstation.  It's designed to
boot SunOS and emulate the display and keyboard found on an original
Sun-2 worksation.

I was frustrated by the original Sun 2 emulator - tme.  It's an amazing
piece of software but really hard to follow internally.  It seems to
be an extreme example of abstraction.  Perhaps too abstract, at least
for me.

So I wrote this to learn about the MMU and, well SCSI.  My goal is to
eventually create an FPGA version of the Sun-2.

It now boots SunOS 2.0, 3.2, and 3.5 cleanly.  You can install from the distribution tapes.
The SCSI emulation code is still shakey for tapes, however.

SunOS 2.0 will boot multiuser and works as you'd expect.

## Mouse Support

The emulator implements the MouseSystems 5-byte serial mouse protocol,
the same protocol used by the original Sun-2 optical mouse. Mouse data
is fed to the Z8530 SCC channel A (port at 0x780000), which is the
hardware mouse port on real Sun-2 hardware.

### How to use

1. Boot SunOS and start a graphical environment (SunView or X11 on bwtwo)
2. Press **Right Alt** (PC) or **Right Option** (Mac) to capture the mouse
3. The console prints `mouse: capture ON` to confirm
4. Press the same key again to release the mouse

When captured:
- Mouse movement and all three buttons (left, middle, right) are sent
  to SunOS via the MouseSystems protocol
- The SDL window grabs the cursor (relative mouse mode)

When not captured:
- Mouse events are ignored by the emulator
- Right-click pastes clipboard text into the keyboard channel

### Protocol details

The MouseSystems protocol sends 5-byte packets at 1200 baud:

| Byte | Content                                              |
|------|------------------------------------------------------|
| 0    | `0x80 \| buttons` (bit 2=L, bit 1=M, bit 0=R; 0=pressed) |
| 1    | X delta, first half (signed, -127 to +127)           |
| 2    | Y delta, first half (positive = up)                  |
| 3    | X delta, second half                                 |
| 4    | Y delta, second half                                 |

Movement deltas are split across bytes 1-2 and 3-4 to handle large
movements. Packets are rate-limited to avoid overflowing the 16-byte
SCC input FIFO. Button-only packets (zero deltas) are sent immediately
on press/release.

### Clipboard paste

When the mouse is **not** captured, right-clicking the emulator window
pastes the host clipboard as keyboard input. Text is converted to Sun-2
keyboard scancodes and drip-fed into the SCC keyboard channel (channel B)
with throttling to prevent FIFO overflow. This is useful for typing
commands into SunOS without a graphical environment.

## Some changes by sigurbjornl

* Converted to use SDL2
* Added an alpha quality 3C400 ethernet driver to the sun, with support for using bpf for packet input/output, it's very likely that it has lots of bugs, and it might crash

```
You need to configure your ethernet interface near the top of 3c400.c (under sim)
The driver sets promisicous mode, and can sniff all traffic on the interface you select to run it on, if you don't like this, don't use it!
Don't (ever!) run the emulator as root, you can chmod the BPF device files to 660 (if they aren't already) and add your user to the group set on the device to get access
Your success with communicating with the host that the emulator is running on will vary
It's best if you can use a dummy bridge interface to bind to, I used the vmnet interfaces from VMWare with a high degree of success, both in NAT and Bridged mode
If you want to route you'll need to setup proxy-arp since the Sun will ARP for any entry regardless of gateway settings, I tested parpd (https://github.com/rsmarples/parpd), and it works fine
```

