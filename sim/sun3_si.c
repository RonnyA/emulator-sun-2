/*
 * sun3_si.c -- Sun-3/60 SI SCSI board (onboard variant).
 *
 * Sits at OBIO 0x140000, IPL 2.  Combines an NCR 5380 chip with a small
 * onboard DMA controller (Sun's "si" board, not the VME variant).
 *
 * Register layout (per RetroCore Sun3SIBoard.cs and TME tme/bus/sun3-mainbus):
 *
 *   offset  size  name
 *   0x00..0x07   NCR5380 chip registers (8 byte-wide regs)
 *   0x08..0x0B  4  DMA address (big-endian u32)
 *   0x0C..0x0F  4  DMA byte count (big-endian u32)
 *   0x10..0x11  2  AM9516 UDC data    (chained DMA)
 *   0x12..0x13  2  AM9516 UDC address (chained DMA)
 *   0x14..0x15  2  FIFO data
 *   0x16..0x17  2  FIFO byte count
 *   0x18..0x19  2  CSR (control / status)
 *   0x1A..0x1F  6  VME-only registers (BPR, IV/AM) -- ignored on onboard
 *
 * Bus + target state machine is delegated to scsi3.c.  This file owns
 * the NCR-5380 register decode, the AM9516 UDC indirect register file,
 * the SI CSR + IRQ wiring, and translates between host SI register
 * accesses and the scsi3 per-byte API.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "sim68k.h"
#include "m68k.h"
#include "sim.h"
#include "scsi3.h"
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
static uint8_t  s_odr;          /* last write to ODR (initiator data drive) */
static uint16_t s_csr;
static uint32_t s_dma_addr;
static uint32_t s_dma_count;
static uint16_t s_udc_data;
static uint16_t s_udc_addr;
static uint16_t s_fifo_count;
static uint16_t s_udc_regs[64];

/* NCR 5380 arbitration state (single-initiator simplification). */
static int s_arb_state;   /* 0=idle, 1=arming AIP, 2=AIP-seen (won) */

/* IRQ pending state. */
static int s_bus_irq;
static int s_end_of_dma;

/* Deferred DMA-completion IRQ.  Real hardware fires the SBC_IP /
   end-of-DMA interrupt only after the chip has actually drained the
   FIFO, which (in the kernel's si_sbc_dma_setup sequence) happens
   AFTER the kernel's `junk = SBC_RD.clr` at line si.c:1493 reads RPI
   to clear stale interrupts.  Our chain run executes inline so the
   IRQ would otherwise be set _before_ that RPI read and immediately
   cleared.  Stash the completion in this flag and apply it on the
   next CSR read — by that point the kernel's setup sequence has
   finished and it's polling for SBC_IP. */
static int s_dma_done_pending;
static void si_apply_pending_dma_done(void)
{
    if (!s_dma_done_pending) return;
    s_dma_done_pending = 0;
    s_csr |= SI_CSR_INT_NCR5380 | SI_CSR_FIFO_EMPTY;
    s_bus_irq = 1;
    s_end_of_dma = 1;
}

extern unsigned char sun3_dvma_read_byte (uint32_t va);
extern void          sun3_dvma_write_byte(uint32_t va, unsigned char v);
extern void          int_controller_set  (unsigned int level);
extern void          int_controller_clear(unsigned int level);

extern int trace_scsi;
int trace_si = 0;

/* --- IRQ machinery ------------------------------------------------ */
static int s_irq_asserted;
static void si_irq_eval(void)
{
    int want = ((s_csr & SI_CSR_INT_ENABLE) &&
                (s_csr & (SI_CSR_INT_DMA | SI_CSR_INT_NCR5380 |
                          SI_CSR_DMA_BUS_ERROR))) ? 1 : 0;
    if (want && !s_irq_asserted) {
        if (trace_scsi)
            fprintf(stderr, "[si] IRQ assert (csr=%04X)\n", s_csr);
        int_controller_set(2);
        s_irq_asserted = 1;
    } else if (!want && s_irq_asserted) {
        if (trace_scsi)
            fprintf(stderr, "[si] IRQ clear (csr=%04X)\n", s_csr);
        int_controller_clear(2);
        s_irq_asserted = 0;
    }
}

