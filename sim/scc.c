/*
 * sun-2 emulator
 * 10/2014  Brad Parker <brad@heeltoe.com>
 *
 * SCC serial chip emulation -- per-instance Z8530.
 *
 * The Sun-2/120 has TWO physical Z8530 chips: one for kbd/mouse
 * (chan A=kbd, chan B=mouse) and one for the serial ports
 * (chan A=ttya, chan B=ttyb).  This module models a chip as
 * scc_chip_t.  All state -- including RegisterPtr, MIE, RR3 IP mask,
 * IRQ line -- is per-chip, mirroring the layout RetroCore uses in
 * Z8530SCC.cs (Channel.cs, Registers.cs).
 *
 * The legacy globals (scc_init[], scc_wr[][], scc_rr[][], etc.) are
 * kept as a shim so test_scc.c (which #include "scc.c") keeps working
 * during the refactor.  The shim funnels everything through two static
 * chip instances.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sim68k.h"
#include "m68k.h"
#include "sim.h"
#include "scc.h"
#include "scc_tcp.h"

extern int quiet;
int trace_scc = 0;

/* --- trace flags resolved once at module init --------------------- */
static int scc_wr_trace_enabled  = 0;
static int scc_int_trace_enabled = 0;
static int scc_rd_trace_enabled  = 0;

void scc_init_traces(void)
{
    const char *e;
    e = getenv("SCC_WR_TRACE");
    scc_wr_trace_enabled  = (e && *e && *e != '0');
    e = getenv("SCC_INT_TRACE");
    scc_int_trace_enabled = (e && *e && *e != '0');
    e = getenv("SCC_RD_TRACE");
    scc_rd_trace_enabled  = (e && *e && *e != '0');
}

/* ============================================================== */
/* FIFO helpers                                                   */
/* ============================================================== */

static void fifo_clear(scc_fifo_t *f)
{
    f->count = 0;
    f->wptr  = 0;
    f->rptr  = 0;
}

static int fifo_push(scc_fifo_t *f, uint8_t b)
{
    if (f->count >= SCC_FIFO_SIZE) return 0;
    f->buf[f->wptr++] = b;
    f->wptr &= (SCC_FIFO_SIZE - 1);
    f->count++;
    return 1;
}

static int fifo_pop(scc_fifo_t *f, uint8_t *out)
{
    if (f->count <= 0) {
        /* Datasheet: data port returns "the last received byte" when
           the receiver is idle.  Returning 0xff broke programs that
           re-read after a short delay. */
        *out = f->last_byte;
        return 0;
    }
    f->last_byte = f->buf[f->rptr++];
    f->rptr &= (SCC_FIFO_SIZE - 1);
    f->count--;
    *out = f->last_byte;
    return 1;
}

/* ============================================================== */
/* Per-chip core                                                  */
/* ============================================================== */

static void chip_set_rr0_rx(scc_chip_t *chip, int ch)
{
    if (chip->ififo[ch].count > 0)
        chip->rr[ch][0] |=  SCC_RR0_RX_READY;
    else
        chip->rr[ch][0] &= ~SCC_RR0_RX_READY;
}

void scc_chip_init(scc_chip_t *chip, const char *name)
{
    memset(chip, 0, sizeof(*chip));
    chip->name = name ? name : "scc";
    /* Channel-reset defaults per Z8530 datasheet (Channel.cs:Clear()). */
    chip->rr[0][0] = SCC_RR0_TX_READY;
    chip->rr[1][0] = SCC_RR0_TX_READY;
    chip->rr[0][1] = SCC_RR1_ALL_SENT;
    chip->rr[1][1] = SCC_RR1_ALL_SENT;
    chip->init_done = 1;
}

