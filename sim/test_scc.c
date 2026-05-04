/*
 * test_scc.c -- unit tests for the per-instance scc_chip_t API.
 *
 * Build:
 *   make test-scc
 *
 * Each TEST() block builds a local scc_chip_t and exercises it via
 * the public per-instance API (scc_chip_init / scc_chip_read /
 * scc_chip_write / scc_chip_in_push / scc_chip_ack / etc.).  No
 * global state is touched, so tests are independent of each other
 * and of the legacy shim used by the live emulator.
 *
 * Stubs at the bottom replace the parts of the larger emulator we
 * don't link in for a unit test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "scc.c"   /* pulls in implementation + the legacy shim */

/* ---- assertion helper ---------------------------------------------- */
static int g_failed = 0;
static int g_total  = 0;

#define CHECK(name, cond) do {                                        \
    g_total++;                                                        \
    if (!(cond)) {                                                    \
        fprintf(stderr, "  FAIL  %s:  %s\n", (name), #cond);          \
        g_failed++;                                                   \
    } else {                                                          \
        fprintf(stderr, "  pass  %s\n", (name));                      \
    }                                                                 \
} while (0)

/* IRQ-callback recorder so we can assert on line edges. */
static int        irq_state;
static int        irq_changes;
static scc_chip_t *irq_last_chip;

static void rec_irq(scc_chip_t *chip, int asserted)
{
    irq_state    = asserted;
    irq_changes++;
    irq_last_chip = chip;
}

/* TX-byte recorder. */
static uint8_t tx_buf[64];
static int     tx_count;
static int     tx_last_chan;

static void rec_tx(scc_chip_t *chip, int chan, uint8_t byte)
{
    (void)chip;
    if (tx_count < (int)sizeof(tx_buf)) tx_buf[tx_count++] = byte;
    tx_last_chan = chan;
}

static void reset_recorders(void)
{
    irq_state    = 0;
    irq_changes  = 0;
    irq_last_chip = NULL;
    tx_count     = 0;
    tx_last_chan = -1;
    memset(tx_buf, 0, sizeof(tx_buf));
}

/* Helper: WR0-then-data two-step on a chan. */
static void wr_reg(scc_chip_t *chip, int chan, int reg, uint8_t value)
{
    int cmd = (reg >= 8) ? 1 : 0;
    int low = reg & 0x7;
    unsigned int off = (chan == SCC_CH_A) ? 0x4 : 0x0;
    scc_chip_write(chip, off, (cmd << 3) | low);
    scc_chip_write(chip, off, value);
}

static unsigned int rd_reg(scc_chip_t *chip, int chan, int reg)
{
    int cmd = (reg >= 8) ? 1 : 0;
    int low = reg & 0x7;
    unsigned int off = (chan == SCC_CH_A) ? 0x4 : 0x0;
    scc_chip_write(chip, off, (cmd << 3) | low);
    return scc_chip_read(chip, off);
}

/* =================================================================== */
/* Test 1 -- chip initialization defaults                              */
/* =================================================================== */
static void test_init(void)
{
    fprintf(stderr, "[test 1] init defaults\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t1");

    CHECK("init: register_ptr = 0",        chip.register_ptr == 0);
    CHECK("init: MIE = 0",                  chip.master_int_enable == 0);
    CHECK("init: interrupt_pending = 0",    chip.interrupt_pending == 0);
    CHECK("init: irq_asserted = 0",         chip.irq_asserted == 0);
    CHECK("init: chan B RR0 has TX_READY",  (chip.rr[SCC_CH_B][0] & SCC_RR0_TX_READY) != 0);
    CHECK("init: chan A RR0 has TX_READY",  (chip.rr[SCC_CH_A][0] & SCC_RR0_TX_READY) != 0);
    CHECK("init: chan B RR1 has ALL_SENT",  (chip.rr[SCC_CH_B][1] & SCC_RR1_ALL_SENT) != 0);
}

