/*
 * sun emulator -- Sun-3/60 machine implementation.
 *
 * MC68020 + Sun custom MMU.  This file contains:
 *   - Sun-3 MMU (segmap[8 ctx][2048] + pagemap[4096], 8 KB pages,
 *     FC=3 control space register dispatch, boot bypass for FC=6).
 *   - IDPROM with valid checksum.
 *   - System Enable / Bus Error registers.
 *   - Bus dispatch (sun3_cpu_read/write) routing to RAM, ROM, OBIO,
 *     OBMEM via PTE page-type field.
 *   - OBIO device routing for zs0 / zs1 SCCs and minimal stubs for
 *     EEPROM, ICM7170 clock, memory error / interrupt registers.
 *   - sun3_ops vtable.
 *
 * Reference: RetroCore C# implementation at
 *   E:/Dev/Repos/Ronny/RetroCore/Emulated.HW/Sun/MMU/Sun3/Sun3MMU.cs
 *   E:/Dev/Repos/Ronny/RetroCore/Emulated.Machines/Sun/Sun3/MachineSun3*.cs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "m68k.h"
#include "sim.h"
#include "sim68k.h"
#include "scc.h"
#include "scc_tcp.h"
#include "machine.h"
#include "sun3.h"

/* ============================================================== */
/*  Sun-3 MMU constants                                            */
/* ============================================================== */

/* 28-bit virtual address space; everything above is don't-care. */
#define SUN3_VA_MASK            0x0FFFFFFFu

/* Page geometry. */
#define SUN3_PAGE_LOG2          13                          /* 8 KB pages */
#define SUN3_PAGE_SIZE          (1u << SUN3_PAGE_LOG2)
#define SUN3_PAGE_OFFSET_MASK   (SUN3_PAGE_SIZE - 1u)       /* 0x1FFF */

/* Map structure. */
#define SUN3_CONTEXTS           8
#define SUN3_SEGS_PER_CTX       2048
#define SUN3_PMEGS              256
#define SUN3_PAGES_PER_PMEG     16
#define SUN3_TOTAL_PTES         (SUN3_PMEGS * SUN3_PAGES_PER_PMEG)  /* 4096 */
#define SUN3_SEG_INDEX_MASK     0x7FFu                      /* 11 bits */
#define SUN3_PTE_INDEX_MASK     0xFu                        /* 4 bits */
#define SUN3_PGFRAME_MASK       0x0007FFFFu                 /* 19 bits */

/* PTE bits (per RetroCore HelperEnum.cs PAGE_MAP). */
#define SUN3_PTE_VALID          (1u << 31)
#define SUN3_PTE_WRITE          (1u << 30)
#define SUN3_PTE_SYSTEM         (1u << 29)
#define SUN3_PTE_NC             (1u << 28)
#define SUN3_PTE_PGTYPE_MASK    (3u << 26)
#define SUN3_PTE_PGTYPE_SHIFT   26
#define SUN3_PTE_REF            (1u << 25)
#define SUN3_PTE_MOD            (1u << 24)

/* Page-type field values (bus selection). */
#define SUN3_PGTYPE_OBMEM       0
#define SUN3_PGTYPE_OBIO        1
#define SUN3_PGTYPE_VME_D16     2
#define SUN3_PGTYPE_VME_D32     3

/* System Enable register bits. */
#define SUN3_ENABLE_DIAG        0x01
#define SUN3_ENABLE_FPA         0x02
#define SUN3_ENABLE_COPY        0x04
#define SUN3_ENABLE_VIDEO       0x08
#define SUN3_ENABLE_CACHE       0x10
#define SUN3_ENABLE_SDVMA       0x20
#define SUN3_ENABLE_FPP         0x40
#define SUN3_ENABLE_NOTBOOT     0x80

/* Interrupt register bits at OBIO 0x0A0000.  Per C# MachineSun3Memory.cs:413. */
#define SUN3_IREG_INTS_ENAB     0x01
#define SUN3_IREG_SOFT_INT_1    0x02
#define SUN3_IREG_SOFT_INT_2    0x04
#define SUN3_IREG_SOFT_INT_3    0x08
#define SUN3_IREG_VIDEO_ENAB    0x10
#define SUN3_IREG_CLOCK_ENAB_5  0x20
#define SUN3_IREG_CLOCK_ENAB_7  0x80

/* Bus error register bits.  Per C# MachineSun3Memory.cs:502-508 -- these
   are the actual Sun-3-specific bits (NOT the generic BusErrorFlags enum
   in HelperEnum.cs which uses a different layout).  The PROM POST RAM
   probe checks (BUSERR & 0xFC) == 0x20 -- i.e. it expects TIMEOUT=0x20. */
#define SUN3_BUSERR_WATCHDOG    0x01    /* bit 0: Watchdog or user reset */
#define SUN3_BUSERR_FPAENERR    0x04    /* bit 2: FPA enable error */
#define SUN3_BUSERR_FPABERR     0x08    /* bit 3: FPA bus error */
#define SUN3_BUSERR_VMEBUSERR   0x10    /* bit 4: VME bus error */
#define SUN3_BUSERR_TIMEOUT     0x20    /* bit 5: address nonexistent */
#define SUN3_BUSERR_PROTERR     0x40    /* bit 6: MMU protection error */
#define SUN3_BUSERR_INVALID     0x80    /* bit 7: MMU page invalid */

/* PROM placement.  PROM is mapped at PA 0x0FEF0000 (size 64 KB) by
   convention; in boot mode (NOTBOOT=0) FC=6 reads bypass the MMU and
   land here directly. */
#define SUN3_PROM_BASE          0x0FEF0000u
#define SUN3_PROM_SIZE          0x00010000u   /* 64 KB */

/* ============================================================== */
/*  Sun-3 state                                                    */
/* ============================================================== */

/* Segment map: flattened [context * 2048 + segindex] -> PMEG. */
static uint8_t  s_segmap[SUN3_CONTEXTS * SUN3_SEGS_PER_CTX];

/* Page map: [pmeg * 16 + pteIdx] -> PTE. */
static uint32_t s_pagemap[SUN3_TOTAL_PTES];

