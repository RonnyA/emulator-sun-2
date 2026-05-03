/*
 * sun-2 emulator
 * 10/2014  Brad Parker <brad@heeltoe.com>
 *
 * SCC serial chip emulation
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "sim68k.h"
#include "m68k.h"
#include "sim.h"
#include "scc_tcp.h"

extern int quiet;
int trace_scc = 0;

/* Trace flags resolved once at module init (scc_init_traces) from
   the SCC_*_TRACE environment variables, then read cheaply on every
   register access.  No getenv() in hot paths. */
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

unsigned int scc_init[4];
unsigned int scc_cmd[4];
unsigned int scc_cmd_primed[4];
unsigned int scc_wr[4][16];
unsigned int scc_rr[4][16];
unsigned int scc_ints[4];
unsigned int scc_int_pending;

struct scc_fifo_s {
  int count;
  int wptr;
  int rptr;
  unsigned char fifo[16];
  int ints_enabled;
};

struct scc_fifo_s scc_ififo[4];
struct scc_fifo_s scc_ofifo[4];


#define RR0_RX_READY	0x01
#define RR0_TX_READY	0x04

#define RR1_ALL_SENT	0x01

#define WR1_EXT_INT_EN 0x01
#define WR1_TX_INT_EN  0x02
#define WR1_RX_INT_EN0 0x08
#define WR1_RX_INT_EN1 0x10

#define WR1_RX_INT_DIS 0
#define WR1_RX_INT_FC  1
#define WR1_RX_INT_ALL 2
#define WR1_RX_INT_SC  3

#define WR9_VIS   0x01
#define WR9_NV    0x02
#define WR9_DLC   0x04
#define WR9_MIE   0x08
#define WR9_ST_HI 0x10
#define WR9_RESET_MASK   0xC0
#define WR9_RESET_CHAN_B 0x40
#define WR9_RESET_CHAN_A 0x80
#define WR9_RESET_WORLD  0xC0

#define RR3_IP_B_STAT 0x01
#define RR3_IP_B_TX   0x02
#define RR3_IP_B_RX   0x04
#define RR3_IP_A_STAT 0x08
#define RR3_IP_A_TX   0x10
#define RR3_IP_A_RX   0x20

static void scc_reset_channel(int ch)
{
  int i;
  for (i = 1; i < 16; i++) {
    scc_wr[ch][i] = 0;
    scc_rr[ch][i] = 0;
  }
  scc_rr[ch][0] = RR0_TX_READY;
  scc_rr[ch][1] = RR1_ALL_SENT;
  scc_ints[ch] = 0;
  scc_ififo[ch].count = 0;
  scc_ififo[ch].wptr = 0;
  scc_ififo[ch].rptr = 0;
  scc_ofifo[ch].count = 0;
  scc_ofifo[ch].wptr = 0;
  scc_ofifo[ch].rptr = 0;
}


int scc_device_ack(int which)
{
  if (trace_scc) printf("scc: irq ack %d\n", which);
  int_controller_clear(IRQ_SCC);
  scc_int_pending = 0;
  /* Clear modified vector in chan B of each chip pair (RR2 chan B holds
     the live modified vector; chan A returns the WR2 base). */
  scc_rr[0][2] = scc_rr[2][2] = 0;
  /* Clear interrupt-pending bitmask in chan A of each chip pair (RR3 is
     only readable on chan A per zsreg.h). */
  scc_rr[1][3] = scc_rr[3][3] = 0;
  return M68K_INT_ACK_AUTOVECTOR;
}