/* =================================================================== */
/* Test 2 -- WR9 MIE gating: no MIE -> no IRQ even with IP bit set     */
/* =================================================================== */
static void test_mie_gates_irq(void)
{
    fprintf(stderr, "[test 2] WR9.MIE gates IRQ\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t2");
    chip.on_irq = rec_irq;
    reset_recorders();

    /* Force RR3 IP bit -- but MIE off.  No IRQ should fire. */
    chip.interrupt_pending = SCC_RR3_IP_A_TX;
    scc_chip_check_irq(&chip);
    CHECK("MIE off: no IRQ assert",        irq_state == 0);
    CHECK("MIE off: irq_asserted = 0",     chip.irq_asserted == 0);

    /* Now enable MIE via WR9 = 0x09 (MIE | VIS). */
    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_MIE | SCC_WR9_VIS);
    CHECK("WR9 MIE: master_int_enable = 1", chip.master_int_enable == 1);
    CHECK("WR9 MIE: vector_incl_stat = 1",  chip.vector_incl_stat == 1);
    CHECK("WR9 MIE: irq_asserted = 1",      chip.irq_asserted == 1);
    CHECK("WR9 MIE: irq_state = 1",         irq_state == 1);
}

/* =================================================================== */
/* Test 3 -- TX path raises RR3 IP_x_TX iff WR1.TIE                    */
/* =================================================================== */
static void test_tx_int(void)
{
    fprintf(stderr, "[test 3] TX writes raise IP_x_TX iff WR1.TIE\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t3");
    chip.on_irq     = rec_irq;
    chip.on_tx_byte = rec_tx;
    reset_recorders();

    /* Enable MIE chip-wide. */
    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_MIE);
    /* Enable TIE on chan A. */
    wr_reg(&chip, SCC_CH_A, 1, SCC_WR1_TX_IE);

    /* Write a data byte to chan A. */
    scc_chip_write(&chip, 0x6, '/');   /* offset 6 = data, chan A */
    CHECK("TX: byte forwarded to on_tx_byte",  tx_count == 1 && tx_buf[0] == '/');
    CHECK("TX: chan = A",                       tx_last_chan == SCC_CH_A);
    CHECK("TX: IP_A_TX set",                    (chip.interrupt_pending & SCC_RR3_IP_A_TX) != 0);
    CHECK("TX: IRQ asserted",                   chip.irq_asserted == 1);

    /* Disable TIE on chan A: another write must NOT raise IP. */
    chip.interrupt_pending = 0;
    chip.irq_asserted = 0;
    irq_state = 0;
    wr_reg(&chip, SCC_CH_A, 1, 0);
    scc_chip_write(&chip, 0x6, 'X');
    CHECK("TX no-TIE: byte still forwarded",   tx_buf[1] == 'X');
    CHECK("TX no-TIE: IP_A_TX NOT set",        (chip.interrupt_pending & SCC_RR3_IP_A_TX) == 0);
}

/* =================================================================== */
/* Test 4 -- WR0 RESET_TXINT (0x28) clears IP_x_TX, drops IRQ if last  */
/* =================================================================== */
static void test_wr0_reset_txint(void)
{
    fprintf(stderr, "[test 4] WR0 RESET_TXINT (0x28) clears IP_x_TX\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t4");
    chip.on_irq     = rec_irq;
    chip.on_tx_byte = rec_tx;
    reset_recorders();

    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_MIE);
    wr_reg(&chip, SCC_CH_A, 1, SCC_WR1_TX_IE);
    scc_chip_write(&chip, 0x6, 'a');         /* writes to data port chan A */
    CHECK("setup: IP_A_TX set",              (chip.interrupt_pending & SCC_RR3_IP_A_TX) != 0);
    CHECK("setup: IRQ asserted",             chip.irq_asserted == 1);

    /* Send WR0 = RESET_TXINT (cmd=5 -> 0x28) on chan A (offset 4). */
    scc_chip_write(&chip, 0x4, 0x28);
    CHECK("RESET_TXINT: IP_A_TX cleared",    (chip.interrupt_pending & SCC_RR3_IP_A_TX) == 0);
    CHECK("RESET_TXINT: IRQ dropped",        chip.irq_asserted == 0);
}