/* Context register (3 bits used). */
static uint8_t  s_context;

/* System Enable register. */
static uint8_t  s_enable;

/* Bus Error register. */
static uint8_t  s_buserr;

/* Diagnostic register (front-panel LED display). */
static uint8_t  s_diag;
/* Last VA passed to MMU translation -- used by the OBIO 0x100000 PROM
   mirror (per TME sun3-mmu.c and C# MachineSun3Memory.cs:2630-2641): the
   PROM alias uses the VIRTUAL address's page frame to form the PROM
   offset, NOT the physical address.  This is what lets the PROM read
   its own data tables (e.g. spec-pages at PROM offset 0xd350) through
   a PTE with PGFRAME=0x80 -- without this, all such reads land on
   PROM[0..0x1FFF] and the PROM gets wildly wrong data, eventually
   writing 0x20000000 to pagemap[0] mid-mmu_init_segments and crashing. */
static uint32_t s_last_va;

/* Interrupt register at OBIO 0x0A0000 (gates all IRQs). */
static uint8_t  s_intreg;

/* Memory-error register at OBIO 0x080000.  Per C# constants
   (MachineSun3Memory.cs:421-427):
     bit 7 = INT_ACTIVE (read-only)
     bit 6 = ENABLE_INT (R/W)
     bit 5 = PAR_TEST
     bit 4 = PAR_ENABLE
     bits 3:0 = ERR_MASK (read-only, lane indicator) */
#define SUN3_MEMERR_INT_ACTIVE  0x80
#define SUN3_MEMERR_ENABLE_INT  0x40
#define SUN3_MEMERR_PAR_TEST    0x20
#define SUN3_MEMERR_PAR_ENABLE  0x10
#define SUN3_MEMERR_ERR_MASK    0x0F
static uint8_t  s_memerr;
/* Parity-test flag: set on a RAM write while PAR_TEST is on, checked
   on the next RAM read with PAR_ENABLE on (per C# line 2258).  This
   is the load-bearing piece of POST stage 0xF0 ("Memory parity error
   test"). */
static int      s_parity_test_written;

/* Clock IRQ pending from ICM7170 (gated to IPL 5 or 7 by intreg). */
static int      s_clock_pending;

/* IDPROM: 32 bytes.  Byte 1 = machine type, byte 14 = XOR checksum
   over bytes 0..13 (and 15..30 are mostly serial / date / scratch). */
static uint8_t  s_idprom[32];

/* MMU last-translation state.  Read by sun3_cpu_read after a fault
   to populate the bus error register correctly. */
static int      s_last_fault;
static int      s_last_proterr;
static uint8_t  s_last_pgtype;

/* External RAM array (provided by sim68k.c).  Sun-3/60 supports up to
   24 MB but the existing g_ram[] is 16 MB.  For now the MVP runs with
   16 MB of RAM, advertised correctly via the IDPROM / PROM probe. */
extern unsigned char g_ram[];
extern unsigned char g_rom[];
extern unsigned int  g_fc;
extern int           eprom_size;

/* SCC chip globals (defined at file scope in scc.c).  scc.h doesn't
   currently expose these; declare here so sun3_obio_read/write can
   route to the correct chip pair. */
extern scc_chip_t g_scc_serial;
extern scc_chip_t g_scc_kbd;

/* Pending bus-error helper (provided by sim68k.c). */
extern void pending_buserr(void);

/* CPU-side interrupt controller (provided by sim68k.c). */
extern void int_controller_set(unsigned int value);
extern void int_controller_clear(unsigned int value);

/* SDL framebuffer hooks (provided by sun2.c). */
extern void sdl_set_fbmem(unsigned char *p);
extern void sdl_init(void);

/* ============================================================== */
/*  Intersil ICM7170 TOD clock (OBIO 0x060000)                     */
/* ============================================================== */
/*
 * Reference: RetroCore Emulated.HW/Sun/Clock/Intersil7170.cs
 *
 * 18 byte-addressable registers at offsets 0x00..0x11.  Sun-3 uses
 * addrShift = 0 (bus address = register index directly).
 *
 *   0x00 CSEC   centiseconds, AUTO-INCREMENTS ON READ -- the PROM
 *              POST oscillator test reads CSEC twice and checksums;
 *              if the value doesn't change, oscillator is "stuck"
 *              and POST fails.  This is the load-bearing register.
 *   0x01 HOUR   hours (0-23)
 *   0x02 MIN    minutes
 *   0x03 SEC    seconds
 *   0x04 MON, 0x05 DAY, 0x06 YEAR (off from 1968), 0x07 DOW
 *   0x08-0x0F   alarm compare registers
 *   0x10 INT    interrupt status (read clears, write sets mask)
 *   0x11 CMD    command register (write only; reads return 0xFF)
 *               bit 4 INTENA, bit 3 RUN, bit 2 FMT24, bits 1:0 freq
 */
static uint8_t s_icm_regs[18];
static uint8_t s_icm_cmd;
static uint8_t s_icm_intmask;

/* Forward decl -- intreg eval is below. */
static void sun3_intreg_eval(void);

static uint8_t icm7170_read(uint32_t off)
{
    if (off >= 18) return 0xFF;
    if (off == 0x10) {                   /* INT: read clears */
        uint8_t v = s_icm_regs[0x10];
        s_icm_regs[0x10] = 0;
        s_clock_pending = 0;             /* clock IRQ cleared by ack */
        sun3_intreg_eval();
        return v;
    }
    if (off == 0x11) return 0xFF;        /* CMD is write-only */
    if (off == 0x00) {                   /* CSEC auto-increments */
        s_icm_regs[0x00]++;
        if (s_icm_regs[0x00] >= 100) s_icm_regs[0x00] = 0;
    }
    return s_icm_regs[off];
}

static void icm7170_write(uint32_t off, uint8_t v)
{
    if (off >= 18) return;
    if (off == 0x10) { s_icm_intmask = v & 0x7F; return; }
    if (off == 0x11) {
        s_icm_cmd = v;
        /* INTENA may have changed -- re-eval pending IRQ. */
        sun3_intreg_eval();
        return;
    }
    s_icm_regs[off] = v;
}

