# sun-2 emulator
#
# 10/2014  Brad Parker <brad@heeltoe.com>
# Copyright (C) 2014-2019 Brad Parker <brad@heeltoe.com>
#
# Cross-platform: macOS / Linux / NetBSD / Windows (MSYS2 MinGW-w64).

# ---------------------------------------------------------------------
# Host detection
# ---------------------------------------------------------------------
UNAME_S := $(shell uname -s 2>/dev/null)

IS_WINDOWS := 0
ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    IS_WINDOWS := 1
endif
ifeq ($(findstring MSYS,$(UNAME_S)),MSYS)
    IS_WINDOWS := 1
endif
ifeq ($(OS),Windows_NT)
    IS_WINDOWS := 1
endif

# Add .exe to the binary name on Windows.
ifeq ($(IS_WINDOWS),1)
    EXE_EXT := .exe
else
    EXE_EXT :=
endif

# ---------------------------------------------------------------------
# Network backend selection
#
# Default per platform:
#   macOS / NetBSD / FreeBSD  -> bpf   (works out of the box)
#   Linux                     -> pcap  (libpcap, apt: libpcap-dev)
#   Windows (MSYS2 MinGW-w64) -> pcap  (Npcap SDK + Npcap runtime)
#
# Override on the command line:
#   make NET_BACKEND=stub      # disable networking entirely
#   make NET_BACKEND=pcap      # force libpcap on macOS, etc
# ---------------------------------------------------------------------
ifeq ($(UNAME_S),Darwin)
    NET_BACKEND ?= bpf
else ifeq ($(UNAME_S),NetBSD)
    NET_BACKEND ?= bpf
else ifeq ($(UNAME_S),FreeBSD)
    NET_BACKEND ?= bpf
else ifeq ($(UNAME_S),OpenBSD)
    NET_BACKEND ?= bpf
else ifeq ($(IS_WINDOWS),1)
    # Default Windows to libpcap only when an Npcap SDK is visible.
    # Otherwise fall back to the no-network stub so a fresh checkout
    # builds without extra setup.
    ifdef NPCAP_SDK
        NET_BACKEND ?= pcap
    else
        NET_BACKEND ?= stub
    endif
else
    NET_BACKEND ?= pcap
endif

ifeq ($(NET_BACKEND),bpf)
    NET_OBJ  := net_bpf.o
    NET_DEFS := -DNET_BACKEND_BPF
    NET_INC  :=
    NET_LIBS :=
else ifeq ($(NET_BACKEND),pcap)
    NET_OBJ  := net_pcap.o
    NET_DEFS := -DNET_BACKEND_PCAP
    ifeq ($(IS_WINDOWS),1)
        # Npcap SDK location — set via env var.  In CI we download the
        # Npcap SDK and point NPCAP_SDK at the extracted folder.
        ifndef NPCAP_SDK
            $(warning NPCAP_SDK is not set; assuming Npcap SDK headers are on the include path)
        endif
        NET_INC  := $(if $(NPCAP_SDK),-I"$(NPCAP_SDK)/Include")
        NET_LIBS := $(if $(NPCAP_SDK),-L"$(NPCAP_SDK)/Lib/x64") -lwpcap
    else
        NET_INC  :=
        NET_LIBS := -lpcap
    endif
else ifeq ($(NET_BACKEND),stub)
    NET_OBJ  := net_stub.o
    NET_DEFS := -DNET_BACKEND_STUB
    NET_INC  :=
    NET_LIBS :=
else
    $(error Unknown NET_BACKEND='$(NET_BACKEND)' (expected bpf, pcap, or stub))
endif

# ---------------------------------------------------------------------
# Object list
# ---------------------------------------------------------------------
OBJ = sim.o sim68k.o sun2.o scc.o am9513.o mm58167.o 3c400.o sc.o scsi.o utils.o \
	$(NET_OBJ) \
	m68kcpu.o m68kops.o m68kopac.o m68kopdm.o m68kopnz.o m68kdasm.o

# Windows: compile the resource file (sim.rc → sim_res.o) so the
# embedded sun.ico ends up in sim.exe's PE resource section.  The
# taskbar / Alt-Tab / file explorer pull from there.
ifeq ($(IS_WINDOWS),1)
    OBJ += sim_res.o
endif

M68K_SRC = ../m68k
DEFS = -DM68K_V33 $(NET_DEFS)

# ---------------------------------------------------------------------
# Per-OS toolchain bits (SDL, link libraries)
# ---------------------------------------------------------------------
# SDL2 detection.  Three sources in priority order:
#   1. Vendored devel tree at ../external/SDL2/<arch>-w64-mingw32/  (Windows
#      only — the upstream MinGW devel zip's sdl2-config and sdl2.pc both
#      ship with hardcoded /tmp/tardir/... paths, so we reference the
#      documented layout directly instead of trusting those tools.)
#   2. sdl2-config on PATH (works on macOS/Homebrew, Linux, MSYS2).
#   3. NetBSD: pkgsrc layout under /usr/pkg.
VENDORED_SDL2_DIR := ../external/SDL2/x86_64-w64-mingw32