void scc_chip_reset_channel(scc_chip_t *chip, int ch)
{
    int i;
    for (i = 1; i < 16; i++) {
        chip->wr[ch][i] = 0;
        chip->rr[ch][i] = 0;
    }
    chip->rr[ch][0] = SCC_RR0_TX_READY;
    chip->rr[ch][1] = SCC_RR1_ALL_SENT;
    fifo_clear(&chip->ififo[ch]);

    /* Drop any pending IP bits owned by this channel. */
    if (ch == SCC_CH_A)
        chip->interrupt_pending &= ~(SCC_RR3_IP_A_STAT | SCC_RR3_IP_A_TX | SCC_RR3_IP_A_RX);
    else
        chip->interrupt_pending &= ~(SCC_RR3_IP_B_STAT | SCC_RR3_IP_B_TX | SCC_RR3_IP_B_RX);
}

void scc_chip_reset_world(scc_chip_t *chip)
{
    scc_chip_reset_channel(chip, 0);
    scc_chip_reset_channel(chip, 1);
    chip->register_ptr = 0;
    chip->master_int_enable = 0;
    chip->vector_incl_stat  = 0;
    chip->stat_high         = 0;
    chip->interrupt_pending = 0;
    if (chip->irq_asserted && chip->on_irq) chip->on_irq(chip, 0);
    chip->irq_asserted = 0;
}

/* Central IRQ re-evaluation.  Mirrors Z8530SCC.cs:CheckIrq() (line 762). */
void scc_chip_check_irq(scc_chip_t *chip)
{
    int should_assert = (chip->interrupt_pending != 0)
                        && (chip->master_int_enable != 0);

    if (should_assert && !chip->irq_asserted) {
        chip->irq_asserted = 1;
        if (chip->on_irq) chip->on_irq(chip, 1);
    } else if (!should_assert && chip->irq_asserted) {
        chip->irq_asserted = 0;
        if (chip->on_irq) chip->on_irq(chip, 0);
    }
}

void scc_chip_raise(scc_chip_t *chip, uint8_t rr3_bit)
{
    chip->interrupt_pending |= rr3_bit;
    scc_chip_check_irq(chip);
}

int scc_chip_ack(scc_chip_t *chip)
{
    if (chip->irq_asserted) {
        chip->irq_asserted = 0;
        if (chip->on_irq) chip->on_irq(chip, 0);
    }
    /* Don't clear interrupt_pending here -- the kernel does that
       explicitly with WR0=RESET_TXINT/CLR_INTR.  Don't re-assert
       inside the ack either: that creates an immediate high->low->
       high transition that the m68k can re-vector into before the
       current handler even gets to run, leading to runaway nesting.
       The handler will read RR0/RR2/RR3 inside its body, and any of
       those reads goes through chip_read_ctl which re-evaluates IRQ
       at the right time. */
    return M68K_INT_ACK_AUTOVECTOR;
}

/* ===== RR2 modified-vector encoding =====
   Per Z8530 datasheet & RetroCore:ReadRegister2():
     Chan A read of RR2 returns the unmodified InterruptVector (WR2).
     Chan B read of RR2 returns the vector with bits filled from the
     highest-priority pending source.  Encoding (status-low, V3:V1):
       0 0 0  Ch B Tx Buffer Empty
       0 0 1  Ch B External / Status
       0 1 0  Ch B Rx Char Available
       0 1 1  Ch B Special Receive
       1 0 0  Ch A Tx Buffer Empty
       1 0 1  Ch A External / Status
       1 1 0  Ch A Rx Char Available
       1 1 1  Ch A Special Receive (also "no interrupt" sentinel)
   When stat_high (WR9.STAT_HIGH) is set, the bits go to V6:V4 instead.
   Computed dynamically from interrupt_pending so a stale RR2 from an
   already-acked source can't mislead the SunOS dispatcher. */