/* =================================================================== */
/* Test 5 -- RX path: in_push raises IP_x_RX iff WR1.RIE              */
/* =================================================================== */
static void test_rx_int(void)
{
    fprintf(stderr, "[test 5] RX in_push raises IP_x_RX iff WR1.RIE\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t5");
    chip.on_irq = rec_irq;
    reset_recorders();

    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_MIE);
    wr_reg(&chip, SCC_CH_A, 1, SCC_WR1_RX_IE);   /* RIE on chan A */

    scc_chip_in_push(&chip, SCC_CH_A, 0x42);
    CHECK("RX: IP_A_RX set",                 (chip.interrupt_pending & SCC_RR3_IP_A_RX) != 0);
    CHECK("RX: RR0[A].RX_READY set",         (chip.rr[SCC_CH_A][0] & SCC_RR0_RX_READY) != 0);
    CHECK("RX: IRQ asserted",                chip.irq_asserted == 1);

    /* Read the data port to drain the FIFO -- IP_RX must clear. */
    unsigned int v = scc_chip_read(&chip, 0x6);  /* data, chan A */
    CHECK("RX read: got the byte",           v == 0x42);
    CHECK("RX read: IP_A_RX cleared",        (chip.interrupt_pending & SCC_RR3_IP_A_RX) == 0);
    CHECK("RX read: IRQ dropped",            chip.irq_asserted == 0);
}

/* =================================================================== */
/* Test 6 -- RR2 chan B = modified vector, chan A = unmodified         */
/* =================================================================== */
static void test_rr2_modified_vector(void)
{
    fprintf(stderr, "[test 6] RR2 chan B is modified by IP, chan A is not\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t6");

    /* Set vector base to 0x40 via WR2 (any chan). */
    wr_reg(&chip, SCC_CH_A, 2, 0x40);

    /* Inject an A_TX pending. */
    chip.interrupt_pending = SCC_RR3_IP_A_TX;

    /* RR2 chan A = unmodified vector (= 0x40). */
    unsigned int va = rd_reg(&chip, SCC_CH_A, 2);
    CHECK("RR2 chan A unmodified",           va == 0x40);

    /* RR2 chan B = vector with code in bits 3:1 (status-low default).
       A_TX code = 100b -> shifted left 1 = 1000b = 0x08.
       Combined with vector base 0x40 (bits clear in 3:1) = 0x48. */
    unsigned int vb = rd_reg(&chip, SCC_CH_B, 2);
    CHECK("RR2 chan B = base | (A_TX<<1) = 0x48", vb == 0x48);

    /* With B_RX pending instead, code = 010 -> bits 3:1 = 0100 = 0x04. */
    chip.interrupt_pending = SCC_RR3_IP_B_RX;
    vb = rd_reg(&chip, SCC_CH_B, 2);
    CHECK("RR2 chan B = base | (B_RX<<1) = 0x44", vb == 0x44);

    /* No interrupt pending -> "special receive" sentinel = 011 -> 0x06.
       Combined with 0x40 = 0x46. */
    chip.interrupt_pending = 0;
    vb = rd_reg(&chip, SCC_CH_B, 2);
    CHECK("RR2 chan B = base | (NoInt<<1) = 0x46", vb == 0x46);
}

/* =================================================================== */
/* Test 7 -- RR3 only readable on chan A, chan B always 0              */
/* =================================================================== */
static void test_rr3_chan_only(void)
{
    fprintf(stderr, "[test 7] RR3 readable only on chan A\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t7");

    chip.interrupt_pending = SCC_RR3_IP_A_TX | SCC_RR3_IP_B_RX;

    unsigned int rA = rd_reg(&chip, SCC_CH_A, 3);
    unsigned int rB = rd_reg(&chip, SCC_CH_B, 3);
    CHECK("RR3 chan A returns IP mask",      rA == (SCC_RR3_IP_A_TX | SCC_RR3_IP_B_RX));
    CHECK("RR3 chan B returns 0",            rB == 0);
}

