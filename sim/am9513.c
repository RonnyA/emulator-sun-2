/*
 * sun-2 emulator
 * 10/2014  Brad Parker <brad@heeltoe.com>
 *
 * am9513 timer emulation
 */

#include <stdio.h>
#include <stdint.h>

#include "sim68k.h"
#include "m68k.h"
#include "sim.h"

int trace_am9513;
extern int trace_irq;

struct am9513_ctr_s {
  unsigned short mode;
  unsigned short load;
  unsigned short hold;
  unsigned short cntr;
};

/* Datasheet: after Master Reset every Counter Mode register holds 0x0B00.
   We zero index 0 (unused — counters are addressed 1..5). */
struct am9513_ctr_s am9513_ctr[6] = {
  { 0,      0, 0, 0 },
  { 0x0b00, 0, 0, 0 },
  { 0x0b00, 0, 0, 0 },
  { 0x0b00, 0, 0, 0 },
  { 0x0b00, 0, 0, 0 },
  { 0x0b00, 0, 0, 0 },
};

unsigned int am9513_status = 0x0b00;
unsigned int am9513_data_ptr;
unsigned int am9513_data_ptr_byte;
unsigned int am9513_irq_t1;
unsigned int am9513_irq_t2;
unsigned int am9513_prescale;
unsigned int am9513_output_bits;
unsigned int am9513_armed_bits;

int am9513_device_ack(int which)
{
  if (trace_am9513) printf("am9513: irq ack %d\n", which);
  switch (which) {
  case 1:
    int_controller_clear(IRQ_9513_TIMER1);
    /* Clear the OUT1 latch so the next counter underflow can re-fire IRQ.
       Without this the counter terminates only once and the system never
       sees periodic timer ticks (rev-1.0F PROM relies on this). */
    am9513_output_bits &= ~(1 << 1);
    return M68K_INT_ACK_AUTOVECTOR;
  case 2:
    int_controller_clear(IRQ_9513_TIMER2);
    am9513_output_bits &= ~(1 << 2);
    return M68K_INT_ACK_AUTOVECTOR;
  }
  return -1;
}

void am9513_update(void)
{
  int i, bit;

#if 0
  if (++am9513_prescale < 10)
    return;
  am9513_prescale = 0;
#endif

  //xxx count must be armed before it can commence counting

  for (i = 1; i < 6; i++) {
    bit = 1 << i;

    if ((am9513_armed_bits & bit) == 0)
      continue;

    if (am9513_ctr[i].mode) {

      if (am9513_ctr[i].mode & 1)
	am9513_ctr[i].cntr++;
      else
	am9513_ctr[i].cntr--;

      if (0) printf("am9513: ctr[%d] cntr=%x\n", i, am9513_ctr[i].cntr);

      if (am9513_ctr[i].cntr == 0) {
	if (trace_am9513) printf("am9513: ctr[%d] cntr==0 output_bits %x\n", i, am9513_output_bits);

	if ((am9513_ctr[i].mode & 0x40) == 0)
	  am9513_ctr[i].cntr = am9513_ctr[i].load;

	if ((am9513_output_bits & (1<<1)) == 0 && (bit & (1<<1))) {
	  if (trace_am9513) printf("am9513: ctr[%d] cntr==0 IRQ!\n", i);
	  am9513_irq_t1 = 1;
#if 0
	  enable_trace(2);
#endif
	}

	if ((am9513_output_bits & (1<<2)) == 0 && (bit & (1<<2))) {
	  if (trace_am9513) printf("am9513: ctr[%d] cntr==0 IRQ!\n", i);
	  am9513_irq_t2 = 1;
	}

	am9513_output_bits |= bit;
      }
    }
  }

  if (am9513_irq_t1) {
    am9513_irq_t1 = 0;
    if (0) printf("am9513: irq t1\n");
    int_controller_set(IRQ_9513_TIMER1);
  }

  if (am9513_irq_t2) {
    am9513_irq_t2 = 0;
    if (trace_irq) printf("am9513: irq t2\n");
    int_controller_set(IRQ_9513_TIMER2);
  }
}