static uint8_t chip_compute_rr2(scc_chip_t *chip, int chan)
{
    uint8_t vec = chip->interrupt_vector;
    uint8_t code;

    if (chan == SCC_CH_A) return vec;

    if      (chip->interrupt_pending & SCC_RR3_IP_A_STAT) code = 0x05;  /* 101 */
    else if (chip->interrupt_pending & SCC_RR3_IP_A_RX)   code = 0x06;  /* 110 */
    else if (chip->interrupt_pending & SCC_RR3_IP_A_TX)   code = 0x04;  /* 100 */
    else if (chip->interrupt_pending & SCC_RR3_IP_B_STAT) code = 0x01;  /* 001 */
    else if (chip->interrupt_pending & SCC_RR3_IP_B_RX)   code = 0x02;  /* 010 */
    else if (chip->interrupt_pending & SCC_RR3_IP_B_TX)   code = 0x00;  /* 000 */
    else                                                  code = 0x03;  /* 011 -- "no int" */

    if (chip->stat_high) {
        /* Place code in bits 6:4. */
        return (uint8_t)((vec & 0x8f) | (code << 4));
    } else {
        /* Place code in bits 3:1. */
        return (uint8_t)((vec & 0xf1) | (code << 1));
    }
}

/* ============================================================== */
/* Register-port access                                           */
/* ============================================================== */

static unsigned int chip_read_ctl(scc_chip_t *chip, int chan)
{
    uint8_t value;
    int reg = chip->register_ptr & 0x0f;

    switch (reg) {
    case 0:
        chip_set_rr0_rx(chip, chan);
        chip->rr[chan][0] |= SCC_RR0_TX_READY;     /* TX is "always ready" -- we send instantly */
        value = chip->rr[chan][0];
        break;
    case 2:
        value = chip_compute_rr2(chip, chan);
        break;
    case 3:
        /* RR3 is only readable on chan A; chan B always returns 0. */
        value = (chan == SCC_CH_A) ? chip->interrupt_pending : 0;
        break;
    default:
        value = chip->rr[chan][reg];
        break;
    }

    /* All reads reset the register pointer (per datasheet & RetroCore). */
    chip->register_ptr = 0;
    return value;
}

static unsigned int chip_read_data(scc_chip_t *chip, int chan)
{
    uint8_t b;
    fifo_pop(&chip->ififo[chan], &b);

    /* FIFO drained -- clear RX pending in RR3 + RR0, re-eval IRQ. */
    if (chip->ififo[chan].count == 0) {
        chip->rr[chan][0] &= ~SCC_RR0_RX_READY;
        if (chan == SCC_CH_A)
            chip->interrupt_pending &= ~SCC_RR3_IP_A_RX;
        else
            chip->interrupt_pending &= ~SCC_RR3_IP_B_RX;
        scc_chip_check_irq(chip);
    }
    return b;
}

static void chip_handle_wr0_cmd(scc_chip_t *chip, int chan, uint8_t value)
{
    int wr0_cmd = (value >> 3) & 0x07;
    int rst_cmd = (value >> 6) & 0x03;

    switch (wr0_cmd) {
    case 0: /* NULL */
    case 1: /* POINT_HIGH -- handled in caller via reg-pointer math */
        break;
    case 2: /* RESET_STATUS -- relatch ext/status, clear ext IP for this chan */
        if (chan == SCC_CH_A) chip->interrupt_pending &= ~SCC_RR3_IP_A_STAT;
        else                  chip->interrupt_pending &= ~SCC_RR3_IP_B_STAT;
        scc_chip_check_irq(chip);
        break;
    case 3: /* SEND_ABORT (SDLC) */
        break;
    case 4: /* ENABLE_INT_NEXT_RX */
        break;
    case 5: /* RESET_TXINT (0x28) -- clear IP_x_TX */
        if (chan == SCC_CH_A) chip->interrupt_pending &= ~SCC_RR3_IP_A_TX;
        else                  chip->interrupt_pending &= ~SCC_RR3_IP_B_TX;
        scc_chip_check_irq(chip);
        break;
    case 6: /* RESET_ERRORS (0x30) -- clear RR1 PE/DO/FE bits */
        chip->rr[chan][1] &= ~0xF0;
        break;
    case 7: /* RESET_HIGHEST_IUS / CLR_INTR (0x38) */
        /* No effect on RR3 IP bits per datasheet -- those clear via
           explicit RESET_TXINT / RESET_STATUS / data-port read.  But
           the IUS chain is per-source; on a level-triggered model the
           IRQ line drops here only if no IP bits remain. */
        scc_chip_check_irq(chip);
        break;
    }
    (void)rst_cmd; /* CRC reset / Tx underrun reset -- not modelled */
}

