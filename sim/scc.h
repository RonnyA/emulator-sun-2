/*
 * sun-2 emulator — Z8530 SCC, per-instance state.
 *
 * Two physical Z8530 chips on a Sun-2/120:
 *   - kbd/mouse chip at OBMEM 0x780000
 *   - serial chip   at OBMEM 0x7F2000 (ttya = chan A, ttyb = chan B)
 *
 * Each chip owns:
 *   - chip-wide  : RegisterPtr (WR0 reg-select), MasterIntEnable (WR9.MIE),
 *                  InterruptVector (WR2), InterruptPending (RR3 mask),
 *                  CurrentIRQStatus (current IRQ-line state).
 *   - per-channel: WR0..15 shadow, RR0..15 shadow, input FIFO.
 *
 * Mirrors the architecture of RetroCore's C# Z8530SCC (see
 * E:/Dev/Repos/Ronny/RetroCore/Emulated.HW/Zilog/SCC/Z8530/Z8530SCC.cs:
 * regs.RegisterPtr, regs.MasterInterruptControl, regs.InterruptPending,
 * regs.CurrentIRQStatus -- and the central CheckIrq() at line 762).
 *
 * Shared globals are gone: each chip's interrupts are independent, so
 * a level-6 service of the kbd chip can no longer block the serial
 * chip's IRQ from re-asserting.
 */

#ifndef SUN2_SCC_H
#define SUN2_SCC_H

#include <stdint.h>

/* ===== Z8530 register bit definitions (subset we model). ===== */

/* RR0 -- Transmit/Receive buffer status, modem status */
#define SCC_RR0_RX_READY   0x01    /* received character available */
#define SCC_RR0_TX_READY   0x04    /* transmit buffer empty */
#define SCC_RR0_DCD        0x08
#define SCC_RR0_CTS        0x20

/* RR1 -- Special-receive condition */
#define SCC_RR1_ALL_SENT   0x01

/* RR3 -- Interrupt Pending bits (chan A only; chan B always reads 0) */
#define SCC_RR3_IP_B_STAT  0x01
#define SCC_RR3_IP_B_TX    0x02
#define SCC_RR3_IP_B_RX    0x04
#define SCC_RR3_IP_A_STAT  0x08
#define SCC_RR3_IP_A_TX    0x10
#define SCC_RR3_IP_A_RX    0x20

/* WR1 -- per-channel interrupt enable */
#define SCC_WR1_EXT_IE     0x01    /* SIE -- ext/status int enable */
#define SCC_WR1_TX_IE      0x02    /* TIE -- transmit int enable   */
#define SCC_WR1_RX_FIRST_IE 0x08
#define SCC_WR1_RX_IE      0x10    /* RIE -- receive int enable (any) */
#define SCC_WR1_RX_IE_MASK 0x18    /* WR1 bits 3-4: any RX-int mode  */

/* WR9 -- chip-wide master int control */
#define SCC_WR9_VIS        0x01
#define SCC_WR9_NV         0x02
#define SCC_WR9_DLC        0x04
#define SCC_WR9_MIE        0x08    /* master interrupt enable */
#define SCC_WR9_STAT_HIGH  0x10    /* shift status bits to 6:4 instead of 3:1 */
#define SCC_WR9_RESET_MASK 0xC0
#define SCC_WR9_RESET_B    0x40
#define SCC_WR9_RESET_A    0x80
#define SCC_WR9_RESET_HW   0xC0

/* ===== Per-chip state. ===== */

#define SCC_FIFO_SIZE 16

typedef struct scc_fifo_s {
    int   count;
    int   wptr;
    int   rptr;
    uint8_t buf[SCC_FIFO_SIZE];
    uint8_t last_byte;     /* "last popped" — what real chip returns on empty */
} scc_fifo_t;

/* Channel index inside a chip: 0 = chan B, 1 = chan A.
   This matches the Sun-2 "address bit 2 selects channel" + the SunOS
   convention (`zsaddr | 4` = chan A). */
#define SCC_CH_B 0
#define SCC_CH_A 1

struct scc_chip_s;

/* Callback types -- the chip calls these to talk to the outside world.
   on_tx_byte: a byte was written to the chip's data port.
               ch=0 = chan B, ch=1 = chan A. Implementation forwards to
               the right tty/keyboard sink.
   on_irq:    chip's IRQ output line state changed.  asserted = 1 to
              raise, 0 to drop.  Implementation routes to the m68k
              interrupt controller (typically int_controller_set/clear
              with IRQ_SCC). */
typedef void (*scc_tx_cb_t)(struct scc_chip_s *chip, int ch, uint8_t byte);
typedef void (*scc_irq_cb_t)(struct scc_chip_s *chip, int asserted);

typedef struct scc_chip_s {
    const char  *name;        /* "kbd" / "serial" -- for trace output */
    int          init_done;

    /* --- chip-wide registers --- */
    uint8_t      register_ptr;        /* low 4 bits select WR/RR */
    uint8_t      master_int_enable;   /* WR9.MIE */
    uint8_t      interrupt_vector;    /* WR2 */
    uint8_t      vector_incl_stat;    /* WR9.VIS */
    uint8_t      stat_high;           /* WR9.STAT_HIGH */
    uint8_t      interrupt_pending;   /* RR3 bitmask, chip-wide */

    /* --- per-channel shadows --- */
    uint8_t      wr[2][16];
    uint8_t      rr[2][16];
    scc_fifo_t   ififo[2];

    /* --- IRQ-line tracking --- */
    int          irq_asserted;

    /* --- callbacks --- */
    scc_tx_cb_t  on_tx_byte;
    scc_irq_cb_t on_irq;
} scc_chip_t;

/* ===== Public API ===== */

void scc_chip_init  (scc_chip_t *chip, const char *name);

/* Bus access.  offset = low 4 bits of the SCC address (Sun-2 layout):
     0 = ctlB, 2 = dataB, 4 = ctlA, 6 = dataA */
unsigned int scc_chip_read (scc_chip_t *chip, unsigned int offset);
void         scc_chip_write(scc_chip_t *chip, unsigned int offset, unsigned int value);

/* Push a received byte into the chip's input FIFO. ch=0=B, 1=A. */
void scc_chip_in_push(scc_chip_t *chip, int ch, uint8_t byte);

/* Manually request an IP bit be set (e.g. external/status change). */
void scc_chip_raise(scc_chip_t *chip, uint8_t rr3_bit);

/* Re-evaluate IRQ line state and call on_irq if it changed. */
void scc_chip_check_irq(scc_chip_t *chip);

/* IRQ ack from CPU.  De-asserts line, then re-evaluates -- if more
   sources are still pending, line goes back high.  Returns autovector
   number for the m68k. */
int  scc_chip_ack(scc_chip_t *chip);

/* WR9 reset commands dispatch here. */
void scc_chip_reset_channel(scc_chip_t *chip, int ch);
void scc_chip_reset_world  (scc_chip_t *chip);

/* Driven from the main loop; pumps the FIFO->RX-int path for bytes
   that arrived between bus accesses. */
void scc_chip_update(scc_chip_t *chip);

/* Trace flag setup -- probe SCC_*_TRACE env vars once at startup. */
void scc_init_traces(void);

#endif /* SUN2_SCC_H */
