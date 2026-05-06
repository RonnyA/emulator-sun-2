/*
 * sun3_si.c -- Sun-3/60 SI SCSI board (onboard variant).
 *
 * Sits at OBIO 0x140000, IPL 2.  Combines an NCR5380 chip with a small
 * onboard DMA controller (Sun's "si" board, not the VME variant).
 *
 * Register layout (per RetroCore Sun3SIBoard.cs and TME tme/bus/sun3-mainbus):
 *
 *   offset  size  name
 *   0x00..0x07   NCR5380 chip registers (8 byte-wide regs)
 *   0x08..0x0B  4  DMA address (big-endian u32)
 *   0x0C..0x0F  4  DMA byte count (big-endian u32)
 *   0x10..0x11  2  AM9516 UDC data    (chained DMA, optional)
 *   0x12..0x13  2  AM9516 UDC address (chained DMA, optional)
 *   0x14..0x15  2  FIFO data
 *   0x16..0x17  2  FIFO byte count
 *   0x18..0x19  2  CSR (control / status)
 *   0x1A..0x1F  6  VME-only registers (BPR, IV/AM) -- ignored on onboard
 *
 * SCSI bus + disk side is delegated to the existing scsi.c chip code
 * (the same code Sun-2's sc.c uses).  This file is just the NCR5380
 * register decode + DMA glue.
 *
 * MVP scope: get the PROM past its si_open() probe so autoboot either
 * succeeds or fails cleanly into the '>' monitor.  Full read-from-disk
 * comes after.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "sim68k.h"
#include "m68k.h"
#include "sim.h"
#include "scsi.h"
#include "sun3_si.h"

/* --- NCR5380 register layout -------------------------------------- */
#define N5380_CSD          0  /* r: Current SCSI Data        */
#define N5380_ODR          0  /* w: Output Data Register     */
#define N5380_ICR          1  /* r/w: Initiator Command Reg  */
#define N5380_MR           2  /* r/w: Mode Register          */
#define N5380_TCR          3  /* r/w: Target Command Reg     */
#define N5380_CSBS         4  /* r: Current SCSI Bus Status  */
#define N5380_SER          4  /* w: Select Enable Register   */
#define N5380_BSR          5  /* r: Bus and Status Register  */
#define N5380_SDS          5  /* w: Start DMA Send           */
#define N5380_IDR          6  /* r: Input Data Register      */
#define N5380_SDTR         6  /* w: Start DMA Target Recv    */
#define N5380_RPI          7  /* r: Reset Parity / Ints      */
#define N5380_SDIR         7  /* w: Start DMA Initiator Recv */

/* ICR bits */
#define ICR_RST            0x80
#define ICR_TEST_MODE      0x40
#define ICR_DIFF_ENABLE    0x20
#define ICR_ACK            0x10
#define ICR_BUSY           0x08
#define ICR_SEL            0x04
#define ICR_ATN            0x02
#define ICR_DATA_BUS       0x01

/* --- SI CSR bits -------------------------------------------------- */
#define SI_CSR_RESET_CTRL    0x0001
#define SI_CSR_RESET_FIFO    0x0002
#define SI_CSR_INT_ENABLE    0x0004
#define SI_CSR_DMA_SEND      0x0008
#define SI_CSR_INT_DMA       0x0100
#define SI_CSR_INT_NCR5380   0x0200
#define SI_CSR_FIFO_EMPTY    0x0400
#define SI_CSR_FIFO_FULL     0x0800
#define SI_CSR_DMA_BUS_ERROR 0x2000
#define SI_CSR_DMA_CONFLICT  0x4000
#define SI_CSR_ONBOARD_DMA   0x8000  /* DMA active */

/* --- module state ------------------------------------------------- */
static uint8_t  s_icr;
static uint8_t  s_mr;
static uint8_t  s_tcr;
static uint8_t  s_ser;
static uint16_t s_data;        /* SCSI bus data shadow */
static uint16_t s_csr;
static uint32_t s_dma_addr;
static uint32_t s_dma_count;
static uint16_t s_udc_data;
static uint16_t s_udc_addr;
static uint16_t s_fifo_count;