/* NCR 5380 phase-mismatch IRQ.  Per spec § 4.7: bsr.IRQ is asserted
   when the target's current phase (cbsr MSG/CD/IO bits) differs from
   what the initiator has programmed in tcr, AND mr.DMA = 1.  This is
   how the SunOS si driver gets a kernel interrupt when the target
   advances from COMMAND to STATUS (or DATA to STATUS) on an
   interrupt-mode command.  The chip raises IRQ; we mirror that into
   the SI board's INT_NCR5380 (= SBC_IP) so si_irq_eval() can fire
   IPL 2 to the CPU.

   Edge-triggered: latch on transitions (mr.DMA rising while
   mismatched, or phase change while mr.DMA on) -- never continuously
   re-assert across multiple CBSR reads or it produces an IRQ storm
   the kernel can't drain. */
static int s_pmtch_last_phase = -2;        /* invalidate at startup */
static int s_pmtch_last_mr_dma;

/* Fire IRQ on transition INTO STATUS or MSG_IN phase when mr.DMA is
   set.  These are the canonical "command completion needed"
   transitions the SunOS si driver waits for in interrupt mode.
   Triggering on any phase mismatch (e.g. CDB→DATA_IN with TCR still
   set to TCR_UNSPECIFIED) produces spurious IRQs the kernel handles
   prematurely, manifesting as a NULL-deref bus error deep in mountroot.

   Edge-triggered + armed-once per command.  Re-arm at bus free. */
static int s_pmtch_armed;

/* Edge-triggered evaluation of the SBC IRQ pin, modelled after
 * RetroCore C# scsi_ctrl_changed (HCL.NCR5380SCSI.cs lines 1006-1052).
 *
 * Fires INT_NCR5380 (= SBC_IP) on the rising edge of any of:
 *   - target REQ (the primary chip-level trigger), OR
 *   - SI_CSR_INT_ENABLE  (kernel arms interrupts into existing state)
 *
 * AND
 *   - bus phase != tcr.phase  (mismatch)
 *   - either mr.DMA = 1   (DMA-mode mismatch)
 *     or  mr.DMA = 0 AND SER != 0   (SER-armed non-DMA mismatch)
 *
 * Strict edge-trigger avoids the "unexpected DATA phase" reset loop
 * caused by re-firing on every CSR/MR poll while target holds REQ. */
static int s_prev_target_req;
static int s_prev_int_enable;
static int s_pmtch_last_mr_dma_save;
static void si_eval_phase_irq_edge(void)
{
    int dma_now    = (s_mr & 0x02) ? 1 : 0;
    int p          = scsi3_phase();
    int req_now    = scsi3_target_req();
    int prev_req   = s_prev_target_req;
    int int_en_now = (s_csr & SI_CSR_INT_ENABLE) ? 1 : 0;
    int prev_int_en = s_prev_int_enable;

    s_pmtch_last_mr_dma  = dma_now;
    s_pmtch_last_phase   = p;
    s_prev_target_req    = req_now;
    s_prev_int_enable    = int_en_now;

    if (p < 0)                        return;
    if (!int_en_now)                  return;
    if (!req_now)                     return;
    if ((p & 7) == (s_tcr & 7))       return;     /* no mismatch */

    /* Edge condition: any of the firing-condition inputs just rose
       into a state where REQ is held + phase mismatched.  REQ rising
       is the chip's primary trigger; INT_ENABLE / mr.DMA rising are
       SI-board / chip-mode arming events that the kernel uses to
       latch interrupts after target has already advanced phase. */
    int prev_dma = s_pmtch_last_mr_dma_save;
    s_pmtch_last_mr_dma_save = dma_now;
    int trigger = (req_now && !prev_req)
               || (int_en_now && !prev_int_en)
               || (dma_now && !prev_dma);
    if (!trigger)                     return;

    int condition = 0;
    if (dma_now)              condition = 1;
    else if (s_ser != 0)      condition = 2;
    if (!condition)           return;

    if (trace_scsi)
        fprintf(stderr, "[si] IRQ fire (mode=%d phase=%d tcr=%X dma=%d ser=%02X req=%d int_en=%d trig=%s)\n",
                condition, p, s_tcr & 7, dma_now, s_ser, req_now, int_en_now,
                (req_now && !prev_req) ? "REQ" : "INT_EN");
    s_csr |= SI_CSR_INT_NCR5380;
    s_bus_irq = 1;
    si_irq_eval();
}