/* Periodic ICM7170 tick.  Called from sun3_device_tick on every CPU
   instruction; after a configurable counter, fires the HSEC interrupt
   if CMD.INTENA + intmask.HSEC are both set.  Counter chosen so the
   POST clock test (LED 0xF5) sees an IRQ within its busy-wait window
   of ~16M iterations. */
#define SUN3_ICM_HSEC_PERIOD 4096u

static void icm7170_tick(void)
{
    static unsigned int counter;
    if (++counter < SUN3_ICM_HSEC_PERIOD) return;
    counter = 0;
    if ((s_icm_cmd & 0x10) == 0) return;           /* INTENA off */
    if ((s_icm_intmask & 0x02) == 0) return;       /* HSEC mask off */
    /* Set INT.HSEC + INT.PENDING and fire CLOCK signal toward intreg. */
    s_icm_regs[0x10] |= 0x02 | 0x80;
    s_clock_pending = 1;
    sun3_intreg_eval();
}

static void icm7170_reset(void)
{
    memset(s_icm_regs, 0, sizeof(s_icm_regs));
    /* Plausible time-of-day: 1986-06-17 12:00:00.  The PROM normally
       takes whatever the host gave us, so any sane value is fine. */
    s_icm_regs[0x00] = 0;        /* CSEC */
    s_icm_regs[0x01] = 12;       /* HOUR */
    s_icm_regs[0x02] = 0;        /* MIN  */
    s_icm_regs[0x03] = 0;        /* SEC  */
    s_icm_regs[0x04] = 6;        /* MON  */
    s_icm_regs[0x05] = 17;       /* DAY  */
    s_icm_regs[0x06] = 18;       /* YEAR (1986 - 1968) */
    s_icm_regs[0x07] = 2;        /* DOW (Tuesday) */
    s_icm_cmd     = 0x08 | 0x04 | 0x03; /* RUN + FMT24 + freq=4.194MHz */
    s_icm_intmask = 0;
}

/* ============================================================== */
/*  bwtwo framebuffer (1152x900, 1 bpp)                            */
/* ============================================================== */
/*
 * Sun-3/60 bwtwo lives on the P4 bus at PA 0xFF000000.  The actual
 * memory layout per RetroCore's SunBwTwo.cs (Sun-3/60 case in
 * MachineSun3Memory.cs:648-660):
 *
 *   PA 0xFF000000 + 0x000000   VRAM (128 KB; 1152*900/8 = 129 600 bytes)
 *   PA 0xFF000000 + 0x1C0000   monitor-sense byte: 0x80 = standard
 *                              1152x900, 0x00 = hires 1600x1280
 *   PA 0xFF300000              P4 register -- explicitly NOT decoded
 *                              on real Sun-3/60 hardware; SunOS
 *                              bwtwo.c probes here, expects bus error,
 *                              and falls back to the non-P4 path.
 *
 * We accept reads/writes anywhere in the 0xFF000000 + 0x200000 window
 * and route to vram or sense byte; everything else returns 0xFF /
 * silently absorbs writes (matches "unmapped" behaviour without
 * raising bus error, which is good enough until SunOS actually probes
 * 0xFF300000).
 */
#define SUN3_BWTWO_BASE        0xFF000000u
#define SUN3_BWTWO_LEN         0x00200000u
#define SUN3_BWTWO_FB_SIZE     (128 * 1024)        /* 0x20000 */
#define SUN3_BWTWO_RES_OFFSET  0x001C0000u          /* monitor sense */

static unsigned char sun3_fbmem[SUN3_BWTWO_FB_SIZE];
static int           sun3_fb_inited;

static void sun3_fb_lazy_init(void)
{
    if (sun3_fb_inited) return;
    sun3_fb_inited = 1;
    memset(sun3_fbmem, 0, sizeof(sun3_fbmem));
    sdl_set_fbmem(sun3_fbmem);
    sdl_init();
}

/* Returns 1 if the PA is inside the bwtwo window (caller dispatched
   the access and should not fall through to "OBMEM unmapped"). */
static int sun3_bwtwo_read(uint32_t pa, int size, uint32_t *out)
{
    if (pa < SUN3_BWTWO_BASE || pa >= SUN3_BWTWO_BASE + SUN3_BWTWO_LEN)
        return 0;
    sun3_fb_lazy_init();
    uint32_t off = pa - SUN3_BWTWO_BASE;
    if (off < SUN3_BWTWO_FB_SIZE) {
        if (size == 1) { *out = sun3_fbmem[off]; return 1; }
        if (size == 2) { *out = ((uint32_t)sun3_fbmem[off] << 8)
                              |  sun3_fbmem[off + 1]; return 1; }
        *out = ((uint32_t)sun3_fbmem[off]   << 24)
             | ((uint32_t)sun3_fbmem[off+1] << 16)
             | ((uint32_t)sun3_fbmem[off+2] << 8 )
             |  sun3_fbmem[off+3];
        return 1;
    }
    if (off >= SUN3_BWTWO_RES_OFFSET && off < SUN3_BWTWO_RES_OFFSET + 4) {
        /* monitor sense: 0x80 = 1152x900 standard.  Same byte for
           every read in this 4-byte window (PROM only checks the top
           bit). */
        *out = (size == 1) ? 0x80u : 0x80808080u;
        return 1;
    }
    *out = 0xFFFFFFFFu;
    return 1;
}

static int sun3_bwtwo_write(uint32_t pa, uint32_t value, int size)
{
    if (pa < SUN3_BWTWO_BASE || pa >= SUN3_BWTWO_BASE + SUN3_BWTWO_LEN)
        return 0;
    sun3_fb_lazy_init();
    uint32_t off = pa - SUN3_BWTWO_BASE;
    if (off < SUN3_BWTWO_FB_SIZE) {
        if (size == 1)      sun3_fbmem[off] = (uint8_t)value;
        else if (size == 2) { sun3_fbmem[off]   = (uint8_t)(value >> 8);
                              sun3_fbmem[off+1] = (uint8_t)value; }
        else                { sun3_fbmem[off]   = (uint8_t)(value >> 24);
                              sun3_fbmem[off+1] = (uint8_t)(value >> 16);
                              sun3_fbmem[off+2] = (uint8_t)(value >> 8);
                              sun3_fbmem[off+3] = (uint8_t)value; }
        return 1;
    }
    /* monitor sense and other regions: silently absorb. */
    return 1;
}