/* AM9516 UDC indirect register file.  PROM writes the register index
   to s_udc_addr (low byte = 0..0x3F), then writes the data to
   s_udc_data; the low-byte commit copies s_udc_data into s_udc_regs
   at the indexed slot.  Reg 0x2E = command; writing 0x00A0 (master
   enable / chain-start) is what kicks the actual DMA. */
static uint16_t s_udc_regs[64];

/* Cached SCSI bus state from scsi.c, refreshed on every NCR5380
   register access. */
static unsigned int s_bus_state;
static unsigned int s_bus_irq;

/* Buffer pending DMA (Sun-3 path): scsi.c calls sc_dma_read_data /
   sc_dma_write_data immediately after parsing the CDB, but the Sun-3
   PROM hasn't programmed the UDC chain block yet — the actual transfer
   is deferred until the PROM writes UDC reg 0x2E = 0x00A0.  Stash the
   pointer and length here for replay at that trigger. */
static unsigned char *s_pending_buf;
static int            s_pending_size;
static int            s_pending_is_read;   /* 1 = data IN (target->host), 0 = OUT */

extern unsigned int scsi_read_cmd_byte(void);

/* Drive scsi.c's bus state machine.  Translate ICR + selected target
   to SCSI bus output lines, call scsi_update(), latch input state. */
static void si_pump(void)
{
    unsigned int out = 0;
    if (s_icr & ICR_RST) out |= SCSI_BUS_RST;
    if (s_icr & ICR_SEL) out |= SCSI_BUS_SEL;
    if (s_icr & ICR_ACK) out |= SCSI_BUS_ACK;
    if (s_icr & ICR_ATN) out |= SCSI_BUS_ATN;
    /* BSY: NCR5380 asserts BSY when it owns the bus (after winning
       arbitration / during target-mode); for initiator-mode passthrough
       leave it driven by scsi.c. */

    /* During SELECTION, real SCSI puts {initiator_bit | target_bit} on
       the data lines (Sun-3 PROM uses 0x81 = init=7 + target=0).  The
       shared scsi.c code, however, only matches the bare target bit
       (0x01 / 0x10) — that pattern is what Sun-2's SC chip drives.
       Mask off the initiator bit only while SEL is asserted so scsi.c
       recognises the target id; everything else (data phase bytes etc)
       passes through unchanged. */
    uint16_t data16 = s_data;
    if (s_icr & ICR_SEL)
        data16 &= 0x7F;

    unsigned int in = 0, irq = 0;
    scsi_update(&data16, out, &in, &irq);
    s_data = data16;
    s_bus_state = in;
    s_bus_irq |= irq;
}

/* Faked REQ-drop-after-ACK: scsi.c keeps SCSI_BUS_REQ continuously
   asserted while in a transfer phase, but the PROM expects per-byte
   handshake (REQ↑ → host ACK → REQ↓ → host ACK↓ → REQ↑ for next byte).
   We simulate that by latching a "REQ is dropped" bit when ACK was
   just asserted, and clearing it when ACK is released.  s_csbs() ANDs
   this with the live bus_state's REQ. */
static int s_req_suppressed;

/* Handshake byte transfer: NCR5380 host puts data on ODR, then asserts
   ACK to clock the byte into the target.  scsi.c does not model the
   per-byte REQ/ACK handshake — it provides byte-level entry points
   (scsi_write_cmd_byte / scsi_read_cmd_byte) and tracks phase via the
   bus state.  Detect ACK transitions here, route the byte by the
   current bus phase, and toggle the REQ suppression so the PROM sees
   a clean handshake. */