static void chip_handle_wr9(scc_chip_t *chip, uint8_t value)
{
    /* Reset commands.  Per zsreg.h: bits 6-7 = reset selector.
       0x40 = chan B, 0x80 = chan A, 0xC0 = world. */
    switch (value & SCC_WR9_RESET_MASK) {
    case SCC_WR9_RESET_HW:
        if (scc_int_trace_enabled)
            fprintf(stderr, "scc-int: %s WR9 <- 0x%02x  RESET_WORLD\n",
                    chip->name, value);
        if (!quiet) printf("scc-%s: WR9 RESET_WORLD\n", chip->name);
        scc_chip_reset_world(chip);
        return;
    case SCC_WR9_RESET_A:
        if (scc_int_trace_enabled)
            fprintf(stderr, "scc-int: %s WR9 <- 0x%02x  RESET_CHAN_A\n",
                    chip->name, value);
        if (!quiet) printf("scc-%s: WR9 RESET_CHAN_A\n", chip->name);
        scc_chip_reset_channel(chip, SCC_CH_A);
        scc_chip_check_irq(chip);
        return;
    case SCC_WR9_RESET_B:
        if (scc_int_trace_enabled)
            fprintf(stderr, "scc-int: %s WR9 <- 0x%02x  RESET_CHAN_B\n",
                    chip->name, value);
        if (!quiet) printf("scc-%s: WR9 RESET_CHAN_B\n", chip->name);
        scc_chip_reset_channel(chip, SCC_CH_B);
        scc_chip_check_irq(chip);
        return;
    }

    /* No reset command -- just store the WR9 mode bits. */
    chip->master_int_enable = (value & SCC_WR9_MIE) ? 1 : 0;
    chip->vector_incl_stat  = (value & SCC_WR9_VIS) ? 1 : 0;
    chip->stat_high         = (value & SCC_WR9_STAT_HIGH) ? 1 : 0;

    if (scc_int_trace_enabled)
        fprintf(stderr,
                "scc-int: %s WR9 <- 0x%02x  MIE=%d VIS=%d StatHigh=%d  IPmask=0x%02x\n",
                chip->name, value,
                chip->master_int_enable, chip->vector_incl_stat,
                chip->stat_high, chip->interrupt_pending);

    scc_chip_check_irq(chip);
}

static void chip_write_ctl(scc_chip_t *chip, int chan, uint8_t value)
{
    /* Z8530 register-pointer protocol (per the chip datasheet, MAME's
       z80scc, and RetroCore's Z8530SCC.cs:Write()):
       - The pointer is just an integer 0..15.
       - Every ctl write stores `value` in WR[pointer].
       - When pointer == 0 the value is ALSO interpreted as WR0:
           low 3 bits   = next pointer;
           bits 3-5     = WR0 command (NULL, POINT_HIGH, RESET_TXINT, ...);
           bits 6-7     = CRC reset;
         If the command is POINT_HIGH the next pointer becomes reg+8.
       - After any non-WR0 write, the pointer auto-resets to 0.
       There is no "primed" flag: the pointer state is the only state. */
    int reg = chip->register_ptr & 0x0f;
    uint8_t next_ptr = 0;

    if (scc_int_trace_enabled)
        fprintf(stderr, "scc-int: %s ctl-write chan=%d val=0x%02x ptr=%d\n",
                chip->name, chan, value, reg);

    chip->wr[chan][reg] = value;

    if (reg == 0) {
        /* WR0: parse low 3 bits + POINT_HIGH, fire WR0 commands. */
        int low = value & 0x07;
        int cmd = (value >> 3) & 0x07;
        next_ptr = (uint8_t)((cmd == 1) ? (low | 0x08) : low);
        chip_handle_wr0_cmd(chip, chan, value);
        chip->register_ptr = next_ptr;
        return;
    }

    switch (reg) {
    case 1:
        if (scc_int_trace_enabled)
            fprintf(stderr, "scc-int: %s WR1 chan=%d <- 0x%02x  TIE=%d RIE=%d SIE=%d\n",
                    chip->name, chan, value,
                    !!(value & SCC_WR1_TX_IE),
                    !!(value & SCC_WR1_RX_IE_MASK),
                    !!(value & SCC_WR1_EXT_IE));
        break;

    case 2:
        /* WR2 is chip-wide. Either channel sees the same vector. */
        chip->interrupt_vector = value;
        chip->rr[0][2] = value;
        chip->rr[1][2] = value;
        break;

    case 9:
        /* WR9 handler does its own pointer reset on the reset-cmd
           path (via scc_chip_reset_world).  For the non-reset path
           we still need the auto-reset, which happens after the
           switch below. */
        chip_handle_wr9(chip, value);
        chip->register_ptr = 0;
        return;

    case 12: case 13: case 14:
        /* baud-rate generator -- just shadow */
        chip->rr[chan][reg] = value;
        break;

    case 15:
        /* WR15 bits 0,2 are always zero (per zsreg.h sanity check). */
        chip->rr[chan][reg] = value & ~0x05;
        break;

    default:
        break;
    }

    /* Z8530: pointer auto-resets to 0 after any non-WR0 write. */
    chip->register_ptr = 0;
    scc_chip_check_irq(chip);
}