/* Push current initiator-driven SCSI signals + ODR data into scsi3. */
static void si_pump(void)
{
    /* SEL is gated on initiator-BSY: during arbitrated selection the
       host first asserts BSY+SEL with own ID on data, then drops BSY
       to release.  scsi3 latches the target id at SEL rising — only
       propagate SEL once init-BSY is clear, otherwise scsi3 sees
       host-id-only and concludes no target. */
    int rst = (s_icr & ICR_RST) ? 1 : 0;
    int sel = ((s_icr & ICR_SEL) && !(s_icr & ICR_BUSY)) ? 1 : 0;
    int ack = (s_icr & ICR_ACK) ? 1 : 0;
    int atn = (s_icr & ICR_ATN) ? 1 : 0;
    int bsy = (s_icr & ICR_BUSY) ? 1 : 0;

    /* During SELECTION the data lines carry (1<<host_id)|(1<<target_id).
       scsi3 strips the host bit internally. */
    scsi3_set_init_data(s_odr);
    scsi3_set_init_lines(bsy, sel, ack, atn, rst);
}

/* --- NCR 5380 register synthesis ---------------------------------- */

/* Encode scsi3 phase (-1, 0..7) into CBSR phase bits. */
static uint8_t phase_bits_csbs(void)
{
    int p = scsi3_phase();
    if (p < 0) return 0;
    uint8_t v = 0;
    if (p & 4) v |= 0x10;     /* MSG */
    if (p & 2) v |= 0x08;     /* CD */
    if (p & 1) v |= 0x04;     /* IO */
    return v;
}

static uint8_t si_csbs(void)
{
    uint8_t v = 0;
    if (scsi3_target_bsy()) v |= 0x40;
    if (scsi3_target_req()) v |= 0x20;
    v |= phase_bits_csbs();
    /* SEL on the live bus is the initiator's drive (gated). */
    if ((s_icr & ICR_SEL) && !(s_icr & ICR_BUSY)) v |= 0x02;
    return v;
}

static uint8_t si_bsr(void)
{
    uint8_t v = 0;
    if (s_end_of_dma) v |= 0x80;          /* End of DMA */
    if (scsi3_target_req()) v |= 0x40;    /* DMA Request */
    if (s_bus_irq)    v |= 0x10;          /* IRQ */
    /* Phase Match: tcr low 3 bits == current bus phase. */
    {
        uint8_t want = s_tcr & 7;
        int p = scsi3_phase();
        uint8_t have = (p < 0) ? 0 : (uint8_t)p;
        if (want == have) v |= 0x08;
    }
    if (s_icr & ICR_ATN) v |= 0x02;
    if (s_icr & ICR_ACK) v |= 0x01;
    return v;
}

/* --- AM9516 UDC chain run ----------------------------------------- */