/* ============================================================== */
/*  IDPROM                                                         */
/* ============================================================== */

void sun3_idprom_set_machine(unsigned char machine_type)
{
    /* Layout per Sun architecture manual:
       byte 0  = format (1)
       byte 1  = machine type (0x17 for Sun-3/60)
       byte 2-7 = ethernet OUI + serial (08:00:20:xx:xx:xx)
       byte 8-11 = manufacture date
       byte 12-14 = serial
       byte 15  = checksum (XOR of bytes 0..14)
       byte 16-31 = scratch / reserved
       The PROM verifies byte 15 == XOR(byte 0..14).  Wrong checksum
       gives cryptic POST failures. */
    memset(s_idprom, 0, sizeof(s_idprom));
    s_idprom[0]  = 0x01;
    s_idprom[1]  = machine_type;
    s_idprom[2]  = 0x08;
    s_idprom[3]  = 0x00;
    s_idprom[4]  = 0x20;
    s_idprom[5]  = 0x12;
    s_idprom[6]  = 0x34;
    s_idprom[7]  = 0x56;
    s_idprom[8]  = 0x00;
    s_idprom[9]  = 0x00;
    s_idprom[10] = 0x00;
    s_idprom[11] = 0x00;
    s_idprom[12] = 0x00;
    s_idprom[13] = 0x12;
    s_idprom[14] = 0x34;
    /* Compute checksum into byte 15. */
    uint8_t cks = 0;
    for (int i = 0; i < 15; i++) cks ^= s_idprom[i];
    s_idprom[15] = cks;
}

/* ============================================================== */
/*  MMU translation                                                */
/* ============================================================== */

/* Translate a 28-bit VA using the active context.  On fault, returns
   the original VA and sets s_last_fault / s_last_proterr.  On success,
   sets s_last_pgtype and returns the physical address. */
static uint32_t sun3_mmu_translate(uint32_t va, unsigned int fc, int is_read)
{
    s_last_fault   = 0;
    s_last_proterr = 0;
    s_last_pgtype  = 0;

    va &= SUN3_VA_MASK;
    s_last_va = va;  /* needed by OBIO PROM mirror */

    /* Decode VA. */
    uint32_t segindex = (va >> (SUN3_PAGE_LOG2 + 4)) & SUN3_SEG_INDEX_MASK;
    uint8_t  pmeg     = s_segmap[s_context * SUN3_SEGS_PER_CTX + segindex];
    uint32_t pteidx   = (uint32_t)((pmeg << 4) | ((va >> SUN3_PAGE_LOG2) & SUN3_PTE_INDEX_MASK));
    uint32_t pte      = s_pagemap[pteidx];

    if (!(pte & SUN3_PTE_VALID)) {
        s_last_fault = 1;
        return va;
    }

    /* Sun-3 protection check (2-bit, simpler than Sun-2's 6-bit). */
    int pte_system = (pte & SUN3_PTE_SYSTEM) != 0;
    int pte_write  = (pte & SUN3_PTE_WRITE)  != 0;
    int prot = 0;

    switch (fc) {
    case 1:  /* user data */
    case 2:  /* user program */
        if (pte_system) prot = 1;
        else if (!is_read && !pte_write) prot = 1;
        break;
    case 5:  /* supervisor data */
    case 6:  /* supervisor program */
        if (!is_read && !pte_write) prot = 1;
        break;
    default:
        break;
    }

    if (prot) {
        s_last_fault   = 1;
        s_last_proterr = 1;
        return va;
    }

    /* Set REF on every access, MOD on writes. */
    uint32_t ambits = SUN3_PTE_REF | (is_read ? 0 : SUN3_PTE_MOD);
    if ((pte & ambits) != ambits) s_pagemap[pteidx] = pte | ambits;

    s_last_pgtype = (uint8_t)((pte & SUN3_PTE_PGTYPE_MASK) >> SUN3_PTE_PGTYPE_SHIFT);
    uint32_t pgframe = pte & SUN3_PGFRAME_MASK;
    return (pgframe << SUN3_PAGE_LOG2) | (va & SUN3_PAGE_OFFSET_MASK);
}

/* ============================================================== */
/*  FC=3 control space (MMU register access)                       */
/* ============================================================== */

/* Decode the upper 4 bits of the address to select the register
   group; bits 27:0 are the "virtual address being mapped" for
   PGMAP/SEGMAP accesses. */

static uint32_t sun3_ctl_read(uint32_t address, int size)
{
    uint32_t reg = (address >> 28) & 0xFu;
    uint32_t va  = address & SUN3_VA_MASK;

    switch (reg) {
    case 0x0: {            /* IDPROM (read-only, byte-addressable) */
        return s_idprom[address & 0x1Fu];
    }
    case 0x1: {            /* PGMAP -- 32-bit PTE, byte-addressed */
        uint32_t segindex = (va >> (SUN3_PAGE_LOG2 + 4)) & SUN3_SEG_INDEX_MASK;
        uint8_t  pmeg     = s_segmap[s_context * SUN3_SEGS_PER_CTX + segindex];
        uint32_t pteidx   = (uint32_t)((pmeg << 4) | ((va >> SUN3_PAGE_LOG2) & SUN3_PTE_INDEX_MASK));
        uint32_t pte      = s_pagemap[pteidx];
        if (size == 4) return pte;
        if (size == 2) {
            int hi = ((address & 2) == 0);
            return hi ? (pte >> 16) & 0xFFFFu : pte & 0xFFFFu;
        }
        int byteOff = address & 3;
        return (pte >> (24 - byteOff * 8)) & 0xFFu;
    }
    case 0x2: {            /* SEGMAP -- 8-bit PMEG */
        uint32_t segindex = (va >> (SUN3_PAGE_LOG2 + 4)) & SUN3_SEG_INDEX_MASK;
        return s_segmap[s_context * SUN3_SEGS_PER_CTX + segindex];
    }
    case 0x3:              /* CONTEXT */
        return s_context & 0x07u;
    case 0x4:              /* ENABLE */
        return s_enable;
    case 0x5:              /* UDVMA */
        return 0;
    case 0x6: {            /* BUSERR -- read clears */
        uint8_t v = s_buserr;
        s_buserr = 0;
        return v;
    }
    case 0x7:              /* DIAG */
        return s_diag;
    case 0x8: case 0x9: case 0xA:  /* VAC -- not present on 3/60 */
        return 0;
    case 0xF:              /* UART bypass -- legacy diagnostic */
        return 0;
    default:
        return 0;
    }
}