static void chip_write_data(scc_chip_t *chip, int chan, uint8_t value)
{
    char ds = (value >= ' ' && value <= '~') ? (char)value : '.';
    if (trace_scc)
        printf("scc-%s%d: write data %02x %c\n", chip->name, chan, value, ds);
    if (scc_wr_trace_enabled)
        fprintf(stderr, "scc-wr: %s ch=%d 0x%02x %c\n",
                chip->name, chan, value, ds);

    /* Hand the byte to whoever is listening.  on_tx_byte is wired by
       sim68k.c io_init to scc_tcp_send_byte (serial chip) or the
       keyboard sink (kbd chip). */
    if (chip->on_tx_byte) chip->on_tx_byte(chip, chan, value);

    /* TX is "instant" -- the FIFO is empty by the time we return,
       which is exactly when a real Z8530 latches the TX-empty
       interrupt.  Per RetroCore WriteData() (line 707-714):
         if WR1.TIE -> set RR3 IP_x_TX, CheckIrq().
       MIE gating happens inside scc_chip_check_irq. */
    chip->rr[chan][1] |= SCC_RR1_ALL_SENT;
    chip->rr[chan][0] |= SCC_RR0_TX_READY;

    if (chip->wr[chan][1] & SCC_WR1_TX_IE) {
        if (chan == SCC_CH_A) chip->interrupt_pending |= SCC_RR3_IP_A_TX;
        else                  chip->interrupt_pending |= SCC_RR3_IP_B_TX;
        scc_chip_check_irq(chip);
    }
}

unsigned int scc_chip_read(scc_chip_t *chip, unsigned int offset)
{
    /* offset bit 2 = chan select (1 = chan A), bit 1 = data/ctl (1 = data). */
    int chan = (offset & 0x04) ? SCC_CH_A : SCC_CH_B;
    int is_data = (offset & 0x02) ? 1 : 0;
    unsigned int value = is_data ? chip_read_data(chip, chan)
                                 : chip_read_ctl (chip, chan);
    if (scc_rd_trace_enabled)
        fprintf(stderr, "scc-rd: %s off=%x ch=%d %s -> 0x%02x\n",
                chip->name, offset, chan,
                is_data ? "data" : "ctl ", value & 0xff);
    return value;
}

void scc_chip_write(scc_chip_t *chip, unsigned int offset, unsigned int value)
{
    int chan = (offset & 0x04) ? SCC_CH_A : SCC_CH_B;
    int is_data = (offset & 0x02) ? 1 : 0;
    if (is_data) chip_write_data(chip, chan, (uint8_t)value);
    else         chip_write_ctl (chip, chan, (uint8_t)value);
}