static void si_handshake_ack(int ack_was_set)
{
    int ack_now = (s_icr & ICR_ACK) ? 1 : 0;
    if (!ack_was_set && ack_now) {
        /* Rising edge of ACK -- clock the byte. */
        unsigned int phase = s_bus_state & (SCSI_BUS_MSG | SCSI_BUS_CD | SCSI_BUS_IO);

        if (phase == SCSI_BUS_CD) {
            /* COMMAND OUT: host -> target */
            scsi_write_cmd_byte(s_data & 0xFF);
        } else if (phase == (SCSI_BUS_CD | SCSI_BUS_IO)) {
            /* STATUS IN: target -> host */
            s_data = (s_data & 0xFF00) | (scsi_read_cmd_byte() & 0xFF);
        } else if (phase == (SCSI_BUS_MSG | SCSI_BUS_CD | SCSI_BUS_IO)) {
            /* MESSAGE IN: target -> host (1-byte command-complete) */
            s_data = (s_data & 0xFF00) | (scsi_read_cmd_byte() & 0xFF);
        } else if (phase == SCSI_BUS_IO) {
            /* DATA IN: not in handshake mode -- handled via DMA path */
        } else if (phase == 0) {
            /* DATA OUT: not in handshake mode -- handled via DMA path */
        }
        si_pump();
        s_req_suppressed = 1;
    } else if (ack_was_set && !ack_now) {
        /* Falling edge of ACK -- target re-asserts REQ for next byte
           (or has already moved on to the next phase). */
        s_req_suppressed = 0;
        si_pump();
    }
}

/* Translate SCSI bus state lines into NCR5380's CSBS register layout. */
static uint8_t si_csbs(void)
{
    uint8_t v = 0;
    if (s_bus_state & SCSI_BUS_RST) v |= 0x80;
    if (s_bus_state & SCSI_BUS_BSY) v |= 0x40;
    if ((s_bus_state & SCSI_BUS_REQ) && !s_req_suppressed) v |= 0x20;
    if (s_bus_state & SCSI_BUS_MSG) v |= 0x10;
    if (s_bus_state & SCSI_BUS_CD)  v |= 0x08;
    if (s_bus_state & SCSI_BUS_IO)  v |= 0x04;
    if (s_bus_state & SCSI_BUS_SEL) v |= 0x02;
    /* bit 0 = DBP (parity); leave 0 */
    return v;
}

/* Bus and Status Register: a mix of phase-match + irq state.  Minimum
   to keep the PROM moving is to report phase-match when the requested
   phase (TCR low 3 bits) equals the bus phase (MSG/CD/IO from CSBS). */
static uint8_t si_bsr(void)
{
    uint8_t v = 0;
    /* bit 7 = End of DMA */
    /* bit 6 = DMA Request */
    if (s_bus_state & SCSI_BUS_REQ) v |= 0x40;
    /* bit 5 = Parity Error */
    /* bit 4 = IRQ */
    if (s_bus_irq) v |= 0x10;
    /* bit 3 = Phase Match */
    {
        uint8_t want = s_tcr & 7;
        uint8_t have = (uint8_t)(((s_bus_state & SCSI_BUS_MSG) ? 4 : 0) |
                                 ((s_bus_state & SCSI_BUS_CD)  ? 2 : 0) |
                                 ((s_bus_state & SCSI_BUS_IO)  ? 1 : 0));
        if (want == have) v |= 0x08;
    }
    /* bit 2 = Busy Error */
    /* bit 1 = ATN */
    if (s_bus_state & SCSI_BUS_ATN) v |= 0x02;
    /* bit 0 = ACK */
    if (s_bus_state & SCSI_BUS_ACK) v |= 0x01;
    return v;
}

extern unsigned char sun3_dvma_read_byte(uint32_t va);
extern void          sun3_dvma_write_byte(uint32_t va, unsigned char v);

extern int trace_scsi;