void scc_throw_interrupt(int ch, int which)
{
  int st_hi = scc_wr[ch][9] & WR9_ST_HI;
  int chan_b = ch & ~1;       /* chip B-channel index (RR2 modified vector) */
  int chan_a = ch | 1;        /* chip A-channel index (RR3 IP bitmask)       */
  int is_chan_a = (ch & 1);

  /* IRQ is level-triggered on the Z8530: while any IP bit in chan A's
     RR3 is set, the chip keeps asserting the interrupt line.  The
     kernel acks individual conditions with WR0 = RESET_xxxINT and ends
     the IUS chain with WR0 = CLR_INTR.  We must therefore allow
     multiple throws to update RR2/RR3 -- the previous "if pending,
     return" early-out swallowed every byte after the first in a TX
     burst, hanging SunOS's interrupt-driven tty after one char. */
  scc_int_pending = 1;
  if (trace_scc) printf("scc%d: throw interrupt %d\n", ch, which);

  /* RR2 modified-vector encoding (per zsreg.h, ZSIR_xx_x):
       chan B + TX empty = 0x00 (low) / 0x00 (hi)
       chan B + RX char  = 0x04 (low) / 0x20 (hi)
       chan A + TX empty = 0x08 (low) / 0x10 (hi)
       chan A + RX char  = 0x0c (low) / 0x30 (hi)
     The modified vector lives in *chan B's* RR2 of each chip. */
  if (is_chan_a) {
    switch (which) {
    case 1: scc_rr[chan_b][2] = st_hi ? 0x10 : 0x08; break;
    case 2: scc_rr[chan_b][2] = st_hi ? 0x30 : 0x0c; break;
    }
  } else {
    switch (which) {
    case 1: scc_rr[chan_b][2] = st_hi ? 0x00 : 0x00; break;
    case 2: scc_rr[chan_b][2] = st_hi ? 0x20 : 0x04; break;
    }
  }

  /* RR3 interrupt-pending bitmask lives in chan A of each chip pair.
     SunOS zslevel6intr (zs_common.c:244-288) polls chan A's RR3 to
     find which chip is interrupting. */
  if (is_chan_a) {
    switch (which) {
    case 1: scc_rr[chan_a][3] |= RR3_IP_A_TX; break;
    case 2: scc_rr[chan_a][3] |= RR3_IP_A_RX; break;
    }
  } else {
    switch (which) {
    case 1: scc_rr[chan_a][3] |= RR3_IP_B_TX; break;
    case 2: scc_rr[chan_a][3] |= RR3_IP_B_RX; break;
    }
  }

  int_controller_set(IRQ_SCC);
}

void scc_in_push(int ch, int v)
{
  scc_ififo[ch].count++;
  scc_ififo[ch].fifo[ scc_ififo[ch].wptr++ ] = v;
  scc_ififo[ch].wptr &= 0xf;
  /* Z8530 RR0 bit 0 = "Rx Character Available".  Without this, the
     Sun-2 PROM busy-loops on RR0 waiting for a typed character that
     it never thinks has arrived (even though our FIFO has the byte). */
  scc_rr[ch][0] |= 0x01;
}

int scc_in_pop(int ch, unsigned int *pv)
{
  if (scc_ififo[ch].count <= 0) {
    *pv = -1;
    /* FIFO empty — clear Rx Character Available. */
    scc_rr[ch][0] &= ~0x01;
    return 0;
  }

  scc_ififo[ch].count--;
  *pv = scc_ififo[ch].fifo[ scc_ififo[ch].rptr++ ];
  scc_ififo[ch].rptr &= 0xf;
  /* If FIFO is now empty, clear RX_READY so the next status read
     reflects "no more chars". */
  if (scc_ififo[ch].count <= 0)
    scc_rr[ch][0] &= ~0x01;
  return 1;
}

void scc_chk_init(int ch)
{
  if (!scc_init[ch]) {
    scc_init[ch] = 1;
    scc_cmd[ch] = 0;
    scc_cmd_primed[ch] = 0;
    scc_rr[ch][0] = RR0_TX_READY;
    scc_rr[ch][1] = RR1_ALL_SENT;
  }
}

/* ------------- */

unsigned int scc_rd_ctl(int ch, int size)
{
  unsigned int value = 0xffff;
  int reg, cmd;

  scc_chk_init(ch);

  reg = scc_cmd[ch] & 0x7;
  cmd = (scc_cmd[ch] & 0x38) >> 3;
  if (cmd == 1)
    reg += 8;

  value = scc_rr[ch][reg];
  scc_cmd_primed[ch] = 0;
  scc_cmd[ch] = 0;

  if (trace_scc) {
    if (reg == 0 && value == 4)
      ;
   else
      printf("scc%d: read ctl; read %d -> %x (%d)\n", ch, reg, value, size);
  }
  return value;
}

