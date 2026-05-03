/*
 * test_scc.c -- unit tests for the 5 SCC bugs we fixed in scc.c.
 *
 * Build:
 *   make test-scc      (or:  cc -I. -I../m68k -fcommon -DM68K_V33 \
 *                              test_scc.c -o test_scc.exe )
 *
 * Each TEST() block exercises one bug-fix and asserts the post-state
 * of scc_rr / scc_wr / scc_ints / scc_int_pending matches the
 * canonical Z8530 spec from sun/sys/sundev/zsreg.h.
 *
 * Stubs at the bottom of this file replace the real CPU/IRQ/TCP glue
 * so the SCC module can run in isolation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Pull in the SCC implementation directly so we can poke the global
   state arrays.  We can't link against scc.o because we need the
   *test* version of int_controller_set / sun2_kb_write / etc. */
#include "scc.c"

/* ---- assertion helper ---- */
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

/* Reset all SCC global state between tests. */
static void test_reset_world(void)
{
    memset(scc_init,         0, sizeof(scc_init));
    memset(scc_cmd,          0, sizeof(scc_cmd));
    memset(scc_cmd_primed,   0, sizeof(scc_cmd_primed));
    memset(scc_wr,           0, sizeof(scc_wr));
    memset(scc_rr,           0, sizeof(scc_rr));
    memset(scc_ints,         0, sizeof(scc_ints));
    scc_int_pending = 0;
    memset(scc_ififo,        0, sizeof(scc_ififo));
    memset(scc_ofifo,        0, sizeof(scc_ofifo));
}

/* Helper: do the "select reg N, then write data byte" two-step on
   ctlA of channel ch.  Mirrors the Z8530 indirect-register protocol. */
static void wr_reg(int ch, int reg, int value)
{
    /* First write: register pointer (low 3 bits) + cmd group (bits 3-5).
       For regs 0-7 cmd=0; for regs 8-15 cmd=1 (point hi). */
    int cmd = (reg >= 8) ? 1 : 0;
    int low = reg & 0x7;
    scc_wr_ctl(ch, (cmd << 3) | low, 1);
    scc_wr_ctl(ch, value, 1);
}

/* =========================================================== */
/* Bug 1 -- WR13 must NOT clobber RR9                          */
/* =========================================================== */
static void test_bug1_wr13_no_rr9_clobber(void)
{
    fprintf(stderr, "[bug 1] WR13 must not corrupt RR9 mirror\n");
    test_reset_world();
    scc_chk_init(0);
    scc_rr[0][9] = 0xAA;            /* sentinel */
    wr_reg(0, 13, 0x55);            /* write WR13 = 0x55 */
    CHECK("RR13 mirrors WR13", scc_rr[0][13] == 0x55);
    CHECK("RR9 sentinel unchanged (bug 1 fixed)", scc_rr[0][9] == 0xAA);
}

/* =========================================================== */
/* Bug 2 -- WR15 must NOT clobber RR11                         */
/* =========================================================== */
static void test_bug2_wr15_no_rr11_clobber(void)
{
    fprintf(stderr, "[bug 2] WR15 must not corrupt RR11 mirror\n");
    test_reset_world();
    scc_chk_init(0);
    scc_rr[0][11] = 0xCC;           /* sentinel */
    wr_reg(0, 15, 0xFF);            /* WR15 with all-bits-set */
    CHECK("RR15 mirrors WR15 with bits 0,2 cleared",
          scc_rr[0][15] == (0xFF & ~5));
    CHECK("RR11 sentinel unchanged (bug 2 fixed)", scc_rr[0][11] == 0xCC);
}