/* =================================================================== */
/* Test 8 -- WR9 reset commands                                        */
/* =================================================================== */
static void test_wr9_resets(void)
{
    fprintf(stderr, "[test 8] WR9 RESET_CHAN_A/B/WORLD\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t8");
    chip.on_irq = rec_irq;
    reset_recorders();

    /* Pre-load some state. */
    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_MIE);
    wr_reg(&chip, SCC_CH_A, 1, 0xff);
    wr_reg(&chip, SCC_CH_B, 1, 0xff);
    chip.interrupt_pending = SCC_RR3_IP_A_TX | SCC_RR3_IP_B_RX;
    scc_chip_check_irq(&chip);

    /* RESET_CHAN_A via WR9 = 0x80. */
    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_RESET_A);
    CHECK("RESET_A: WR1 chan A cleared",     chip.wr[SCC_CH_A][1] == 0);
    CHECK("RESET_A: WR1 chan B untouched",   chip.wr[SCC_CH_B][1] == 0xff);
    CHECK("RESET_A: chan-A IP cleared",      (chip.interrupt_pending & (SCC_RR3_IP_A_TX|SCC_RR3_IP_A_RX|SCC_RR3_IP_A_STAT)) == 0);
    CHECK("RESET_A: chan-B IP intact",       (chip.interrupt_pending & SCC_RR3_IP_B_RX) != 0);

    /* RESET_CHAN_B via WR9 = 0x40. */
    wr_reg(&chip, SCC_CH_B, 9, SCC_WR9_RESET_B);
    CHECK("RESET_B: WR1 chan B cleared",     chip.wr[SCC_CH_B][1] == 0);
    CHECK("RESET_B: chan-B IP cleared",      (chip.interrupt_pending & (SCC_RR3_IP_B_TX|SCC_RR3_IP_B_RX|SCC_RR3_IP_B_STAT)) == 0);

    /* RESET_WORLD via WR9 = 0xC0. */
    wr_reg(&chip, SCC_CH_A, 1, 0xaa);
    wr_reg(&chip, SCC_CH_B, 1, 0xbb);
    wr_reg(&chip, SCC_CH_A, 9, SCC_WR9_RESET_HW);
    CHECK("RESET_WORLD: WR1 chan A cleared", chip.wr[SCC_CH_A][1] == 0);
    CHECK("RESET_WORLD: WR1 chan B cleared", chip.wr[SCC_CH_B][1] == 0);
    CHECK("RESET_WORLD: MIE cleared",        chip.master_int_enable == 0);
    CHECK("RESET_WORLD: IP cleared",         chip.interrupt_pending == 0);
    CHECK("RESET_WORLD: IRQ dropped",        chip.irq_asserted == 0);
}

/* =================================================================== */
/* Test 9 -- IRQ ack drops the line; re-assertion is deferred           */
/*    Contract: ack itself never re-asserts inside the same call.       */
/*    Doing so creates an immediate high->low->high transition that    */
/*    the m68k can re-vector into before the current handler runs,     */
/*    leading to runaway nesting + stack overflow on real boots.       */
/*    The line goes back high on the next state-changing operation     */
/*    (chip_check_irq from a register write, in_push, or update).      */
/* =================================================================== */
static void test_ack_reasserts(void)
{
    fprintf(stderr, "[test 9] ack drops IRQ; re-assertion is deferred\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t9");
    chip.on_irq = rec_irq;
    reset_recorders();

    chip.master_int_enable = 1;
    chip.interrupt_pending = SCC_RR3_IP_A_TX | SCC_RR3_IP_A_RX;
    scc_chip_check_irq(&chip);
    CHECK("setup: IRQ asserted",             chip.irq_asserted == 1);
    CHECK("setup: irq_changes = 1",          irq_changes == 1);

    /* Ack -- drops the line; sources still pending but line stays low. */
    int vec = scc_chip_ack(&chip);
    CHECK("ack: returns autovector",         vec == M68K_INT_ACK_AUTOVECTOR);
    CHECK("ack: IRQ dropped",                chip.irq_asserted == 0);
    CHECK("ack: irq_changes = 2 (raise+drop)", irq_changes == 2);

    /* Sources still in interrupt_pending. */
    CHECK("ack leaves IP intact",
          chip.interrupt_pending == (SCC_RR3_IP_A_TX | SCC_RR3_IP_A_RX));

    /* Next state-changing op re-asserts.  Use scc_chip_update which
       any sim main-loop pump call would do. */
    scc_chip_update(&chip);
    CHECK("post-ack update re-asserts",      chip.irq_asserted == 1);

    /* Now clear remaining sources via WR0 RESET_TXINT + manual RX clear. */
    scc_chip_write(&chip, 0x4, 0x28);                /* RESET_TXINT chan A */
    chip.interrupt_pending &= ~SCC_RR3_IP_A_RX;
    scc_chip_check_irq(&chip);
    CHECK("after clear: IRQ dropped",        chip.irq_asserted == 0);
}