unsigned int scc_rd_data(int ch, int size)
{
  unsigned int value = 0xffff;
  scc_in_pop(ch, &value);
  if (trace_scc) printf("scc%d: read data %x (%d)\n", ch, value, size);
  return value;
}

void scc_wr_ctl(int ch, int value, int size)
{
  scc_chk_init(ch);
  if (!scc_cmd_primed[ch]) {
    int wr0_cmd  = (value >> 3) & 0x07;
    int chan_a   = ch | 1;             /* chan A of this chip pair */

    scc_cmd[ch] = value;
    scc_cmd_primed[ch] = 1;
    if (trace_scc) printf("scc%d: write ctl %x (%d)\n", ch, value, size);

    /* WR0 command codes (per zsreg.h ZSWR0_* + Z8530 spec).  Codes
       > 1 are one-shot commands -- no second write follows.  Codes
       0 (NULL) and 1 (POINT_HI) just set the register pointer and
       expect a data byte next. */
    if (wr0_cmd > 1)
      scc_cmd_primed[ch] = 0;

    /* Execute the command side-effects.  Without this, SunOS's
       interrupt ACK writes (RESET_TXINT, RESET_HIGHEST_IUS) leave
       IP bits set in RR3 and the same interrupt re-fires forever. */
    switch (wr0_cmd) {
    case 2:  /* RESET_STATUS_INT (0x10) -- clear IP_x_STAT in chan A RR3 */
      if (ch & 1)  scc_rr[chan_a][3] &= ~RR3_IP_A_STAT;
      else         scc_rr[chan_a][3] &= ~RR3_IP_B_STAT;
      break;
    case 5:  /* RESET_TXINT (0x28) -- clear IP_x_TX in chan A RR3 */
      if (ch & 1)  scc_rr[chan_a][3] &= ~RR3_IP_A_TX;
      else         scc_rr[chan_a][3] &= ~RR3_IP_B_TX;
      break;
    case 6:  /* RESET_ERRORS (0x30) -- clear RR1 PE/DO/FE bits */
      scc_rr[ch][1] &= ~0xF0;
      break;
    case 7:  /* RESET_HIGHEST_IUS / CLR_INTR (0x38) -- end-of-int ack */
      scc_int_pending = 0;
      break;
    }
  } else {
    int reg = scc_cmd[ch] & 0x7;
    int cmd = (scc_cmd[ch] & 0x38) >> 3;
    int rst = (scc_cmd[ch] & 0xc0) >> 6;

    scc_cmd_primed[ch] = 0;
    scc_cmd[ch] = 0;

    if (cmd == 1) {
      /* point hi */
      reg += 8;
    }

    scc_wr[ch][reg] = value;
    if (trace_scc) printf("scc%d: write ctl; reg %d <- %x (%d)\n", ch, reg, value, size);

    switch (reg) {
    case 0:
      switch (value & 0xff) {
      case 0x10:
	break;
      case 0x38:
	break;
      }
      break;
    case 1:
      if (trace_scc) printf("scc%d: wr1 <- %02x (%d)\n", ch, value, size);
      if (scc_int_trace_enabled)
        fprintf(stderr, "scc-int: WR1 ch=%d <- 0x%02x\n", ch, value);
      break;
    case 2:
      scc_rr[ch][reg] = value;
      break;
    case 9:
      if (!quiet) printf("scc%d: wr9 <- %02x (%d)\n", ch, value, size);
      if (scc_int_trace_enabled)
        fprintf(stderr, "scc-int: WR9 ch=%d <- 0x%02x  ints[0..3]=%02x %02x %02x %02x\n",
                ch, value, scc_ints[0], scc_ints[1], scc_ints[2], scc_ints[3]);

      /* WR9 reset commands (bits 6-7) per zsreg.h:
           0x40 = reset chan B; 0x80 = reset chan A; 0xC0 = reset world.
         The WR9 register is shared between channels of a chip pair, so
         a write through any channel reaches the same chip's reset. */
      switch (value & WR9_RESET_MASK) {
      case WR9_RESET_WORLD:
        if (!quiet) printf("scc%d: WR9 RESET_WORLD\n", ch);
        scc_reset_channel(ch & ~1);
        scc_reset_channel(ch | 1);
        break;
      case WR9_RESET_CHAN_A:
        if (!quiet) printf("scc%d: WR9 RESET_CHAN_A\n", ch);
        scc_reset_channel(ch | 1);
        break;
      case WR9_RESET_CHAN_B:
        if (!quiet) printf("scc%d: WR9 RESET_CHAN_B\n", ch);
        scc_reset_channel(ch & ~1);
        break;
      }

      /* WR9 is chip-wide on a real Z8530: a write through either
         channel of a chip pair updates the same physical register.
         So MIE must be mirrored to both scc_ints[chan B] AND
         scc_ints[chan A] of this chip pair, otherwise SunOS's
         "WR9 = MIE+VIS via chan B" leaves chan A without MIE and
         our TX-empty firing in scc_wr_data is silently gated off. */
      {
        int chan_b = ch & ~1;
        int chan_a = ch | 1;
        if (scc_wr[ch][9] & WR9_MIE) {
          if (((scc_ints[chan_b] | scc_ints[chan_a]) & 0x80) == 0)
            if (!quiet) printf("scc%d: MIE enabled\n", ch);
          scc_ints[chan_b] |= 0x80;
          scc_ints[chan_a] |= 0x80;
        } else {
          if ((scc_ints[chan_b] | scc_ints[chan_a]) & 0x80)
            if (!quiet) printf("scc%d: MIE disabled\n", ch);
          scc_ints[chan_b] &= ~0x80;
          scc_ints[chan_a] &= ~0x80;
        }
      }
      break;
    case 12:
      scc_rr[ch][reg] = value;
      break;
    case 13:
      scc_rr[ch][reg] = value;
      break;
    case 14:
      scc_rr[ch][reg] = value;
      break;
    case 15:
      scc_rr[ch][reg] = value & ~5;
      break;
    }

    if (scc_wr[ch][1] & WR1_TX_INT_EN) {
      if ((scc_ints[ch] & 0x01) == 0) {
	  if (!quiet) printf("scc%d: TX int enabled\n", ch);
	  scc_ints[ch] |= 0x01;
      }
    } else {
      if (scc_ints[ch] & 0x01) {
	if (!quiet) printf("scc%d: TX int disabled\n", ch);
	scc_ints[ch] &= ~0x01;
      }
    }

    if (scc_wr[ch][1] & (WR1_RX_INT_EN0|WR1_RX_INT_EN1)) {
      if ((scc_ints[ch] & 0x02) == 0) {
	  if (!quiet) printf("scc%d: RX int enabled\n", ch);
	  scc_ints[ch] |= 0x02;
      }
    } else {
      if (scc_ints[ch] & 0x02) {
	if (!quiet) printf("scc%d: RX int disabled\n", ch);
	scc_ints[ch] &= ~0x02;
      }
    }

  }

}

