
PROM   = media/rom/sun2-multi-rev-R.bin
DISK   = media/disk/disk.img

# Which SunOS to stage when "make run" is called and disk.img is missing.
# Override with: make run RUN_VERSION=20  (or 35).
RUN_VERSION ?= 32

# .exe suffix on Windows so `make run` finds the binary either way.
ifeq ($(OS),Windows_NT)
    EXE_EXT := .exe
else ifneq (,$(findstring MINGW,$(shell uname -s 2>/dev/null)))
    EXE_EXT := .exe
else ifneq (,$(findstring MSYS,$(shell uname -s 2>/dev/null)))
    EXE_EXT := .exe
else
    EXE_EXT :=
endif

SIM = sim/sim$(EXE_EXT)

# ---------------------------------------------------------------------
# Capture extra positional words after `run` / `run-trace`, so the user
# can write `make run 7` to pass --net-iface=7 to sim.  Standard GNU
# Make idiom: pull every goal after the first into RUN_ARGS, then add
# a no-op rule for each captured word so `make` doesn't error with
# "no rule to make target '7'".
#
# Use the FIRST captured word as the network interface; ignore the
# rest.  This keeps the syntax simple and lets the user still pass
# things like `make run 7 RUN_VERSION=20` (variable assignments are
# not in MAKECMDGOALS so they don't get captured).
# ---------------------------------------------------------------------
RUN_TARGETS := run run-trace
ifneq (,$(filter $(RUN_TARGETS),$(MAKECMDGOALS)))
    RUN_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
    ifneq (,$(RUN_ARGS))
        $(eval $(RUN_ARGS):;@:)
        NET_IFACE_FLAG := --net-iface=$(firstword $(RUN_ARGS))
    endif
endif

.PHONY: all release sunos20 sunos32 sunos35 run run-trace net-list clean help fetch-sdl2 fetch-npcap-sdk

all:
	$(MAKE) -C m68k all
	$(MAKE) -C sim  all

# CI calls "make release"; for the sun-2 build the regular target
# already compiles with -O2, so this is just an alias for `all`.
release: all

# ---------------------------------------------------------------------
# Disk staging.  Each target copies the matching SunOS image to the
# generic disk.img the simulator opens, and (where applicable) writes
# the right tape directory name to media/tape/tape so the bootloader
# can find install media.
#
# We use cp instead of `ln -s` so this works on Windows hosts where
# real symlinks need Developer Mode / admin rights.
# ---------------------------------------------------------------------
sunos20:
	cp media/disk/my-sun2-s2.0-disk.img $(DISK)
	rm -rf media/tape/tape
	cp -R media/tape/tape2.0 media/tape/tape

sunos32:
	cp media/disk/my-sun2-s3.2-disk.img $(DISK)
	rm -rf media/tape/tape
	cp -R media/tape/tape3.2 media/tape/tape

sunos35:
	cp media/disk/my-sun2-s3.5-disk.img $(DISK)
	rm -rf media/tape/tape

# ---------------------------------------------------------------------
# Run the emulator with a disk attached.  If disk.img isn't staged yet,
# auto-stage SunOS $(RUN_VERSION) first.  On Windows, also drop SDL2.dll
# next to the binary so it loads without putting external/ on PATH.
#
# Default `make run` passes `-q` so the bus-error / vector / register
# trace stays off.  Without -q the printf flood from the Multibus probe
# loop runs the emulator at well under 1% real-time and the PROM never
# reaches its banner.  Use `make run-trace` to get the verbose path
# back when actually debugging the simulator.
# ---------------------------------------------------------------------
define stage_and_run
@if [ ! -f "$(DISK)" ]; then \
    echo "No $(DISK); staging SunOS $(RUN_VERSION) ..."; \
    $(MAKE) sunos$(RUN_VERSION); \
fi
$(if $(filter .exe,$(EXE_EXT)),@if [ -f external/SDL2/x86_64-w64-mingw32/bin/SDL2.dll ] && [ ! -f sim/SDL2.dll ]; then cp external/SDL2/x86_64-w64-mingw32/bin/SDL2.dll sim/SDL2.dll; fi)
$(SIM) $(1) $(NET_IFACE_FLAG) --prom=$(PROM) --disk=$(DISK) --tape=media/tape/tape
endef

run: all
	$(call stage_and_run,-q)

run-trace: all
	$(call stage_and_run,)

# List host network interfaces visible to the active backend.  Useful
# for picking what to pass to --net-iface or SUN2_NET_IFACE.
net-list: all
ifeq ($(EXE_EXT),.exe)
	@if [ -f external/SDL2/x86_64-w64-mingw32/bin/SDL2.dll ] && [ ! -f sim/SDL2.dll ]; then \
	    cp external/SDL2/x86_64-w64-mingw32/bin/SDL2.dll sim/SDL2.dll; \
	fi
endif
	$(SIM) --net-list

clean:
	$(MAKE) -C m68k clean
	$(MAKE) -C sim  clean

# Vendor SDL2 MinGW devel under external/SDL2/  (Windows / w64devkit only).
# Lets you build without MSYS2.  Override version with SDL2_VERSION=...
fetch-sdl2:
	@sh scripts/fetch-sdl2.sh

# Vendor the Npcap SDK under external/npcap-sdk/  (Windows only).
# Required to compile against libpcap on Windows — the Npcap *runtime*
# alone (wpcap.dll in System32\Npcap) doesn't ship pcap.h or the import
# library.  After running this, sim/Makefile auto-detects the SDK and
# defaults NET_BACKEND to pcap.
fetch-npcap-sdk:
	@sh scripts/fetch-npcap-sdk.sh

help:
	@echo "sun-2 emulator build"
	@echo "  make             Build for the host (default)"
	@echo "  make release     Same as 'make' (kept for CI symmetry)"
	@echo "  make clean       Remove build artifacts"
	@echo "  make run         Build, stage default disk, and run (quiet)"
	@echo "  make run N       Same, with --net-iface=N (index from --net-list,"
	@echo "                   or a literal interface name like eth0)"
	@echo "  make run-trace   Same as run but with full bus-error / vector trace"
	@echo "  make run-trace N Trace mode with --net-iface=N"
	@echo "  make net-list    Print available host network interfaces"
	@echo "                   (then pass one with 'make run N' or"
	@echo "                    --net-iface=NAME or SUN2_NET_IFACE=NAME)"
	@echo "  make run RUN_VERSION=20|32|35"
	@echo "                   Stage that SunOS image instead of the default 3.2"
	@echo "  make sunos20     Stage SunOS 2.0 disk + tape (no run)"
	@echo "  make sunos32     Stage SunOS 3.2 disk + tape (no run)"
	@echo "  make sunos35     Stage SunOS 3.5 disk (no run)"
	@echo "  make fetch-sdl2  Download SDL2 MinGW devel into external/SDL2/"
	@echo "                   (only needed for local Windows / w64devkit builds)"
	@echo "  make fetch-npcap-sdk"
	@echo "                   Download Npcap SDK into external/npcap-sdk/ for"
	@echo "                   building libpcap support on Windows"
	@echo
	@echo "Network backend (override per-build):"
	@echo "  make NET_BACKEND=bpf    BSD Packet Filter (default macOS/BSD)"
	@echo "  make NET_BACKEND=pcap   libpcap (default Linux) / Npcap (Windows)"
	@echo "  make NET_BACKEND=stub   No networking"