static void sun3_ctl_write(uint32_t address, uint32_t value, int size)
{
    uint32_t reg = (address >> 28) & 0xFu;
    uint32_t va  = address & SUN3_VA_MASK;

    switch (reg) {
    case 0x0:              /* IDPROM is read-only */
        break;
    case 0x1: {            /* PGMAP -- write PTE */
        uint32_t segindex = (va >> (SUN3_PAGE_LOG2 + 4)) & SUN3_SEG_INDEX_MASK;
        uint8_t  pmeg     = s_segmap[s_context * SUN3_SEGS_PER_CTX + segindex];
        uint32_t pteidx   = (uint32_t)((pmeg << 4) | ((va >> SUN3_PAGE_LOG2) & SUN3_PTE_INDEX_MASK));
        if (size == 4) {
            s_pagemap[pteidx] = value;
        } else if (size == 2) {
            int hi = ((address & 2) == 0);
            uint32_t mask = hi ? 0x0000FFFFu : 0xFFFF0000u;
            uint32_t shift = hi ? 16u : 0u;
            s_pagemap[pteidx] = (s_pagemap[pteidx] & mask) | ((value & 0xFFFFu) << shift);
        } else {
            int byteOff = address & 3;
            int shift = 24 - byteOff * 8;
            uint32_t mask = ~(0xFFu << shift);
            s_pagemap[pteidx] = (s_pagemap[pteidx] & mask) | ((value & 0xFFu) << shift);
        }
        break;
    }
    case 0x2: {            /* SEGMAP */
        uint32_t segindex = (va >> (SUN3_PAGE_LOG2 + 4)) & SUN3_SEG_INDEX_MASK;
        s_segmap[s_context * SUN3_SEGS_PER_CTX + segindex] = (uint8_t)(value & 0xFFu);
        break;
    }
    case 0x3:              /* CONTEXT */
        s_context = (uint8_t)(value & 0x07u);
        break;
    case 0x4:              /* ENABLE */
        s_enable = (uint8_t)(value & 0xFFu);
        break;
    case 0x5:              /* UDVMA */
        break;
    case 0x6:              /* BUSERR is read-only (clears on read) */
        break;
    case 0x7:              /* DIAG -- front panel LEDs */
        s_diag = (uint8_t)(value & 0xFFu);
        break;
    default:
        break;
    }
}

/* ============================================================== */
/*  Interrupt routing                                              */
/* ============================================================== */
/*
 * Sun-3 routes all IRQs through OBIO 0x0A0000 (the interrupt
 * register).  The PROM POST stage 0xF6 (Software interrupt level 1)
 * tests this by writing INTS_ENAB | SOFT_INT_1 to the register and
 * waiting for the level-1 IRQ to fire.
 *
 * Per RetroCore MachineSun3Memory.cs:412+, the soft-int bits gate
 * directly: SOFT_INT_1 + INTS_ENAB → IPL 1, SOFT_INT_2 → IPL 2,
 * SOFT_INT_3 → IPL 3.  Clock bits CLOCK_ENAB_5/7 gate the ICM7170's
 * timer interrupt to IPL 5/7.
 */
static uint8_t s_intreg_irq_state;  /* bitmask of currently-asserted IPLs */
/* s_clock_pending is declared earlier (just below s_diag) so the
   ICM7170 helpers can reference it. */

static void sun3_intreg_eval(void)
{
    int ints_on = (s_intreg & SUN3_IREG_INTS_ENAB) != 0;
    uint8_t want = 0;
    if (ints_on) {
        if (s_intreg & SUN3_IREG_SOFT_INT_1) want |= (1u << 1);
        if (s_intreg & SUN3_IREG_SOFT_INT_2) want |= (1u << 2);
        if (s_intreg & SUN3_IREG_SOFT_INT_3) want |= (1u << 3);
        /* Clock IRQ from ICM7170 -- gated by CLOCK_ENAB_5 (IPL 5) or
           CLOCK_ENAB_7 (IPL 7, NMI).  POST stage 0xF5 routes via 5. */
        if (s_clock_pending && (s_intreg & SUN3_IREG_CLOCK_ENAB_5))
            want |= (1u << 5);
        if (s_clock_pending && (s_intreg & SUN3_IREG_CLOCK_ENAB_7))
            want |= (1u << 7);
    }
    /* Edge transitions per IPL. */
    for (int lvl = 1; lvl <= 7; lvl++) {
        uint8_t bit = (1u << lvl);
        if ((want & bit) && !(s_intreg_irq_state & bit))
            int_controller_set(lvl);
        else if (!(want & bit) && (s_intreg_irq_state & bit))
            int_controller_clear(lvl);
    }
    s_intreg_irq_state = want;
}

/* ============================================================== */
/*  OBIO dispatch (post-MMU PA in OBIO space)                      */
/* ============================================================== */