/* Read the AM9516 chain block at the address held in UDC regs 0x26
   (high 8 bits of bits 23:16) and 0x22 (low 16 bits), and replay the
   pending DMA from sc_dma_read_data / sc_dma_write_data.
   Chain block layout (per RetroCore Sun3SIBoard.cs:646-712 and
   confirmed against TME sun3-mainbus.c):
     +0x00  16-bit  mode/command (0x0182 read / 0x0282 write)
     +0x02  16-bit  high(addr_high<<8 | UDC_ADDR_INFO=0x40)
     +0x04  16-bit  addr_low
     +0x06  16-bit  word_count
     +0x08  16-bit  reserved
     +0x0A  16-bit  channel command
   The real DMA target VA = (cb02_hi << 16) | (cb04_hi << 8) | cb04_lo.
   Length comes from the SI FIFO_COUNT_L register (0x16), not from the
   chain block. */
static void si_udc_run_chain(void)
{
    if (!s_pending_buf || s_pending_size <= 0) {
        /* PROM triggered DMA but scsi.c never produced a buffer for
           this command.  Mark DMA done so we don't hang. */
        s_csr |= SI_CSR_INT_DMA | SI_CSR_FIFO_EMPTY;
        return;
    }

    uint32_t chain_hi_byte = (uint32_t)(s_udc_regs[0x26] >> 8);
    uint32_t chain_lo      = (uint32_t)s_udc_regs[0x22];
    uint32_t chain_addr    = (chain_hi_byte << 16) | chain_lo;

    /* Walk chain block: addr_hi at +0x02 (high byte), addr_lo at +0x04..0x05 */
    uint8_t cb02_hi = sun3_dvma_read_byte(chain_addr + 0x02);
    /*uint8_t cb02_lo = sun3_dvma_read_byte(chain_addr + 0x03);*/  /* UDC_ADDR_INFO flag */
    uint8_t cb04_hi = sun3_dvma_read_byte(chain_addr + 0x04);
    uint8_t cb04_lo = sun3_dvma_read_byte(chain_addr + 0x05);

    uint32_t real_va = ((uint32_t)cb02_hi << 16)
                     | ((uint32_t)cb04_hi << 8)
                     |  (uint32_t)cb04_lo;

    /* Byte count: PROM stages it in SI FIFO_COUNT_L (offset 0x16).  If
       that wasn't programmed (stays 0), fall back to the stashed
       buffer length so a 1-sector boot read still works. */
    int n = (int)s_fifo_count;
    if (n <= 0 || n > s_pending_size) n = s_pending_size;

    if (s_pending_is_read) {
        /* Target -> host: copy from stashed buffer to RAM at real_va. */
        for (int i = 0; i < n; i++)
            sun3_dvma_write_byte(real_va + (uint32_t)i, s_pending_buf[i]);
    } else {
        /* Host -> target: copy from RAM into stashed buffer. */
        for (int i = 0; i < n; i++)
            s_pending_buf[i] = sun3_dvma_read_byte(real_va + (uint32_t)i);
    }

    /* Done.  Update DMA + FIFO state so PROM's si_dma_recv loop exits.
       The PROM's si_wait16() poll after start-DMA waits on
       SI_CSR_INT_NCR5380 (the chip's end-of-DMA interrupt) — set it
       too so the wait exits.  s_bus_irq is the underlying NCR5380 IRQ
       latch; sample it into BSR via si_bsr(). */
    s_dma_addr = real_va + (uint32_t)n;
    s_fifo_count  = 0;
    s_csr |= SI_CSR_INT_DMA | SI_CSR_INT_NCR5380 | SI_CSR_FIFO_EMPTY;
    s_csr &= ~(uint16_t)SI_CSR_DMA_BUS_ERROR;
    s_bus_irq = 1;

    /* Pending buffer consumed. */
    s_pending_buf = NULL;
    s_pending_size = 0;
}

/* Called from sc.c when scsi.c wants to do a DMA transfer.  Sun-3
   defers the actual copy until the PROM triggers the UDC. */
void sun3_si_stash_dma_read(unsigned char *buf, int siz)
{
    s_pending_buf = buf;
    s_pending_size = siz;
    s_pending_is_read = 1;
}

void sun3_si_stash_dma_write(unsigned char *buf, int siz)
{
    s_pending_buf = buf;
    s_pending_size = siz;
    s_pending_is_read = 0;
}