static void si_udc_run_chain(void)
{
    /* Walk the chain block at (udc_regs[0x26]:udc_regs[0x22]) to find
       the real DMA target VA.  See spec doc § (UDC) and RetroCore
       Sun3SIBoard.cs. */
    uint32_t chain_hi_byte = (uint32_t)(s_udc_regs[0x26] >> 8);
    uint32_t chain_lo      = (uint32_t)s_udc_regs[0x22];
    uint32_t chain_addr    = (chain_hi_byte << 16) | chain_lo;

    uint8_t cb02_hi = sun3_dvma_read_byte(chain_addr + 0x02);
    uint8_t cb04_hi = sun3_dvma_read_byte(chain_addr + 0x04);
    uint8_t cb04_lo = sun3_dvma_read_byte(chain_addr + 0x05);

    uint32_t real_va = ((uint32_t)cb02_hi << 16)
                     | ((uint32_t)cb04_hi << 8)
                     |  (uint32_t)cb04_lo;

    int n = (int)s_fifo_count;
    int xfered = 0;
    int is_send = (s_csr & SI_CSR_DMA_SEND) ? 1 : 0;

    if (n <= 0) {
        if (trace_si)
            fprintf(stderr, "[si] CHAIN-RUN with bcr=0; nothing to do\n");
    } else if (is_send) {
        /* Host -> target (DATA_OUT). */
        for (int i = 0; i < n; i++) {
            uint8_t b = sun3_dvma_read_byte(real_va + (uint32_t)i);
            if (!scsi3_dma_out(b)) break;
            xfered++;
        }
    } else {
        /* Target -> host (DATA_IN). */
        for (int i = 0; i < n; i++) {
            uint8_t b;
            if (!scsi3_dma_in(&b)) break;
            sun3_dvma_write_byte(real_va + (uint32_t)i, b);
            xfered++;
        }
    }

    if (trace_si)
        fprintf(stderr, "[si] CHAIN-RUN %s va=%08X bcr=%d xfered=%d phase=%d\n",
                is_send ? "WRITE" : "READ",
                real_va, n, xfered, scsi3_phase());

    /* Update SI state: BCR drained, FIFO empty.  Defer the
       INT_NCR5380 / s_bus_irq / s_end_of_dma assertion — see comment
       on s_dma_done_pending.  FIFO_EMPTY is safe to set immediately
       because si_dma_recv polls for it AFTER the RPI clear. */
    s_dma_addr   = real_va + (uint32_t)xfered;
    s_fifo_count = 0;
    s_csr |= SI_CSR_FIFO_EMPTY;
    s_csr &= ~(uint16_t)SI_CSR_DMA_BUS_ERROR;
    s_dma_done_pending = 1;
    si_irq_eval();
}

/* --- public API --------------------------------------------------- */

/* Compatibility stubs.  In the original architecture these were called
   from sc.c when scsi.c's dispatcher fired sc_dma_*_data on Sun-3.
   With the scsi3 split they are dead — Sun-3 routes through sun3_si.c
   which calls scsi3 directly, never via sc.c.  Stubs keep sc.c
   linkable.  Left intentionally empty. */
void sun3_si_stash_dma_read (unsigned char *buf, int siz) { (void)buf; (void)siz; }
void sun3_si_stash_dma_write(unsigned char *buf, int siz) { (void)buf; (void)siz; }

void sun3_si_init(void)
{
    s_icr = 0;
    s_mr = 0;
    s_tcr = 0;
    s_ser = 0;
    s_odr = 0;
    s_csr = SI_CSR_FIFO_EMPTY;
    s_dma_addr = 0;
    s_dma_count = 0;
    s_udc_data = 0;
    s_udc_addr = 0;
    s_fifo_count = 0;
    s_bus_irq = 0;
    s_end_of_dma = 0;
    s_dma_done_pending = 0;
    s_arb_state = 0;
    s_irq_asserted = 0;
    memset(s_udc_regs, 0, sizeof(s_udc_regs));
    scsi3_init();
}