static uint32_t sun3_obio_read(uint32_t pa, int size)
{
    /* PA is the physical OBIO address.  Top byte of pa is don't-care
       once we're in OBIO; switch on the relevant offset. */
    uint32_t base = pa & 0x00FF0000u;
    uint32_t off  = pa & 0x0000FFFFu;
    (void)size;

    switch (base) {
    case 0x000000:   /* zs1 SCC (kbd/mouse) */
        return scc_chip_read(&g_scc_kbd, off & 0x0F);
    case 0x020000:   /* zs0 SCC (console) */
        return scc_chip_read(&g_scc_serial, off & 0x0F);
    case 0x040000:   /* EEPROM/NVRAM stub -- return 0xFF so PROM sees
                        unconfigured state and falls back to defaults */
        return 0xFF;
    case 0x060000:   /* Intersil ICM7170 TOD clock */
        return icm7170_read(off);
    case 0x080000:   /* Memory error register */
        return s_memerr;
    case 0x0A0000:   /* Interrupt register */
        return s_intreg;
    case 0x100000: {
        /* Boot PROM mirror at OBIO 0x100000.  Per TME sun3-mmu.c and
           C# MachineSun3Memory.cs:2630-2641: the PROM alias uses the
           VIRTUAL address's page frame to form the PROM offset, NOT
           the physical address.  This lets PTEs like PGFRAME=0x80 map
           ANY 8KB VA window in the 0xfef0000 range to its corresponding
           PROM data, which is how the PROM reads its own tables. */
        uint32_t prom_off = (s_last_va & ((SUN3_PROM_SIZE - 1u) & ~0x1FFFu))
                          | (s_last_va & 0x1FFFu);
        prom_off &= (SUN3_PROM_SIZE - 1u);
        if (prom_off + (uint32_t)size <= SUN3_PROM_SIZE) {
            if (size == 1) return g_rom[prom_off];
            if (size == 2) return ((uint32_t)g_rom[prom_off] << 8) | g_rom[prom_off + 1];
            return ((uint32_t)g_rom[prom_off]   << 24) |
                   ((uint32_t)g_rom[prom_off+1] << 16) |
                   ((uint32_t)g_rom[prom_off+2] << 8 ) |
                              g_rom[prom_off+3];
        }
        return 0xFFFFFFFFu;
    }
    case 0x120000:   /* LANCE Ethernet -- not implemented */
        return 0xFF;
    case 0x140000:   /* SI SCSI board -- not implemented */
        return 0xFF;
    default:
        return 0xFF;
    }
}

static void sun3_obio_write(uint32_t pa, uint32_t value, int size)
{
    uint32_t base = pa & 0x00FF0000u;
    uint32_t off  = pa & 0x0000FFFFu;
    (void)size;

    switch (base) {
    case 0x000000:
        scc_chip_write(&g_scc_kbd, off & 0x0F, (uint8_t)value);
        break;
    case 0x020000:
        scc_chip_write(&g_scc_serial, off & 0x0F, (uint8_t)value);
        break;
    case 0x040000:   /* EEPROM stub -- ignore writes */
        break;
    case 0x060000:   /* ICM7170 TOD clock */
        icm7170_write(off, (uint8_t)value);
        break;
    case 0x0A0000:   /* Interrupt register */
        s_intreg = (uint8_t)value;
        sun3_intreg_eval();
        break;
    case 0x080000: {
        /* MEMERR CSR at offset 0; latched VA register at offset 4.
           A write to offset 4 (any value) acks the parity NMI -- per
           PROM success path at 0x0FEFB56C which writes 0 to (FFF4004)
           after handling the parity exception.  Writes to CSR (offset
           0) update the value but DO NOT clear the latched parity
           condition in RAM (which the C# tracks via _parityTestWritten
           independently of the CSR). */
        if (off == 0x04) {
            /* Ack the latched parity error. */
            s_memerr &= ~SUN3_MEMERR_INT_ACTIVE;
            s_memerr &= ~SUN3_MEMERR_ERR_MASK;
            s_parity_test_written = 0;
            int_controller_clear(7);
            break;
        }
        s_memerr = (uint8_t)value;
        if (!(s_memerr & SUN3_MEMERR_INT_ACTIVE))
            int_controller_clear(7);
        break;
    }
    case 0x100000:   /* PROM mirror is read-only */
    case 0x120000:   /* LANCE not implemented */
    case 0x140000:   /* SI not implemented */
    default:
        break;
    }
}

/* ============================================================== */
/*  Bus access (top-level cpu_read / cpu_write for Sun-3)          */
/* ============================================================== */