void scc_chip_in_push(scc_chip_t *chip, int ch, uint8_t byte)
{
    fifo_push(&chip->ififo[ch], byte);
    chip->rr[ch][0] |= SCC_RR0_RX_READY;

    /* RX-int enable: WR1 bits 3-4 select RX-int mode (any non-zero = enabled). */
    if (chip->wr[ch][1] & SCC_WR1_RX_IE_MASK) {
        if (ch == SCC_CH_A) chip->interrupt_pending |= SCC_RR3_IP_A_RX;
        else                chip->interrupt_pending |= SCC_RR3_IP_B_RX;
        scc_chip_check_irq(chip);
    }
}

void scc_chip_update(scc_chip_t *chip)
{
    int ch;
    for (ch = 0; ch < 2; ch++) {
        if (chip->ififo[ch].count > 0) {
            chip->rr[ch][0] |= SCC_RR0_RX_READY;
            if (chip->wr[ch][1] & SCC_WR1_RX_IE_MASK) {
                if (ch == SCC_CH_A) chip->interrupt_pending |= SCC_RR3_IP_A_RX;
                else                chip->interrupt_pending |= SCC_RR3_IP_B_RX;
            }
        } else {
            chip->rr[ch][0] &= ~SCC_RR0_RX_READY;
        }
    }
    scc_chip_check_irq(chip);
}

/* ============================================================== */
/* Legacy global-API shim                                          */
/* ============================================================== */
/*
 * The old interface treated all four channels as a flat array
 * (ch=0,1 = serial chip; ch=2,3 = kbd/mouse chip).  Wire that
 * mapping to two underlying scc_chip_t instances so existing call
 * sites in sim68k.c, sun2.c, scc_tcp.c, and test_scc.c keep working
 * unchanged while we migrate the routing.
 */

scc_chip_t g_scc_serial;   /* ttya/ttyb -- chan A=ttya=ch1, chan B=ttyb=ch0 */
scc_chip_t g_scc_kbd;      /* kbd/mouse -- chan A=kbd=ch3,  chan B=mouse=ch2 */

static void shim_irq_cb(scc_chip_t *chip, int asserted)
{
    (void)chip;
    if (asserted) int_controller_set(IRQ_SCC);
    else {
        /* Both chips share the IRQ_SCC line.  Only drop the line if
           NEITHER chip is asserting -- otherwise the other chip's
           IRQ would silently disappear. */
        if (!g_scc_serial.irq_asserted && !g_scc_kbd.irq_asserted)
            int_controller_clear(IRQ_SCC);
    }
}

static void shim_serial_tx(scc_chip_t *chip, int chan, uint8_t byte)
{
    (void)chip;
    /* Map chan -> legacy ch index for the TCP server. */
    int ch = (chan == SCC_CH_A) ? 1 : 0;
    /* SunOS's tty discipline computes parity in SOFTWARE for some
       output paths (cnputc + printf via uart) and OR's the parity
       bit into the data byte before writing it to the data port.
       Real hardware would still drive that bit out on the wire as
       part of the 8-bit cell, but human-readable terminal clients
       expect 7-bit ASCII.  Strip bit 7 on the TCP forwarder.  Side
       effect: this also strips actual MSBs of true 8-bit data --
       not a concern for the SunOS console use case (always ASCII)
       and easy to gate later if a binary protocol ever runs over
       the same channel. */
    scc_tcp_send_byte(ch, byte & 0x7f);
}

static void shim_kbd_tx(scc_chip_t *chip, int chan, uint8_t byte)
{
    (void)chip;
    /* Only chan A (= kbd in our mapping, legacy ch=3) drives the kbd cmd queue.
       chan B = mouse -- not modelled. */
    if (chan == SCC_CH_A) sun2_kb_write(byte, 1);
}