uint32_t sun3_si_read(uint32_t off, int size)
{
    /* NCR 5380 register window. */
    if (off < 8) {
        si_pump();
        switch (off) {
        case N5380_CSD:  return scsi3_bus_data();
        case N5380_ICR: {
            uint8_t v = s_icr;
            if (s_mr & 0x01) {              /* ARBITRATE active */
                if (s_arb_state == 1) {
                    v |= 0x40;              /* AIP=1 */
                    s_arb_state = 2;
                }
            }
            return v;
        }
        case N5380_MR:   return s_mr;
        case N5380_TCR:  return s_tcr;
        case N5380_CSBS:
            si_eval_phase_irq_edge();
            return si_csbs();
        case N5380_BSR:
            si_eval_phase_irq_edge();
            return si_bsr();
        case N5380_IDR:  return scsi3_bus_data();
        case N5380_RPI:
            /* Reading clears INTR/PERR/BERR latches.  Do NOT clear
               s_pmtch_armed here — that flag tracks "fired on this
               REQ assertion" and must persist until REQ falls
               (between bytes / phase change).  Otherwise a kernel
               that re-arms by writing MR/etc. while still in the
               same mismatched state will trigger an IRQ storm. */
            s_bus_irq    = 0;
            s_end_of_dma = 0;
            s_csr &= ~(uint16_t)(SI_CSR_INT_NCR5380 | SI_CSR_INT_DMA);
            si_irq_eval();
            return 0;
        }
    }

    /* SI DMA / CSR / UDC window. */
    if (off >= 0x08 && off < 0x0C) {
        int shift = (3 - (int)(off - 0x08)) * 8;
        return (s_dma_addr >> shift) & 0xFF;
    }
    if (off >= 0x0C && off < 0x10) {
        int shift = (3 - (int)(off - 0x0C)) * 8;
        return (s_dma_count >> shift) & 0xFF;
    }
    /* AM9516 UDC indirect register file: reads return regs[raddr]. */
    if (off == 0x10) {
        uint8_t reg_idx = (uint8_t)(s_udc_addr & 0x3F);
        uint16_t v = s_udc_regs[reg_idx];
        return (v >> 8) & 0xFF;
    }
    if (off == 0x11) {
        uint8_t reg_idx = (uint8_t)(s_udc_addr & 0x3F);
        return s_udc_regs[reg_idx] & 0xFF;
    }
    if (off == 0x12) return (s_udc_addr >> 8) & 0xFF;
    if (off == 0x13) return s_udc_addr & 0xFF;
    if (off == 0x16) return (s_fifo_count >> 8) & 0xFF;
    if (off == 0x17) return s_fifo_count & 0xFF;
    if (off == 0x18) {
        si_apply_pending_dma_done();
        si_irq_eval();
        if (size >= 2) return s_csr;
        return (s_csr >> 8) & 0xFF;
    }
    if (off == 0x19) {
        si_apply_pending_dma_done();
        si_irq_eval();
        return s_csr & 0xFF;
    }

    return 0xFF;
}