static unsigned int sun3_cpu_read(int size, unsigned int address)
{
    /* FC=3: control space; bypass MMU. */
    if (g_fc == 3) {
        /* Composed long reads on 32-bit registers come as size 4.
           PGMAP needs that.  IDPROM is byte-addressed -- size 1
           reads are correct; size 2/4 will read the high byte and
           zero-extend, which is consistent with byte-organized PROM. */
        if (size == 4) {
            uint32_t v = sun3_ctl_read(address, 4);
            return v;
        }
        if (size == 2) {
            uint32_t v = sun3_ctl_read(address, 2);
            return v & 0xFFFFu;
        }
        return sun3_ctl_read(address, 1) & 0xFFu;
    }

    /* FC=6 boot bypass: supervisor program fetches go straight to ROM. */
    if (g_fc == 6 && (s_enable & SUN3_ENABLE_NOTBOOT) == 0) {
        uint32_t off = address & (SUN3_PROM_SIZE - 1u);
        if (size == 1) return g_rom[off];
        if (size == 2) return ((uint32_t)g_rom[off] << 8) | g_rom[off + 1];
        return ((uint32_t)g_rom[off]   << 24) |
               ((uint32_t)g_rom[off+1] << 16) |
               ((uint32_t)g_rom[off+2] << 8 ) |
                g_rom[off+3];
    }

    /* MMU-translated access. */
    uint32_t pa = sun3_mmu_translate(address, g_fc, 1);
    if (s_last_fault) {
        s_buserr = s_last_proterr ? SUN3_BUSERR_PROTERR : SUN3_BUSERR_INVALID;
        /* Raise bus error UNCONDITIONALLY -- the PROM POST relies on
           bus errors firing during its RAM-probe and bus-error-validation
           tests (LED stages 0xF7, 0xF4, 0xF1).  Suppressing in boot mode
           was wrong; the C# reference (MachineSun3Memory.cs ReadMemory
           line 2246) raises bus error regardless of NOTBOOT. */
        pending_buserr();
        return 0;
    }

    switch (s_last_pgtype) {
    case SUN3_PGTYPE_OBMEM: {
        /* RAM is at the bottom of OBMEM. */
        if (pa < MAX_RAM) {
            /* Parity-error check (POST stage 0xF0).  If a RAM write
               while PAR_TEST was set primed s_parity_test_written, and
               PAR_ENABLE is now on, the next RAM read latches a lane
               error in MEMERR and fires IPL 7.  See C# line 2258. */
            if (s_parity_test_written
                && (s_memerr & SUN3_MEMERR_PAR_ENABLE)
                && !(s_memerr & SUN3_MEMERR_INT_ACTIVE)) {
                uint8_t lane = (uint8_t)(0x08u >> (pa & 3u));
                s_memerr |= (uint8_t)(SUN3_MEMERR_INT_ACTIVE | lane);
                if (s_memerr & SUN3_MEMERR_ENABLE_INT)
                    int_controller_set(7);
            }
            if (size == 1) return g_ram[pa];
            if (size == 2) return ((uint32_t)g_ram[pa] << 8) | g_ram[pa + 1];
            return ((uint32_t)g_ram[pa]   << 24) |
                   ((uint32_t)g_ram[pa+1] << 16) |
                   ((uint32_t)g_ram[pa+2] << 8 ) |
                    g_ram[pa+3];
        }
        /* PROM in OBMEM space at PA 0x0FEF0000. */
        if (pa >= SUN3_PROM_BASE && pa < SUN3_PROM_BASE + SUN3_PROM_SIZE) {
            uint32_t off = pa - SUN3_PROM_BASE;
            if (size == 1) return g_rom[off];
            if (size == 2) return ((uint32_t)g_rom[off] << 8) | g_rom[off + 1];
            return ((uint32_t)g_rom[off]   << 24) |
                   ((uint32_t)g_rom[off+1] << 16) |
                   ((uint32_t)g_rom[off+2] << 8 ) |
                    g_rom[off+3];
        }
        /* bwtwo at PA 0xFF000000. */
        {
            uint32_t v;
            if (sun3_bwtwo_read(pa, size, &v)) return v;
        }
        /* Anything else in OBMEM space is unmapped -- bus-error.
           The PROM POST stage 0xF7 ("RAM probe (bus error)") at PROM
           offset 0xB104 maps VA 0x2000 to a PA > installed RAM and
           READS it, expecting a bus error to fire.  See
           ram_probe_and_fill_routine in Ghidra at 0x0FEFB0E0+. */
        s_buserr = SUN3_BUSERR_TIMEOUT;
        pending_buserr();
        return 0;
    }
    case SUN3_PGTYPE_OBIO:
        return sun3_obio_read(pa, size);
    case SUN3_PGTYPE_VME_D16:
    case SUN3_PGTYPE_VME_D32:
    default:
        /* Unmapped page-type or unimplemented VME -- bus error. */
        s_buserr = SUN3_BUSERR_TIMEOUT;
        pending_buserr();
        return 0;
    }
}

static void sun3_cpu_write(int size, unsigned int address, unsigned int value)
{
    if (g_fc == 3) {
        sun3_ctl_write(address, value, size);
        return;
    }

    uint32_t pa = sun3_mmu_translate(address, g_fc, 0);
    if (s_last_fault) {
        s_buserr = s_last_proterr ? SUN3_BUSERR_PROTERR : SUN3_BUSERR_INVALID;
        pending_buserr();
        return;
    }

    switch (s_last_pgtype) {
    case SUN3_PGTYPE_OBMEM:
        if (pa < MAX_RAM) {
            /* If PAR_TEST is armed, this write plants a "bad parity"
               marker -- the next RAM read with PAR_ENABLE on will
               fire the parity-error NMI (POST stage 0xF0). */
            if (s_memerr & SUN3_MEMERR_PAR_TEST)
                s_parity_test_written = 1;
            if (size == 1)      g_ram[pa] = (uint8_t)value;
            else if (size == 2) { g_ram[pa] = (uint8_t)(value >> 8); g_ram[pa+1] = (uint8_t)value; }
            else                { g_ram[pa]   = (uint8_t)(value >> 24);
                                  g_ram[pa+1] = (uint8_t)(value >> 16);
                                  g_ram[pa+2] = (uint8_t)(value >> 8);
                                  g_ram[pa+3] = (uint8_t)value; }
            break;
        }
        /* PROM region: silently absorb (read-only). */
        if (pa >= SUN3_PROM_BASE && pa < SUN3_PROM_BASE + SUN3_PROM_SIZE)
            break;
        /* bwtwo writes (PA 0xFF000000+). */
        if (sun3_bwtwo_write(pa, value, size)) break;
        /* Anything else: bus error, same as the read path. */
        s_buserr = SUN3_BUSERR_TIMEOUT;
        pending_buserr();
        break;
    case SUN3_PGTYPE_OBIO:
        sun3_obio_write(pa, value, size);
        break;
    default:
        s_buserr = SUN3_BUSERR_TIMEOUT;
        pending_buserr();
        break;
    }
}

/* ============================================================== */
/*  Lifecycle / vtable                                             */
/* ============================================================== */

static void sun3_machine_init(void)
{
    /* MMU empty / boot state. */
    memset(s_segmap,  0xFF, sizeof(s_segmap));
    memset(s_pagemap, 0,    sizeof(s_pagemap));
    s_context = 0;
    s_enable  = 0;       /* NOTBOOT=0 -> boot bypass active */
    s_buserr  = 0;
    s_diag    = 0;
    icm7170_reset();

    /* Initialise SCC chip pair (zs0 console + zs1 keyboard).  The
       existing scc.c shim_init() also wires the TX callbacks
       (zs0 -> scc_tcp_send_byte, zs1 -> kbd queue) which is what we
       want for Sun-3 as well. */
    extern void scc_init_traces(void);
    scc_init_traces();

    /* Sun-3 reuses Sun-2's SDL keyboard handler -- the Sun keyboard
       scancode set is identical, and the SDL→Sun mapping table
       (map_sdl_to_sun2kb) is filled by sun2_init().  Without this
       call the table stays zero-initialised and every SDL keypress
       maps to scancode 0 (= dropped). */
    sun2_init();

    /* Bring the SDL window up at startup so the user sees the
       machine launched, even before the PROM writes any pixels.
       Lazy-init handles the framebuffer pointer + SDL setup. */
    sun3_fb_lazy_init();

    /* IDPROM is set up in main() before sim68k() runs (machine_type
       is selected by --mode), so we don't touch s_idprom here. */
}