void scc_wr_data(int ch, int value, int size)
{
  char dc, ds;
  int r;

  value &= 0xff;
  dc = value;
  ds = (value >= ' ' && value <= '~') ? value : '.';
  if (trace_scc) printf("scc%d: write data %02x %c\n", ch, value, ds);

  /* SCC_WR_TRACE=1 in env -- probed once at startup. */
  if (scc_wr_trace_enabled)
    fprintf(stderr, "scc-wr: ch=%d 0x%02x %c\n", ch, value, ds);

    switch (ch) {
    case 0:
    case 1:
      /* Forward TTY console writes to the optional --scc-tcp client. */
      scc_tcp_send_byte(ch, (uint8_t)value);
      break;

    case 3:
      sun2_kb_write(value, size);
      break;
    }

  /* Fire TX-empty interrupt if MIE + TIE are both enabled.  Our TX is
     "instant" (the byte is already gone), so the FIFO is always empty
     immediately after a write -- which is exactly when the chip would
     latch the TX-empty interrupt on real hardware.  Without this,
     SunOS's interrupt-driven tty path stalls after the first byte
     because zsa_txint never fires to write the next byte. */
  if (scc_int_trace_enabled)
    fprintf(stderr, "scc-int: wr_data ch=%d ints=0x%02x %s%s\n",
            ch, scc_ints[ch],
            (scc_ints[ch] & 0x80) ? "[MIE]" : "[!MIE]",
            (scc_ints[ch] & 0x01) ? "[TIE]" : "[!TIE]");
  /* WORKAROUND: SunOS 3.2 GENERIC for Sun-2 writes WR9 = 0x02 (NV only,
     no MIE) on the ttya/ttyb chip even after enabling TIE/RIE in WR1.
     Whether MIE is set elsewhere or whether Sun-2 SunOS uses a polled
     fallback that still wants the interrupt-pending bits set is
     unclear from the GENERIC binary.  Empirically, gating on MIE
     stalls userland output after the first byte ("/").  Gate on TIE
     alone -- if SunOS doesn't want the interrupt, it doesn't enable
     TIE.  Spec-strict behaviour can be restored by re-adding the
     "(scc_ints[ch] & 0x80) &&" prefix below. */
  if (scc_ints[ch] & 0x01) {
    scc_throw_interrupt(ch, 1);
  }
}