void am9513_wr_data(unsigned int value)
{
  int e = (am9513_data_ptr & 0x18)>>3;
  int g = (am9513_data_ptr & 0x7);

  if (trace_am9513) printf("am9513_wr_data: e%d g%d b%d <- %02x\n", e, g, am9513_data_ptr_byte, value);
  switch (g) {
  case 1:
  case 2:
  case 3:
  case 4:
  case 5:
    value &= 0xff;
    if (am9513_data_ptr_byte) {
      switch (e) {
      case 0: am9513_ctr[g].mode = (value << 8) | (am9513_ctr[g].mode & 0xff); break;
      case 1: am9513_ctr[g].load = (value << 8) | (am9513_ctr[g].load & 0xff); break;
      case 2: am9513_ctr[g].hold = (value << 8) | (am9513_ctr[g].hold & 0xff); break;
      case 3: ;
	break;
      case 7:
	break;
      }
    } else {
      switch (e) {
      case 0: am9513_ctr[g].mode = value | (am9513_ctr[g].mode & 0xff00); break;
      case 1: am9513_ctr[g].load = value | (am9513_ctr[g].load & 0xff00); break;
      case 2: am9513_ctr[g].hold = value | (am9513_ctr[g].hold & 0xff00); break;
      case 3: ;
	break;
      case 7:
	break;
      }
    }
  }

  if (trace_am9513) {
    switch (e) {
    case 0: printf("am9513: mode[%d] = %04x\n", g, am9513_ctr[g].mode); break;
    case 1: printf("am9513: load[%d] = %04x\n", g, am9513_ctr[g].load); break;
    case 2: printf("am9513: hold[%d] = %04x\n", g, am9513_ctr[g].hold); break;
    }
  }

  if (am9513_data_ptr_byte) {
    am9513_data_ptr_byte = 0;
  }
}

void am9513_wr_cmd(unsigned int value)
{
  unsigned int n, mask, bits;
  int i;

  //if ((value & 0xff) != 0xe1) printf("am9513_wr_cmd; cmd %x ", value);
  switch (value & 0xff) {
  case 0xff:
    /* Master Reset: per datasheet, disarms all counters, zeros Master
       Mode / Load / Hold registers and sets every Counter Mode register
       to 0x0B00.  Also clear the IRQ-pending flags so a stray edge
       isn't latched after reset. */
    if (trace_am9513) printf("am9513: reset\n");
    {
      int j;
      for (j = 1; j < 6; j++) {
        am9513_ctr[j].mode = 0x0b00;
        am9513_ctr[j].load = 0;
        am9513_ctr[j].hold = 0;
        am9513_ctr[j].cntr = 0;
      }
      am9513_armed_bits = 0;
      am9513_output_bits = 0;
      am9513_irq_t1 = 0;
      am9513_irq_t2 = 0;
      am9513_data_ptr = 0;
      am9513_data_ptr_byte = 0;
    }
    return;
  case 0xe7: if (trace_am9513) printf("am9513: clr mm13\n"); return;
  case 0xe6: if (trace_am9513) printf("am9513: clr mm12\n"); return;
  case 0xe0: if (trace_am9513) printf("am9513: clr mm14\n"); return;
  case 0xef: if (trace_am9513) printf("am9513: set mm13\n"); return;
  case 0xee: if (trace_am9513) printf("am9513: set mm12\n"); return;
  case 0xe8: if (trace_am9513) printf("am9513: set mm14\n"); return;
  }

  switch (value & 0xf8) {
  case 0xf0:
    printf("am9513: step counter\n"); return;

  case 0xe0:
    n = value & 0x7;
    mask = ~(1 << n);
    am9513_output_bits &= mask;
    if (trace_am9513) printf("am9513: clr output bit %d %x\n", n, am9513_output_bits);
//    am9513_ctr[n].cntr = 0;
    return;

  case 0xe8:
    printf("am9513: set output bit\n");
    return;
  }

  switch (value & 0xe0) {
  case 0xc0:
    if (trace_am9513) printf("am9513: disarm\n");
    bits = value & 0x1f;
    am9513_armed_bits &= ~(bits << 1);
    return;
  case 0xa0:
    if (trace_am9513) printf("am9513: save hold\n");

    /* SAVE: copy counter -> HOLD (datasheet).  S5..S1 select counters in
       value bits 4..0; previous code used '&&' (logical) and wrong shift,
       so SAVE always clobbered all five counters' LOAD registers. */
    for (i = 1; i < 6; i++) {
      if (value & (1 << (i - 1)))
	am9513_ctr[i].hold = am9513_ctr[i].cntr;
    }
    return;
  case 0x80:
    if (trace_am9513) printf("am9513: disarm and save\n");

    for (i = 1; i < 6; i++) {
      if (value & (1 << (i - 1)))
	am9513_ctr[i].hold = am9513_ctr[i].cntr;
    }
    bits = value & 0x1f;
    am9513_armed_bits &= ~(bits << 1);
    return;
  case 0x60:
    if (trace_am9513) {
      printf("am9513: load and arm ");
      if (value & 0x10) { printf("s5 "); }
      if (value & 0x08) { printf("s4 "); }
      if (value & 0x04) { printf("s3 "); }
      if (value & 0x02) { printf("s2 "); }
      if (value & 0x01) { printf("s1 "); }
      printf("\n");
    }

    for (i = 1; i < 6; i++) {
      if (value & (1 << (i - 1)))
	am9513_ctr[i].cntr = am9513_ctr[i].load;
    }
    bits = value & 0x1f;
    am9513_armed_bits |= bits << 1;
    return;
  case 0x40:
    if (trace_am9513) {
      printf("am9513: load ");
      if (value & 0x10) { printf("s5 "); }
      if (value & 0x08) { printf("s4 "); }
      if (value & 0x04) { printf("s3 "); }
      if (value & 0x02) { printf("s2 "); }
      if (value & 0x01) { printf("s1 "); }
      printf("\n");
    }

    for (i = 1; i < 6; i++) {
      if (value & (1 << (i - 1)))
	am9513_ctr[i].cntr = am9513_ctr[i].load;
    }
    return;
  case 0x20:
    if (trace_am9513) {
      printf("am9513: arm ");
      if (value & 0x10) { printf("s5 "); }
      if (value & 0x08) { printf("s4 "); }
      if (value & 0x04) { printf("s3 "); }
      if (value & 0x02) { printf("s2 "); }
      if (value & 0x01) { printf("s1 "); }
      printf("\n");
    }

    bits = value & 0x1f;
    am9513_armed_bits |= bits << 1;
    return;
  case 0x00:
    if (trace_am9513) printf("am9513: load data ptr e%d g%d\n", (value & 0x18)>>3, (value & 0x7));
    am9513_data_ptr = value;
    am9513_data_ptr_byte = 1;
    return;
  }

  if (trace_am9513) printf("am9513: unknown?\n");
}