static int shim_inited = 0;
static void shim_init(void)
{
    if (shim_inited) return;
    scc_chip_init(&g_scc_serial, "serial");
    scc_chip_init(&g_scc_kbd,    "kbd");
    g_scc_serial.on_tx_byte = shim_serial_tx;
    g_scc_serial.on_irq     = shim_irq_cb;
    g_scc_kbd   .on_tx_byte = shim_kbd_tx;
    g_scc_kbd   .on_irq     = shim_irq_cb;
    shim_inited = 1;
}

/* Map legacy ch=[0..3] to (chip, chan).
   ch=0 = serial chan B = ttyb ; ch=1 = serial chan A = ttya
   ch=2 = kbd chan B    = mouse; ch=3 = kbd chan A    = kbd */
static scc_chip_t *shim_chip(int ch) {
    return (ch < 2) ? &g_scc_serial : &g_scc_kbd;
}
static int shim_chan(int ch) {
    return (ch & 1) ? SCC_CH_A : SCC_CH_B;
}
/* Inverse helper: given chip+chan, recover the legacy ch index. */
static int shim_legacy_ch(scc_chip_t *chip, int chan) {
    int base = (chip == &g_scc_serial) ? 0 : 2;
    return base + ((chan == SCC_CH_A) ? 1 : 0);
}

/* ----- legacy public functions ----- */

int scc_device_ack(int which)
{
    (void)which;
    shim_init();
    if (trace_scc) printf("scc: irq ack %d\n", which);
    /* Only ack chips that are actually asserting -- the line is shared. */
    int vec = M68K_INT_ACK_AUTOVECTOR;
    if (g_scc_serial.irq_asserted) vec = scc_chip_ack(&g_scc_serial);
    if (g_scc_kbd.irq_asserted)    vec = scc_chip_ack(&g_scc_kbd);
    return vec;
}

void scc_in_push(int ch, int v)
{
    shim_init();
    scc_chip_in_push(shim_chip(ch), shim_chan(ch), (uint8_t)v);
}

int scc_in_pop(int ch, unsigned int *pv)
{
    shim_init();
    scc_chip_t *chip = shim_chip(ch);
    int chan = shim_chan(ch);
    if (chip->ififo[chan].count <= 0) {
        *pv = chip->ififo[chan].last_byte;
        return 0;
    }
    uint8_t b;
    fifo_pop(&chip->ififo[chan], &b);
    *pv = b;
    return 1;
}

void scc_throw_interrupt(int ch, int which)
{
    shim_init();
    scc_chip_t *chip = shim_chip(ch);
    int chan = shim_chan(ch);
    uint8_t bit;

    if (which == 1) bit = (chan == SCC_CH_A) ? SCC_RR3_IP_A_TX : SCC_RR3_IP_B_TX;
    else            bit = (chan == SCC_CH_A) ? SCC_RR3_IP_A_RX : SCC_RR3_IP_B_RX;

    chip->interrupt_pending |= bit;
    scc_chip_check_irq(chip);

    if (trace_scc)
        printf("scc-%s ch=%d: throw int which=%d -> RR3=0x%02x\n",
               chip->name, chan, which, chip->interrupt_pending);
}

unsigned int scc_read(unsigned int pa, int size)
{
    shim_init();
    scc_chip_t *chip;
    /* The kbd/mouse chip lives in OBMEM 0x780000-0x7800FF;
       everything else routes to the serial chip. */
    if ((pa & 0x00ffff00) == 0x780000) chip = &g_scc_kbd;
    else                                chip = &g_scc_serial;
    unsigned int value = scc_chip_read(chip, pa & 0x0f);
    (void)size;
    return value;
}

void scc_write(unsigned int pa, unsigned int value, int size)
{
    shim_init();
    scc_chip_t *chip;
    if ((pa & 0x00ffff00) == 0x780000) chip = &g_scc_kbd;
    else                                chip = &g_scc_serial;
    scc_chip_write(chip, pa & 0x0f, value);
    (void)size;
}

void scc_update(void)
{
    shim_init();
    scc_chip_update(&g_scc_serial);
    scc_chip_update(&g_scc_kbd);
}

/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 4 */
/* End:              */