/* --- public API -------------------------------------------------- */

void sun3_si_init(void)
{
    /* trace_scsi = 1; */
    s_icr = 0;
    s_mr = 0;
    s_tcr = 0;
    s_ser = 0;
    s_data = 0;
    s_csr = SI_CSR_FIFO_EMPTY;
    s_dma_addr = 0;
    s_dma_count = 0;
    s_udc_data = 0;
    s_udc_addr = 0;
    s_fifo_count = 0;
    s_bus_state = 0;
    s_bus_irq = 0;
}

uint32_t sun3_si_read(uint32_t off, int size)
{
    /* NCR5380 register window */
    if (off < 8) {
        si_pump();                        /* refresh bus snapshot */
        switch (off) {
        case N5380_CSD:  return s_data & 0xFF;
        case N5380_ICR:  return s_icr;
        case N5380_MR:   return s_mr;
        case N5380_TCR:  return s_tcr;
        case N5380_CSBS: return si_csbs();
        case N5380_BSR:  return si_bsr();
        case N5380_IDR:  return s_data & 0xFF;
        case N5380_RPI:  s_bus_irq = 0; return 0;
        }
    }

    /* SI DMA / CSR / UDC window */
    if (off >= 0x08 && off < 0x0C) {
        int shift = (3 - (int)(off - 0x08)) * 8;
        return (s_dma_addr >> shift) & 0xFF;
    }
    if (off >= 0x0C && off < 0x10) {
        int shift = (3 - (int)(off - 0x0C)) * 8;
        return (s_dma_count >> shift) & 0xFF;
    }
    if (off == 0x10) return (s_udc_data >> 8) & 0xFF;
    if (off == 0x11) return s_udc_data & 0xFF;
    if (off == 0x12) return (s_udc_addr >> 8) & 0xFF;
    if (off == 0x13) return s_udc_addr & 0xFF;
    if (off == 0x16) return (s_fifo_count >> 8) & 0xFF;
    if (off == 0x17) return s_fifo_count & 0xFF;
    if (off == 0x18) {
        if (size >= 2) return s_csr;
        return (s_csr >> 8) & 0xFF;
    }
    if (off == 0x19) return s_csr & 0xFF;

    return 0xFF;
}