/* =========================================================== */
/* Bug 3 -- RR2 modified vector must land in chan B of correct */
/*          chip pair, not unconditionally [2][2]              */
/* =========================================================== */
static void test_bug3_rr2_chip_pair(void)
{
    fprintf(stderr, "[bug 3] RR2 modified vector goes to chip-B index\n");

    /* ch=0/1 = chip 0 (chan B / chan A);  ch=2/3 = chip 1.       */

    /* Case A: throw RX int on ch=0 (chip 0, chan B).
       Modified vector should appear in chan B's RR2 = [0][2],
       NOT in [2][2]. */
    test_reset_world();
    scc_throw_interrupt(0, 2);
    CHECK("ch=0 RX int -> RR2[chip0 chan B] = 0x04",
          scc_rr[0][2] == 0x04);
    CHECK("ch=0 RX int does NOT touch RR2[chip1 chan B]",
          scc_rr[2][2] == 0x00);

    /* Case B: throw RX int on ch=1 (chip 0, chan A).
       Modified vector should still land in chan B's RR2 = [0][2]
       (chan A's RR2 = base; chan B's RR2 = modified). */
    test_reset_world();
    scc_throw_interrupt(1, 2);
    CHECK("ch=1 RX int -> RR2[chip0 chan B] = 0x0c",
          scc_rr[0][2] == 0x0c);
    CHECK("ch=1 RX int does NOT touch RR2[chip1 chan B]",
          scc_rr[2][2] == 0x00);

    /* Case C: throw RX int on ch=2 (chip 1, chan B).
       Should land in [2][2], NOT [0][2]. */
    test_reset_world();
    scc_throw_interrupt(2, 2);
    CHECK("ch=2 RX int -> RR2[chip1 chan B] = 0x04",
          scc_rr[2][2] == 0x04);
    CHECK("ch=2 RX int does NOT touch RR2[chip0 chan B]",
          scc_rr[0][2] == 0x00);

    /* Case D: ch=3 (chip 1, chan A) -- vector in chan B = [2][2] */
    test_reset_world();
    scc_throw_interrupt(3, 2);
    CHECK("ch=3 RX int -> RR2[chip1 chan B] = 0x0c",
          scc_rr[2][2] == 0x0c);
}

/* =========================================================== */
/* Bug 4 -- RR3 IP bits must be set in chan A of chip pair     */
/* =========================================================== */
static void test_bug4_rr3_ip_bits(void)
{
    fprintf(stderr, "[bug 4] RR3 IP bits set in chan A of chip pair\n");

    /* ch=0 (chip0 chan B) RX -> IP_B_RX (0x04) in chan A's RR3 = [1][3] */
    test_reset_world();
    scc_throw_interrupt(0, 2);
    CHECK("ch=0 RX -> RR3[chip0 chan A] has IP_B_RX=0x04",
          (scc_rr[1][3] & 0x04) != 0);

    /* ch=1 (chip0 chan A) TX -> IP_A_TX (0x10) in [1][3] */
    test_reset_world();
    scc_throw_interrupt(1, 1);
    CHECK("ch=1 TX -> RR3[chip0 chan A] has IP_A_TX=0x10",
          (scc_rr[1][3] & 0x10) != 0);

    /* ch=2 (chip1 chan B) TX -> IP_B_TX (0x02) in [3][3] */
    test_reset_world();
    scc_throw_interrupt(2, 1);
    CHECK("ch=2 TX -> RR3[chip1 chan A] has IP_B_TX=0x02",
          (scc_rr[3][3] & 0x02) != 0);

    /* ch=3 (chip1 chan A) RX -> IP_A_RX (0x20) in [3][3] */
    test_reset_world();
    scc_throw_interrupt(3, 2);
    CHECK("ch=3 RX -> RR3[chip1 chan A] has IP_A_RX=0x20",
          (scc_rr[3][3] & 0x20) != 0);

    /* Cross-pair isolation: ch=0 int must NOT touch [3][3]. */
    test_reset_world();
    scc_throw_interrupt(0, 2);
    CHECK("ch=0 RX leaves RR3[chip1 chan A] = 0",
          scc_rr[3][3] == 0);

    /* scc_device_ack clears RR3 of chan A in both chip pairs. */
    test_reset_world();
    scc_throw_interrupt(0, 2);
    scc_throw_interrupt(2, 2);   /* second one is masked by int_pending */
    scc_rr[3][3] = 0x04;          /* but pretend chip1 also has IP set */
    scc_device_ack(6);
    CHECK("scc_device_ack clears RR3[chip0 chan A]",
          scc_rr[1][3] == 0);
    CHECK("scc_device_ack clears RR3[chip1 chan A]",
          scc_rr[3][3] == 0);
}