unsigned int scc_read(unsigned int pa, int size)
{
  if (scc_rd_trace_enabled)
    fprintf(stderr, "scc-rd: pa=%06x sz=%d", pa, size);
  unsigned int offset = pa & 0x0f;
  unsigned int value;
  int w;

  w = 0;
  if ((pa & 0x00ffff00) == 0x780000)
    w = 2;

  switch (offset) {
  case 0:
    value = scc_rd_ctl(w+0, size);
    break;
  case 2:
    value = scc_rd_data(w+0, size);
    break;
  case 4:
    value = scc_rd_ctl(w+1, size);
    break;
  case 6:
    value = scc_rd_data(w+1, size);
    break;
  default:
    value = 0;
    break;
  }
  if (scc_rd_trace_enabled)
    fprintf(stderr, " -> ch=%d %s -> 0x%02x\n",
            (offset >= 4) ? w+1 : w+0,
            (offset & 2) ? "data" : "ctl ",
            value & 0xff);

  if (0) printf("scc: read pa=%x -> %x (%d)\n", pa, value, size);

  return value;
}

void scc_write(unsigned int pa, unsigned int value, int size)
{
  unsigned int offset = pa & 0xf;
  int w;

  if (0) printf("scc: write pa=%x <- %x (%d)\n", pa, value, size);

  w = 0;
  if ((pa & 0x00ffff00) == 0x780000)
    w = 2;

  switch (offset) {
  case 0:
    scc_wr_ctl(w+0, value, size);
    break;
  case 2:
    scc_wr_data(w+0, value, size);
    break;
  case 4:
    scc_wr_ctl(w+1, value, size);
    break;
  case 6:
    scc_wr_data(w+1, value, size);
    break;
  }
}

void scc_update(void)
{
  int i;

//broken - incomplete probe?
scc_ints[2] |= 0x80;
scc_ints[3] |= 0x80;

  for (i = 0; i < 4; i++) {
    if (scc_ififo[i].count > 0) {
      scc_rr[i][0] |= RR0_RX_READY;

      if ((scc_ints[i] & 0x80) && (scc_ints[i] & 0x02)) {
	scc_throw_interrupt(i, 2);
      }
    } else {
      scc_rr[i][0] &= ~RR0_RX_READY;
    }
  }

#if 0
  for (i = 0; i < 4; i++) {
    if (scc_ofifo[i].count == 0) {
      scc_rr[i][0] |= RR0_TX_READY;

      if ((scc_ints[i] & 0x80) && (scc_ints[i] & 0x01)) {
	scc_throw_interrupt(i, 1);
      }
    } else {
      scc_rr[i][0] &= ~RR0_TX_READY;
    }
  }
#endif
}

/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