void sun3_si_write(uint32_t off, uint32_t value, int size)
{
    /* NCR5380 register window */
    if (off < 8) {
        switch (off) {
        case N5380_ODR:  s_data = (s_data & 0xFF00) | (value & 0xFF); si_pump(); break;
        case N5380_ICR: {
            int ack_was = (s_icr & ICR_ACK) ? 1 : 0;
            s_icr = (uint8_t)value;
            si_pump();
            si_handshake_ack(ack_was);
            break;
        }
        case N5380_MR:   s_mr  = (uint8_t)value; si_pump(); break;
        case N5380_TCR:  s_tcr = (uint8_t)value; si_pump(); break;
        case N5380_SER:  s_ser = (uint8_t)value; break;
        case N5380_SDS:
        case N5380_SDTR:
        case N5380_SDIR:
            /* Start-DMA register writes: real DMA work is driven by
               the SI/UDC chain mechanism (see si_udc_run_chain).  The
               write here is just the host arming the NCR5380 chip
               for the DMA cycle that the SI board will run. */
            break;
        }
        return;
    }

    /* SI DMA / CSR / UDC window */
    if (off >= 0x08 && off < 0x0C) {
        int shift = (3 - (int)(off - 0x08)) * 8;
        s_dma_addr = (s_dma_addr & ~(0xFFu << shift)) | ((value & 0xFF) << shift);
        return;
    }
    if (off >= 0x0C && off < 0x10) {
        int shift = (3 - (int)(off - 0x0C)) * 8;
        s_dma_count = (s_dma_count & ~(0xFFu << shift)) | ((value & 0xFF) << shift);
        return;
    }
    /* AM9516 UDC indirect register file.  PROM almost always uses
       16-bit (move.w) accesses to UDC_DATA / UDC_ADDR — see si_wr#137..
       144 in the boot trace: `move.w #0x002E, (0x12,A5)` then
       `move.w #0x00A0, (0x10,A5)`.  Handle word-sized writes by
       committing the whole value at once, but also support byte-sized
       accesses (the low-byte write is what commits the latched value
       into s_udc_regs[]).  Reg 0x2E = channel command; writing 0xA0
       there (master enable / chain-start) kicks the actual DMA. */
    if (off == 0x10) {
        if (size >= 2) s_udc_data = (uint16_t)value;
        else           s_udc_data = (s_udc_data & 0x00FF) | ((value & 0xFF) << 8);
        if (size >= 2) {
            uint8_t reg_idx = (uint8_t)(s_udc_addr & 0x3F);
            s_udc_regs[reg_idx] = s_udc_data;
            if (reg_idx == 0x2E && (s_udc_data == 0x00A0 || (s_udc_data >> 8) == 0xA0))
                si_udc_run_chain();
        }
        return;
    }
    if (off == 0x11) {
        s_udc_data = (s_udc_data & 0xFF00) | (value & 0xFF);
        uint8_t reg_idx = (uint8_t)(s_udc_addr & 0x3F);
        s_udc_regs[reg_idx] = s_udc_data;
        if (reg_idx == 0x2E && (s_udc_data == 0x00A0 || (s_udc_data >> 8) == 0xA0))
            si_udc_run_chain();
        return;
    }
    if (off == 0x12) {
        if (size >= 2) s_udc_addr = (uint16_t)value;
        else           s_udc_addr = (s_udc_addr & 0x00FF) | ((value & 0xFF) << 8);
        return;
    }
    if (off == 0x13) { s_udc_addr = (s_udc_addr & 0xFF00) | (value & 0xFF); return; }
    if (off == 0x16) {
        if (size >= 2) s_fifo_count = (uint16_t)value;
        else           s_fifo_count = (s_fifo_count & 0x00FF) | ((value & 0xFF) << 8);
        return;
    }
    if (off == 0x17) { s_fifo_count = (s_fifo_count & 0xFF00) | (value & 0xFF); return; }
    /* CSR write at offset 0x18 (word) / 0x19 (low-byte).  Only bits
       0..4 are host-writable on the onboard variant (TME, RetroCore
       Sun3SIBoard.cs); everything else is status / cleared by reset.
       RESET_CTRL and RESET_FIFO are momentary triggers — process the
       reset action, then clear the trigger bits so they don't latch. */
    if (off == 0x18 || off == 0x19) {
        const uint16_t mask = 0x001F;     /* writable bits */
        uint16_t new_writable;
        if (off == 0x18 && size >= 2) {
            new_writable = (uint16_t)(value & mask);
        } else if (off == 0x18 /* size 1, high byte: nothing writable */) {
            return;
        } else /* off == 0x19, low byte */ {
            new_writable = (uint16_t)(value & mask);
        }
        s_csr = (s_csr & ~mask) | new_writable;

        if (s_csr & SI_CSR_RESET_CTRL) {
            /* Reset the chip: clear NCR5380 + all interrupt / error
               status, drop the SCSI bus.  FIFO_EMPTY back on. */
            s_icr = 0; s_mr = 0; s_tcr = 0;
            s_csr &= ~(uint16_t)(SI_CSR_INT_DMA | SI_CSR_INT_NCR5380 |
                                 SI_CSR_DMA_BUS_ERROR | SI_CSR_DMA_CONFLICT);
            s_csr |= SI_CSR_FIFO_EMPTY;
            s_bus_irq = 0;
            s_pending_buf = NULL;
            s_pending_size = 0;
            si_pump();
        }
        /* RESET_CTRL / RESET_FIFO are momentary — clear after handling. */
        s_csr &= ~(uint16_t)(SI_CSR_RESET_CTRL | SI_CSR_RESET_FIFO);
        return;
    }
}