/* =================================================================== */
/* Test 10 -- legacy shim: scc_in_push(3, ...) goes to kbd chip A      */
/* =================================================================== */
static void test_legacy_shim(void)
{
    fprintf(stderr, "[test 10] legacy global API -> per-chip routing\n");
    /* shim_init resets g_scc_serial and g_scc_kbd. */
    g_scc_serial.init_done = 0;
    g_scc_kbd.init_done    = 0;
    shim_inited = 0;
    shim_init();

    scc_in_push(3, 0xab);   /* legacy ch=3 = kbd chan A */
    CHECK("legacy ch=3 -> g_scc_kbd chan A FIFO has 1 byte",
          g_scc_kbd.ififo[SCC_CH_A].count == 1);
    CHECK("legacy ch=3 byte = 0xab",
          g_scc_kbd.ififo[SCC_CH_A].buf[0] == 0xab);
    CHECK("legacy ch=3 does NOT touch g_scc_serial",
          g_scc_serial.ififo[SCC_CH_A].count == 0
       && g_scc_serial.ififo[SCC_CH_B].count == 0);

    scc_in_push(0, 0xcd);   /* legacy ch=0 = serial chan B = ttyb */
    CHECK("legacy ch=0 -> g_scc_serial chan B FIFO has 1 byte",
          g_scc_serial.ififo[SCC_CH_B].count == 1);
    CHECK("legacy ch=0 byte = 0xcd",
          g_scc_serial.ififo[SCC_CH_B].buf[0] == 0xcd);
}

/* =================================================================== */
/* Test 11 -- regression: per-chip MIE is independent                   */
/*    The original bug: a single global scc_int_pending bit blocked    */
/*    the serial chip from asserting while the kbd chip was active.    */
/* =================================================================== */
static void test_chip_independence(void)
{
    fprintf(stderr, "[test 11] two chips assert IRQ independently\n");
    scc_chip_t kbd, ser;
    scc_chip_init(&kbd, "kbd");
    scc_chip_init(&ser, "ser");
    kbd.on_irq = rec_irq;
    ser.on_irq = rec_irq;
    reset_recorders();

    kbd.master_int_enable = 1;
    ser.master_int_enable = 1;

    /* kbd chip raises an IRQ; serial then raises -- both should fire
       independently and each should track its own irq_asserted state. */
    kbd.interrupt_pending = SCC_RR3_IP_A_RX;
    scc_chip_check_irq(&kbd);
    CHECK("kbd asserts",                     kbd.irq_asserted == 1);

    ser.interrupt_pending = SCC_RR3_IP_A_TX;
    scc_chip_check_irq(&ser);
    CHECK("ser asserts independently",       ser.irq_asserted == 1);
    CHECK("kbd still asserted",              kbd.irq_asserted == 1);

    /* Drop kbd via ack + clear -- ser should remain asserted. */
    kbd.interrupt_pending = 0;
    scc_chip_check_irq(&kbd);
    CHECK("kbd dropped after clear",         kbd.irq_asserted == 0);
    CHECK("ser STILL asserted (independence)", ser.irq_asserted == 1);
}

/* =================================================================== */
/* Test 12 -- WR2 vector is chip-wide                                   */
/* =================================================================== */
static void test_wr2_chip_wide(void)
{
    fprintf(stderr, "[test 12] WR2 is chip-wide (write A reads B)\n");
    scc_chip_t chip;
    scc_chip_init(&chip, "t12");

    wr_reg(&chip, SCC_CH_A, 2, 0x77);
    CHECK("WR2 stored",                      chip.interrupt_vector == 0x77);

    /* Read RR2 from chan A = unmodified */
    unsigned int v = rd_reg(&chip, SCC_CH_A, 2);
    CHECK("RR2 chan A returns WR2",          v == 0x77);
}

int main(void)
{
    fprintf(stderr, "===== scc.c per-instance unit tests =====\n");
    test_init();
    test_mie_gates_irq();
    test_tx_int();
    test_wr0_reset_txint();
    test_rx_int();
    test_rr2_modified_vector();
    test_rr3_chan_only();
    test_wr9_resets();
    test_ack_reasserts();
    test_legacy_shim();
    test_chip_independence();
    test_wr2_chip_wide();
    fprintf(stderr, "===== %d/%d passed (%d failed) =====\n",
            g_total - g_failed, g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}

/* ============================================================
 * Stubs -- replace bits of the bigger sim we don't link in.
 * ============================================================ */

int quiet = 1;
int g_scc_boards = 0;   /* unit test: never use expansion boards */

void int_controller_set(unsigned int n)   { (void)n; }
void int_controller_clear(unsigned int n) { (void)n; }

void scc_tcp_send_byte(int ch, uint8_t b) { (void)ch; (void)b; }
void sun2_kb_write(int v, int sz)         { (void)v; (void)sz; }