void sun3_si_write(uint32_t off, uint32_t value, int size)
{
    /* NCR 5380 register window. */
    if (off < 8) {
        switch (off) {
        case N5380_ODR:
            s_odr = (uint8_t)value;
            si_pump();
            si_eval_phase_irq_edge();
            break;
        case N5380_ICR:
            s_icr = (uint8_t)value;
            si_pump();
            si_eval_phase_irq_edge();
            break;
        case N5380_MR: {
            uint8_t old_mr = s_mr;
            s_mr = (uint8_t)value;
            if (trace_scsi)
                fprintf(stderr, "[si] MR<- %02X (old=%02X dma=%d->%d)\n",
                        s_mr, old_mr, (old_mr&2)?1:0, (s_mr&2)?1:0);
            if (!(old_mr & 0x01) && (s_mr & 0x01))
                s_arb_state = 1;
            else if ((old_mr & 0x01) && !(s_mr & 0x01))
                s_arb_state = 0;
            si_pump();
            /* MR.DMA rising edge: latch phase-mismatch IRQ if the
               target is already in a phase different from tcr. */
            if (!(old_mr & 0x02) && (s_mr & 0x02))
                si_eval_phase_irq_edge();
            break;
        }
        case N5380_TCR:  s_tcr = (uint8_t)value; si_pump(); si_eval_phase_irq_edge(); break;
        case N5380_SER:  s_ser = (uint8_t)value; si_eval_phase_irq_edge(); break;
        case N5380_SDS:
        case N5380_SDTR:
        case N5380_SDIR:
            /* Start-DMA register writes arm the chip's DMA mode bit;
               the actual byte transfers run via the UDC chain run. */
            break;
        }
        return;
    }

    /* SI DMA / CSR / UDC window. */
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
    if (off == 0x10) {
        if (size >= 2) s_udc_data = (uint16_t)value;
        else           s_udc_data = (s_udc_data & 0x00FF) | ((value & 0xFF) << 8);
        if (size >= 2) {
            uint8_t reg_idx = (uint8_t)(s_udc_addr & 0x3F);
            s_udc_regs[reg_idx] = s_udc_data;
            if (trace_scsi)
                fprintf(stderr, "[si] UDC reg[%02X] <- %04X\n", reg_idx, s_udc_data);
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
    if (off == 0x17) {
        s_fifo_count = (s_fifo_count & 0xFF00) | (value & 0xFF);
        return;
    }
    /* CSR write: bits 0..4 are host-writable on the onboard variant.
       Per spec § 5: RESET_CTRL (bit 0) and RESET_FIFO (bit 1) are
       active-LOW level signals — bit=0 holds the unit in reset.  The
       kernel's si_reset sequence writes csr=0 (assert reset on both
       bits), then csr=SI_CSR_SCSI_RES|SI_CSR_FIFO_RES (bits 0+1 set,
       deassert reset).  We detect the 1→0 falling edge of RESET_CTRL
       to perform the chip-reset action. */
    if (off == 0x18 || off == 0x19) {
        const uint16_t mask = 0x001F;
        uint16_t new_writable;
        uint16_t old_csr = s_csr;
        if (off == 0x18 && size >= 2) {
            new_writable = (uint16_t)(value & mask);
        } else if (off == 0x18 /* size 1, high byte: nothing writable */) {
            return;
        } else /* off == 0x19, low byte */ {
            new_writable = (uint16_t)(value & mask);
        }
        s_csr = (s_csr & ~mask) | new_writable;
        if (trace_scsi)
            fprintf(stderr, "[si] CSR<- %04X (off=%X) old=%04X new=%04X\n",
                    (unsigned)value, off, old_csr, s_csr);

        /* Falling edge of RESET_CTRL (bit 0): chip-reset asserted. */
        if ((old_csr & SI_CSR_RESET_CTRL) && !(s_csr & SI_CSR_RESET_CTRL)) {
            if (trace_scsi)
                fprintf(stderr, "[si] CSR.RESET_CTRL asserted (1->0) -> bus reset\n");
            s_icr = 0; s_mr = 0; s_tcr = 0;
            s_csr &= ~(uint16_t)(SI_CSR_INT_DMA | SI_CSR_INT_NCR5380 |
                                 SI_CSR_DMA_BUS_ERROR | SI_CSR_DMA_CONFLICT);
            s_csr |= SI_CSR_FIFO_EMPTY;
            s_bus_irq = 0;
            s_end_of_dma = 0;
            scsi3_init();
            si_pump();
        }
        si_irq_eval();
        /* INT_ENABLE may have just risen into a pre-existing phase
           mismatch; re-evaluate the SBC IRQ level. */
        si_eval_phase_irq_edge();
        si_irq_eval();
        return;
    }
}