/* =========================================================== */
/* Bug 5 -- WR9 reset commands honored                         */
/* =========================================================== */
static void test_bug5_wr9_reset(void)
{
    fprintf(stderr, "[bug 5] WR9 reset commands clear chip state\n");

    /* RESET_CHAN_B: write 0x40 to WR9 via ch=0 -> resets chip0 chan B (=ch 0). */
    test_reset_world();
    scc_chk_init(0);
    scc_chk_init(1);
    /* Pre-load some non-zero state on both channels of chip 0. */
    scc_wr[0][1]   = 0xFF;     scc_wr[1][1]   = 0xFF;
    scc_rr[0][3]   = 0xAB;     scc_rr[1][3]   = 0xAB;
    scc_ints[0]    = 0x82;     scc_ints[1]    = 0x82;
    scc_ififo[0].count = 5;    scc_ififo[1].count = 5;

    wr_reg(0, 9, 0x40);        /* WR9 = RESET_CHAN_B (chip 0 chan B = idx 0) */

    CHECK("RESET_CHAN_B clears WR1 of chan B (idx 0)", scc_wr[0][1] == 0);
    CHECK("RESET_CHAN_B clears RR3 of chan B (idx 0)", scc_rr[0][3] == 0);
    CHECK("RESET_CHAN_B clears scc_ints of chan B",     scc_ints[0] == 0);
    CHECK("RESET_CHAN_B clears input FIFO of chan B",   scc_ififo[0].count == 0);
    /* Sister channel A (idx 1) must NOT be touched. */
    CHECK("RESET_CHAN_B leaves chan A WR1 untouched",   scc_wr[1][1] == 0xFF);
    CHECK("RESET_CHAN_B leaves chan A RR3 untouched",   scc_rr[1][3] == 0xAB);
    /* RR0/RR1 of reset channel get the canonical defaults. */
    CHECK("RESET_CHAN_B sets RR0 = TX_READY",           scc_rr[0][0] == 0x04);
    CHECK("RESET_CHAN_B sets RR1 = ALL_SENT",           scc_rr[0][1] == 0x01);

    /* RESET_CHAN_A: write 0x80 via ch=1 -> resets chan A (idx 1). */
    test_reset_world();
    scc_chk_init(0); scc_chk_init(1);
    scc_wr[0][1] = 0xAA;  scc_wr[1][1] = 0xBB;
    wr_reg(1, 9, 0x80);
    CHECK("RESET_CHAN_A clears chan A WR1",             scc_wr[1][1] == 0);
    CHECK("RESET_CHAN_A leaves chan B WR1 untouched",   scc_wr[0][1] == 0xAA);

    /* RESET_WORLD: write 0xC0 -> resets BOTH channels of chip pair. */
    test_reset_world();
    scc_chk_init(0); scc_chk_init(1);
    scc_chk_init(2); scc_chk_init(3);
    scc_wr[0][1] = scc_wr[1][1] = 0xAA;
    scc_wr[2][1] = scc_wr[3][1] = 0xBB;
    wr_reg(0, 9, 0xC0);
    CHECK("RESET_WORLD clears chip0 chan B WR1",        scc_wr[0][1] == 0);
    CHECK("RESET_WORLD clears chip0 chan A WR1",        scc_wr[1][1] == 0);
    CHECK("RESET_WORLD on chip0 leaves chip1 chan B",   scc_wr[2][1] == 0xBB);
    CHECK("RESET_WORLD on chip0 leaves chip1 chan A",   scc_wr[3][1] == 0xBB);
}

int main(void)
{
    fprintf(stderr, "===== scc.c unit tests =====\n");
    test_bug1_wr13_no_rr9_clobber();
    test_bug2_wr15_no_rr11_clobber();
    test_bug3_rr2_chip_pair();
    test_bug4_rr3_ip_bits();
    test_bug5_wr9_reset();
    fprintf(stderr, "===== %d/%d passed (%d failed) =====\n",
            g_total - g_failed, g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}

/* ============================================================
 * Stubs for the parts of the emulator scc.c references that we
 * don't want pulled in for a unit test.
 * ============================================================ */

int quiet = 1;                  /* silence !quiet printfs */

void int_controller_set(unsigned int n)   { (void)n; }
void int_controller_clear(unsigned int n) { (void)n; }

void scc_tcp_send_byte(int ch, uint8_t b) { (void)ch; (void)b; }
void sun2_kb_write(int v, int sz)         { (void)v; (void)sz; }