unsigned int am9513_read(unsigned int pa, int size)
{
  unsigned int offset = pa & 0xff;
  unsigned int value = 0;

  switch (offset) {
  case 0: case 1:
    /* Data port read — returns the current value pointed at by the data
       pointer (mode/load/hold/etc).  We don't fully model the data-pointer
       read path; return 0.  The PROM mainly writes here. */
    value = 0;
    break;

  case 2: case 3: {
    /* Status register — bits 1..5 are the current OUT pin states for
       counters 1..5; bit 7 is the byte-pointer.  Sun-2 PROM rev 1.0F
       polls this to detect counter terminal-count after issuing a
       "clr output bit" command.  Previously we returned a static
       0x0b00 here, so OUT1 always read as 1 and the polling loop on
       counter 1 never made progress. */
    value = (am9513_output_bits & 0x3e);  /* OUT1..OUT5 in bits 1..5 */
    if (am9513_data_ptr_byte) value |= 0x80;  /* BPR (byte pointer) */
    break;
  }
  }

  if (trace_am9513)
    printf("am9513: read %x (%d) -> %x\n", pa, size, value);

  return value;
}

/* AM9513 data-pointer auto-increment.  After every successful data
   access the pointer cycles Mode → Load → Hold within a counter group,
   then wraps to Mode of the next counter (1..5).  The PROM relies on
   this to write 16-bit mode/load/hold triples sequentially after a
   single "load data ptr" command. */
static void am9513_advance_dp(void)
{
  int e = (am9513_data_ptr & 0x18) >> 3;
  int g = (am9513_data_ptr & 0x07);

  if (g >= 1 && g <= 5) {
    if (e < 2) {
      e++;
    } else {
      e = 0;
      g = (g < 5) ? g + 1 : 1;
    }
    am9513_data_ptr = (am9513_data_ptr & ~0x1f) | ((e & 3) << 3) | (g & 7);
  }
}

/* Word write to the data port — used when the chip is in 16-bit data-bus
   mode (Master Mode bit 13 set).  Sun-2 rev 1.0F PROM enables this and
   then writes 16-bit mode/load/hold values atomically.  The earlier code
   truncated word writes to 8 bits, dropping the high byte and corrupting
   the timer counters so no periodic IRQ ever fired. */
static void am9513_wr_data16(unsigned int value)
{
  int e = (am9513_data_ptr & 0x18) >> 3;
  int g = (am9513_data_ptr & 0x7);

  value &= 0xffff;

  if (trace_am9513)
    printf("am9513_wr_data16: e%d g%d <- %04x\n", e, g, value);

  if (g >= 1 && g <= 5) {
    switch (e) {
    case 0: am9513_ctr[g].mode = value; break;
    case 1: am9513_ctr[g].load = value; break;
    case 2: am9513_ctr[g].hold = value; break;
    }
  }
  /* 16-bit mode bypasses the LSB/MSB byte sequencer */
  am9513_data_ptr_byte = 0;
  am9513_advance_dp();
}

void am9513_write(unsigned int pa, unsigned int value, int size)
{
  unsigned int offset = pa & 0xff;

  if (trace_am9513)
    printf("am9513: write %x (%d) <- %x\n", pa, size, value);

  if (size == 2) {
    /* 16-bit access.  Data port: write whole 16-bit register atomically.
       Command port: AM9513 commands are 8 bits; PROM sends 0xff in the
       high byte as padding.  Take the low byte. */
    switch (offset) {
    case 0: am9513_wr_data16(value); break;
    case 2: am9513_wr_cmd(value & 0xff); break;
    }
    return;
  }

  /* Byte access — preserves the existing 8-bit-mode byte sequencer */
  switch (offset) {
  case 0: am9513_wr_data(value & 0xff); break;
  case 2: am9513_wr_cmd(value & 0xff); break;
  }
}

/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