/* Per-instruction device tick.  Sun-3 has no am9513; the ICM7170 is
   not yet implemented for clock interrupts.  The PROM polls the
   tick counter at OBIO 0x600C2 directly via reads, so an explicit
   tick callback isn't needed for the banner-print phase. */
static void sun3_auto_abort_tick(void);
static void sun3_device_tick(void)
{
    scc_update();
    icm7170_tick();
    sun3_auto_abort_tick();
}

/* ============================================================== */
/*  Keyboard / auto-abort                                          */
/* ============================================================== */
/*
 * Per RetroCore MachineSun3Memory.cs:1775+:
 *
 *   - On RESET (cmd 0x01) the PROM expects three bytes back via the
 *     SCC RX FIFO: 0xFF (reset-ack), 0x04 (Type-4 keyboard layout id),
 *     0x7F (idle marker).
 *
 *   - The PROM ALSO expects to read the keyboard type byte from a
 *     specific BSS location at VA 0xFFFFE013.  On real hardware the
 *     NMI handler polls the SCC and stores the byte there, but during
 *     the keyboard-probe window the PROM hasn't enabled CLK7 in the
 *     interrupt register, so NMI never fires and the byte is never
 *     stashed.  Workaround: write 0x04 directly to physical RAM at
 *     the address VA 0xFFFFE013 maps to.
 *
 *   - Bell / LED / click commands (0x02..0x0B) are silent no-ops.
 *
 *   - Auto-abort works by starting a delay countdown on the keyboard
 *     reset response.  When the countdown expires, inject the L1-A
 *     scancode burst into the keyboard FIFO.  The PROM's keyboard
 *     state machine at 0x0FEF3B08 only detects L1-A after init has
 *     completed, hence the delay.
 */
extern void scc_in_push(int ch, int v);

#define SUN_KEY_L1   0x01
#define SUN_KEY_A    0x4D
#define SUN_KEY_IDLE 0x7F

static int s_auto_abort_enabled;
static int s_auto_abort_done;
static int s_auto_abort_countdown;   /* device_tick units; 0 = inactive */

void sun3_set_auto_abort(int enabled) { s_auto_abort_enabled = enabled; }

static void sun3_send_l1a(void)
{
    /* Press L1, press A, release A, release L1, idle. */
    scc_in_push(3, SUN_KEY_L1);
    scc_in_push(3, SUN_KEY_A);
    scc_in_push(3, SUN_KEY_A   | 0x80);
    scc_in_push(3, SUN_KEY_L1  | 0x80);
    scc_in_push(3, SUN_KEY_IDLE);
}

static void sun3_kb_write(int v)
{
    /* Non-reset commands (bell, LED, click) are silent on real hw. */
    if (v >= 0x02 && v <= 0x0B) return;

    if (v == 0x01) {
        /* RESET: reply 0xFF, 0x04, 0x7F via SCC RX. */
        scc_in_push(3, 0xFF);
        scc_in_push(3, 0x04);
        scc_in_push(3, SUN_KEY_IDLE);

        /* Write keyboard type 0x04 directly to RAM at VA 0xFFFFE013
           because the PROM's NMI handler doesn't fire during this
           probe window.  Translate via current MMU mapping (FC=5,
           supervisor data).  Save and restore fault state so we don't
           clobber an in-flight CPU fault. */
        {
            int saved_fault   = s_last_fault;
            int saved_proterr = s_last_proterr;
            uint8_t saved_pgtype = s_last_pgtype;
            uint32_t saved_va = s_last_va;

            uint32_t pa = sun3_mmu_translate(0x0FFFE013u, 5, 0);
            if (!s_last_fault && pa < MAX_RAM)
                g_ram[pa] = 0x04;

            s_last_fault   = saved_fault;
            s_last_proterr = saved_proterr;
            s_last_pgtype  = saved_pgtype;
            s_last_va      = saved_va;
        }

        /* Auto-abort: arm a delay so the L1-A burst lands AFTER the
           PROM finishes keyboard init and is ready to detect Stop-A.
           The device_tick fires once per CPU instruction; ~2 M ticks
           matches the C# countdown. */
        if (s_auto_abort_enabled && !s_auto_abort_done)
            s_auto_abort_countdown = 2 * 1000 * 1000;
    }
}

static void sun3_auto_abort_tick(void)
{
    if (s_auto_abort_countdown <= 0) return;
    if (--s_auto_abort_countdown == 0 && !s_auto_abort_done) {
        sun3_send_l1a();
        s_auto_abort_done = 1;
    }
}

/* IRQ acknowledge.  Sun-3/60 routes all IRQs through the interrupt
   register at OBIO 0x0A0000.  Soft int / clock IRQs use auto-vectors;
   the SCC at IPL 6 uses its own vector via scc_device_ack. */
static int sun3_irq_ack(int level)
{
    if (level == 6) return scc_device_ack(1);   /* zs SCC at IPL 6 on Sun-3 */
    /* Soft ints (IPL 1/2/3) and clock (IPL 5/7) use auto-vector. */
    return M68K_INT_ACK_AUTOVECTOR;
}

const machine_ops_t sun3_ops = {
    .name           = "sun3",
    .family         = MACH_SUN3,
    .m68k_cpu_type  = M68K_CPU_TYPE_68020,
    .init           = sun3_machine_init,
    .reset          = NULL,
    .cpu_read       = sun3_cpu_read,
    .cpu_write      = sun3_cpu_write,
    .device_tick    = sun3_device_tick,
    .irq_ack        = sun3_irq_ack,
    .kb_write       = sun3_kb_write,
};