ifeq ($(IS_WINDOWS),1)
    # NOTE: SDL2's MinGW config emits "-mwindows" which links us as a Win32
    # GUI app (subsystem:windows).  That kills the console — stdout/stderr
    # go nowhere, perror() is invisible, and the user can't tell whether
    # the disk image opened or failed.  We strip "-mwindows" and force
    # "-mconsole" so the trace prints actually reach the terminal.
    ifneq ($(wildcard $(VENDORED_SDL2_DIR)/include/SDL2),)
        EXTRA_INC := -I$(VENDORED_SDL2_DIR)/include -I$(VENDORED_SDL2_DIR)/include/SDL2 -Dmain=SDL_main
        SYS_LIBS  := -L$(VENDORED_SDL2_DIR)/lib -lmingw32 -lSDL2main -lSDL2 -mconsole -lws2_32
    else
        EXTRA_INC := $(shell sdl2-config --cflags)
        SDL_LIBS_RAW := $(shell sdl2-config --libs)
        # ws2_32 supplies htonl/htons on Windows.
        SYS_LIBS  := $(filter-out -mwindows,$(SDL_LIBS_RAW)) -mconsole -lws2_32
    endif
else ifeq ($(UNAME_S),Darwin)
    EXTRA_INC := $(shell sdl2-config --cflags)
    SYS_LIBS  := $(shell sdl2-config --libs)
else ifeq ($(UNAME_S),NetBSD)
    EXTRA_INC := -I/usr/pkg/include
    SYS_LIBS  := -R/usr/pkg/lib -L/usr/pkg/lib -lSDL2 -lpthread
else
    EXTRA_INC := $(shell sdl2-config --cflags)
    SYS_LIBS  := $(shell sdl2-config --libs) -lpthread
endif

# -fcommon restores pre-GCC-10 tentative-definition handling.  The
# original code declares e.g. `int trace_irq;` in both am9513.c and
# sim68k.c without `extern`, which the modern default (-fno-common)
# rejects as multiple definitions.
CFLAGS = -I. -I$(M68K_SRC) $(EXTRA_INC) $(NET_INC) -g -O2 -fcommon $(DEFS)
LIBS   = $(NET_LIBS) $(SYS_LIBS)

TARGET = sim$(EXE_EXT)

# ---------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------
all: $(TARGET)
	@echo ""
	@echo "Built $(TARGET) (host=$(UNAME_S), net=$(NET_BACKEND))"

$(TARGET): $(OBJ)
	$(CC) -o $(TARGET) $(OBJ) $(LIBS)

sim.o: sim.c
	$(CC) -c $(CFLAGS) $<

sim68k.o: sim68k.c
	$(CC) -c $(CFLAGS) $<

sun2.o: sun2.c
	$(CC) -c $(CFLAGS) $<

net_bpf.o: net_bpf.c net.h
	$(CC) -c $(CFLAGS) $<

net_pcap.o: net_pcap.c net.h
	$(CC) -c $(CFLAGS) $<

net_stub.o: net_stub.c net.h
	$(CC) -c $(CFLAGS) $<

m68kcpu.o: $(M68K_SRC)/m68kcpu.c $(M68K_SRC)/m68kops.h $(M68K_SRC)/m68kcpu.h
	$(CC) -c $(CFLAGS) -o $@ $<

m68kops.o: $(M68K_SRC)/m68kmake$(EXE_EXT) $(M68K_SRC)/m68kops.h $(M68K_SRC)/m68kops.c
	$(CC) -c $(CFLAGS) $(M68K_SRC)/m68kops.c -o m68kops.o

m68kopac.o: $(M68K_SRC)/m68kmake$(EXE_EXT) $(M68K_SRC)/m68kops.h $(M68K_SRC)/m68kopac.c
	$(CC) -c $(CFLAGS) $(M68K_SRC)/m68kopac.c -o m68kopac.o

m68kopdm.o: $(M68K_SRC)/m68kmake$(EXE_EXT) $(M68K_SRC)/m68kops.h $(M68K_SRC)/m68kopdm.c
	$(CC) -c $(CFLAGS) $(M68K_SRC)/m68kopdm.c -o m68kopdm.o

m68kopnz.o: $(M68K_SRC)/m68kmake$(EXE_EXT) $(M68K_SRC)/m68kops.h $(M68K_SRC)/m68kopnz.c
	$(CC) -c $(CFLAGS) $(M68K_SRC)/m68kopnz.c -o m68kopnz.o

m68kdasm.o: $(M68K_SRC)/m68kdasm.c
	$(CC) -c $(CFLAGS) -o $@ $<

# Windows resource compile: windres handles .rc files in MinGW.
sim_res.o: sim.rc sun.ico
	windres -i sim.rc -O coff -o sim_res.o

clean:
	rm -f *.o
	rm -f sim sim.exe
