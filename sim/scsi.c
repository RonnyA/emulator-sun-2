#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "sim.h"
#include "scsi.h"
#include "scsi_disk.h"

extern int trace_scsi;
extern int trace_sc;
extern int quiet;

int scsi_bus_phase;
unsigned int scsi_bus_state;
unsigned int scsi_bus_data;
unsigned int scsi_irq;

static int id_selected;
static int time_in_state;
static int future_phase;
static int future_phase_time;

int scsi_cmd_size;
unsigned char scsi_cmd_buf[32];
unsigned char scsi_cmd_last;
int scsi_cmd_has_been_written;
int scsi_cmd_has_been_read;



void _scsi_set_phase(int phase, int delay)
{
  if (delay == 0) {
    scsi_bus_phase = phase;
    future_phase_time = 0;
    time_in_state = 0;
    return;
  }

  if (future_phase_time == 0) {
    future_phase = phase;
    future_phase_time = delay;
  }
}

void _scsi_show_state(void)
{
  printf("scsi: phase ");

  switch (scsi_bus_phase) {
  case PHASE_BUS_FREE:	  printf("BUS-FREE"); break;
  case PHASE_ARBITRATION: printf("ARBITRATION"); break;
  case PHASE_SELECTION:	  printf("SELECTION"); break;
  case PHASE_RESELECTION: printf("RESELECTION"); break;
  case PHASE_DATA_OUT:	  printf("DATA-OUT"); break;
  case PHASE_DATA_IN:	  printf("DATA-IN"); break;
  case PHASE_COMMAND:	  printf("COMMAND"); break;
  case PHASE_STATUS:	  printf("STATUS"); break;
  case PHASE_MESSAGE_OUT: printf("MSG-OUT"); break;
  case PHASE_MESSAGE_IN:  printf("MSG-IN"); break;
  }

  printf(" data %02x bus [", scsi_bus_data);
  if (scsi_bus_state & SCSI_BUS_REQ) printf("REQ ");
  if (scsi_bus_state & SCSI_BUS_SEL) printf("SEL ");
  if (scsi_bus_state & SCSI_BUS_BSY) printf("BSY ");
  if (scsi_bus_state & SCSI_BUS_MSG) printf("MSG ");
  if (scsi_bus_state & SCSI_BUS_CD) printf("CD ");
  if (scsi_bus_state & SCSI_BUS_IO) printf("IO ");
  printf("]\n");
}

int _scsi_set_state(void)
{
  scsi_bus_state &= ~(SCSI_BUS_SEL |
		      SCSI_BUS_BSY |
		      SCSI_BUS_REQ |
		      SCSI_BUS_MSG |
		      SCSI_BUS_CD |
		      SCSI_BUS_IO);

  switch (scsi_bus_phase) {
  case PHASE_BUS_FREE:	  break;
  case PHASE_ARBITRATION:
    scsi_bus_state |= SCSI_BUS_BSY; break;
  case PHASE_SELECTION:
    scsi_bus_state |= SCSI_BUS_SEL; break;
  case PHASE_RESELECTION:
    scsi_bus_state |= SCSI_BUS_SEL; break;
  case PHASE_DATA_OUT:
    scsi_bus_state |= SCSI_BUS_BSY; break;
  case PHASE_DATA_IN:
    scsi_bus_state |= SCSI_BUS_BSY |                              SCSI_BUS_IO; break;
  case PHASE_COMMAND:
    scsi_bus_state |= SCSI_BUS_BSY |                SCSI_BUS_CD;               break;
  case PHASE_STATUS:
    scsi_bus_state |= SCSI_BUS_BSY |                SCSI_BUS_CD | SCSI_BUS_IO; break;
  case PHASE_MESSAGE_OUT:
    scsi_bus_state |= SCSI_BUS_BSY | SCSI_BUS_MSG | SCSI_BUS_CD;               break;
  case PHASE_MESSAGE_IN:
    scsi_bus_state |= SCSI_BUS_BSY | SCSI_BUS_MSG | SCSI_BUS_CD | SCSI_BUS_IO; break;
  }
  return scsi_bus_state;
}

void _scsi_check_initiator(void)
{
  if (trace_scsi > 1)
    printf("_scsi_check_initiator() scsi_bus_state %x\n", scsi_bus_state);

  /* Historical heuristic: drop REQ+BSY when seemingly stuck in
     PHASE_DATA_OUT (BSY|REQ with no MSG/CD/IO).  Original mask omitted
     IO, which made this also fire in PHASE_DATA_IN (BSY|IO|REQ) once
     we started using that phase for Sun-3 chained-DMA flow — clobbering
     REQ mid-DMA-handshake.  Include IO in the mask so DATA_IN is
     excluded; only the literal DATA_OUT-with-stuck-REQ case still
     triggers the clear. */
  if ((scsi_bus_state & (SCSI_BUS_BSY | SCSI_BUS_MSG | SCSI_BUS_CD | SCSI_BUS_IO | SCSI_BUS_REQ)) ==
      (SCSI_BUS_BSY | SCSI_BUS_REQ))
    {
      if (trace_scsi) printf("scsi: clear REQ (DATA_OUT stuck)\n");
      scsi_bus_state &= ~SCSI_BUS_REQ;
      scsi_bus_state &= ~SCSI_BUS_BSY;
    }
}


void _scsi_update_bus_state(unsigned int out, unsigned int *pirq)
{
  if (trace_scsi > 1)
    printf("_scsi_update_bus_state()\n");

  *pirq = 0;

  switch (scsi_bus_phase) {
  case PHASE_BUS_FREE:
    if (trace_scsi > 1) printf("scsi: PHASE_BUS_FREE\n");
if (trace_scsi && id_selected >= 0) printf("scsi: PHASE_BUS_FREE (last select %d)\n", id_selected);
    id_selected = -1;
    if (scsi_bus_state & SCSI_BUS_SEL) {
      _scsi_set_phase(PHASE_SELECTION, 0);
      if (trace_scsi > 1) printf("scsi: SEL, data 0x%x (sc_data 0x%x)\n", scsi_bus_data, sc_get_data());

      if (scsi_bus_data == 0x01) {
	scsi_bus_state |= SCSI_BUS_BSY;
	if (trace_scsi > 1) printf("scsi: target id0\n");
	id_selected = 0;
      } else
	if (scsi_bus_data == 0x10) {
	  scsi_bus_state |= SCSI_BUS_BSY;
	  if (trace_scsi > 1) printf("scsi: target id4\n");
	  id_selected = 4;
      } else {
        int x;
	for (x = 0; x < 8; x++) if (scsi_bus_data & (1 << x)) { id_selected = x; if (!quiet) printf("scsi: target id%d\n", x); break; }
	scsi_bus_state &= ~SCSI_BUS_BSY;
scsi_bus_state = 0;
//	if (trace_scsi) printf("scsi: ~id0&~id4, remove BSY\n");
	if (trace_scsi) printf("scsi: bus %02x, remove BSY\n", scsi_bus_data);
//_scsi_set_phase(PHASE_BUS_FREE, 0);
if (trace_scsi > 1) _scsi_show_state();
scsi_bus_data = 0;
//scsi_bus_data = 0xff;
      }
    }
    break;
  case PHASE_ARBITRATION:
    break;
  case PHASE_SELECTION:
    if (trace_scsi > 1) printf("scsi: PHASE_SELECTION (%d)\n", time_in_state);

    if ((scsi_bus_state & SCSI_BUS_SEL) == 0) {
      /* SEL drop with no target asserting BSY (unrecognised target) means
         selection failed -- drop straight to BUS_FREE so the host driver's
         selection-timeout path can run.  Only assert BSY+go-COMMAND when
         a known target accepted the selection (id 0 for sd0, id 4 for
         the Sun-2 second target). */
      if (id_selected == 0 || id_selected == 4) {
        scsi_bus_state |= SCSI_BUS_BSY;
        _scsi_set_phase(PHASE_COMMAND, 1);
        if (trace_scsi > 1) printf("scsi: PHASE_SELECTION, no SEL -> COMMAND\n");
      } else {
        _scsi_set_phase(PHASE_BUS_FREE, 0);
        if (trace_scsi > 1) printf("scsi: PHASE_SELECTION, no SEL, no target -> BUS-FREE\n");
      }
    }

    if ((scsi_bus_state & (SCSI_BUS_SEL|SCSI_BUS_BSY)) == 0 && time_in_state > 2) {
      _scsi_set_phase(PHASE_BUS_FREE, 0);
      if (trace_scsi > 1) printf("scsi: PHASE_SELECTION, no SEL, no BSY -> BUS-FREE\n");
    }
    break;
  case PHASE_RESELECTION:
    break;
  case PHASE_DATA_OUT:
  case PHASE_DATA_IN:
    break;

  case PHASE_COMMAND:
    if (trace_scsi > 1) printf("scsi: COMMAND (%d)\n", time_in_state);
    if (time_in_state == 0) {
      scsi_cmd_size = 0;
      scsi_irq = 0;
      scsi_bus_state |= SCSI_BUS_CD;
      scsi_bus_state |= SCSI_BUS_REQ;
    }

    if (scsi_cmd_size > 0) {
      unsigned char *pbuf;
      int psiz;

      if (trace_scsi && scsi_cmd_size == 1)
	printf("scsi: command done; cmd[0] %02x\n", scsi_cmd_buf[0]);

      if (id_selected == 4 && scsi_cmd_size == 6) {
	if (trace_scsi)
	  printf("scsi: tape command %02x cdb=%02x %02x %02x %02x %02x %02x\n",
	         scsi_cmd_buf[0],
	         scsi_cmd_buf[0], scsi_cmd_buf[1], scsi_cmd_buf[2],
	         scsi_cmd_buf[3], scsi_cmd_buf[4], scsi_cmd_buf[5]);
      }

      switch ((scsi_cmd_buf[0] & 0xf0) >> 4) {
      case 0:
	if (scsi_cmd_size == 6) {
	  if (trace_scsi)
	    printf("scsi: command0 done; size %d, cmd[0] %02x\n", scsi_cmd_size, scsi_cmd_buf[0]);
	  *pirq = 1;
	  sc_reset_odd_len();
	  switch (scsi_cmd_buf[0]) {
	  case 0x00: /* STATUS (test unit ready) — no DMA */
	    if (trace_scsi) printf("scsi: command status\n");
	    _scsi_test_unit_ready(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz);
	    /* No DMA; snap residue to 0xFFFF so PROM "sd: short transfer"
	       check (PC ef9448) sees a clean transfer.  See sc.c. */
	    sc_dma_complete_no_xfer();
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x01: /* REWIND */
	    if (trace_scsi)
	      printf("scsi%d: REWIND (was file %d)\n",
	             id_selected, scsi_units[id_selected].fileno);
	    sc_dma_complete_no_xfer();    /* no data phase: snap dma_count=0xFFFF */
	    _scsi_set_phase(PHASE_STATUS, 0);
	    _scsi_set_filenum(id_selected, 0);
	    break;
	  case 0x03: /* SENSE */
	    if (trace_scsi) printf("scsi: command sense\n");
	    _scsi_request_sense(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz);
	    sc_dma_read_data(pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x08: /* READ */
	    if (trace_scsi)
	      printf("scsi: command READ %02x %02x %02x %02x %02x %02x %02x %02x\n",
		     scsi_cmd_buf[0], scsi_cmd_buf[1], scsi_cmd_buf[2], scsi_cmd_buf[3],
		     scsi_cmd_buf[4], scsi_cmd_buf[5], scsi_cmd_buf[6], scsi_cmd_buf[7]);
	    if (_scsi_read_block(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz)) {
	      abortf("scsi: read failed from disk image\n");
	    }
	    if ((scsi_cmd_buf[1] & 0xe0) == 0) {
	      sc_dma_read_data(pbuf, psiz);
	      _scsi_set_phase(PHASE_STATUS, 0);
	    } else {
	      scsi_units[id_selected].status[0] = 0x02;
	      _scsi_set_phase(PHASE_STATUS, 0);
	    }
	    break;
	  case 0x0a: /* WRITE */
	    if (trace_scsi) {
	      printf("scsi: command WRITE %02x %02x %02x %02x %02x %02x %02x %02x\n",
		     scsi_cmd_buf[0], scsi_cmd_buf[1], scsi_cmd_buf[2], scsi_cmd_buf[3],
		     scsi_cmd_buf[4], scsi_cmd_buf[5], scsi_cmd_buf[6], scsi_cmd_buf[7]);
//	      trace_scsi = 1;
//	      trace_sc = 1;
	    }
	    _scsi_write_block_start(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz);
	    sc_dma_write_data(pbuf, psiz);
	    _scsi_write_block_end(id_selected, scsi_cmd_buf, 6, pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x0d: /* QIC02 (vendor specific for cipher tape) */
	    printf("scsi: command QIC02\n");
	    sc_dma_complete_no_xfer();    /* no data phase: snap dma_count=0xFFFF */
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  default:
	    printf("scsi: command0 %02x?", scsi_cmd_buf[0]);
	    break;
	  }
	}
	break;
      case 1:
	if (scsi_cmd_size == 6) {
	  if (trace_scsi)
	    printf("scsi: command1 done; size %d, cmd[0] %02x\n", scsi_cmd_size, scsi_cmd_buf[0]);
	  *pirq = 1;
	  sc_reset_odd_len();
	  switch (scsi_cmd_buf[0]) {
	  case 0x11: /* SPACE */
	    if (trace_scsi)
	      printf("scsi%d: SPACE (advance from file %d) cmd=%02x %02x %02x %02x %02x %02x\n",
	             id_selected, scsi_units[id_selected].fileno,
	             scsi_cmd_buf[0], scsi_cmd_buf[1], scsi_cmd_buf[2],
	             scsi_cmd_buf[3], scsi_cmd_buf[4], scsi_cmd_buf[5]);
	    sc_dma_complete_no_xfer();    /* no data phase: snap dma_count=0xFFFF */
	    _scsi_set_phase(PHASE_STATUS, 0);
	    _scsi_next_file(id_selected);
	    break;
	  case 0x12: /* INQUIRY */
	    if (trace_scsi)
	      printf("scsi: command INQUIRY %02x %02x %02x %02x %02x %02x %02x %02x\n",
		     scsi_cmd_buf[0], scsi_cmd_buf[1], scsi_cmd_buf[2], scsi_cmd_buf[3],
		     scsi_cmd_buf[4], scsi_cmd_buf[5], scsi_cmd_buf[6], scsi_cmd_buf[7]);
	    if (_scsi_inquiry(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz)) {
	      abortf("scsi: inquiry failed\n");
	    }
	    sc_dma_read_data(pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x15: /* MODE_SELECT — DATA OUT (host RAM -> device).
			Tick dma_count for every parameter byte the host
			pushes; otherwise tpboot's mode-2 confirm sees a
			residual and prints "st: short transfer". */
	    if (trace_scsi) printf("scsi: command mode_select\n");
	    if (_scsi_mode_select(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz)) {
	      abortf("scsi: mode_select failed\n");
	    }
	    sc_dma_write_data(pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x1a: /* MODE_SENSE */
	    if (trace_scsi) printf("scsi: command mode_sense\n");
	    if (_scsi_mode_sense(id_selected, scsi_cmd_buf, 6, &pbuf, &psiz)) {
	      abortf("scsi: mode sense failed\n");
	    }
	    sc_dma_read_data(pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x1b: /* START_STOP_UNIT */
	    if (trace_scsi) printf("scsi: command start_stop_unit\n");
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  default:
	    printf("scsi: command1 %02x?", scsi_cmd_buf[0]);
	    break;
	  }
	}
	break;
      case 2:
	/* 10-byte SCSI commands (group 2: 0x20..0x2F).  CDB is 10 bytes
	   total — wait for full CDB before dispatch. */
	if (scsi_cmd_size == 10) {
	  unsigned char *pbuf;
	  int psiz;
	  if (trace_scsi)
	    printf("scsi: command2 done; size %d, cmd[0] %02x\n",
		   scsi_cmd_size, scsi_cmd_buf[0]);
	  *pirq = 1;
	  sc_reset_odd_len();
	  switch (scsi_cmd_buf[0]) {
	  case 0x25: /* READ CAPACITY (10) */
	    if (trace_scsi) printf("scsi: command READ CAPACITY\n");
	    if (_scsi_read_capacity(id_selected, scsi_cmd_buf, 10, &pbuf, &psiz)) {
	      abortf("scsi: read_capacity failed\n");
	    }
	    sc_dma_read_data(pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  case 0x28: /* READ (10) */ {
	    /* Decode 10-byte CDB into a synthetic 6-byte form so we
	       can reuse _scsi_read_block().  10-byte format:
	         [0]opcode [1]flags [2..5]LBA(BE32) [6]reserved
	         [7..8]xfer_len(BE16) [9]control */
	    int sblock = ((unsigned)scsi_cmd_buf[2] << 24)
			| ((unsigned)scsi_cmd_buf[3] << 16)
			| ((unsigned)scsi_cmd_buf[4] << 8)
			|  (unsigned)scsi_cmd_buf[5];
	    int scount = ((unsigned)scsi_cmd_buf[7] << 8)
			|  (unsigned)scsi_cmd_buf[8];
	    if (trace_scsi)
	      printf("scsi: command READ(10) lba=0x%x count=%d\n", sblock, scount);
	    /* Build a synthetic READ(6) CDB. */
	    unsigned char cdb6[6];
	    cdb6[0] = 0x08;
	    cdb6[1] = (sblock >> 16) & 0x1F;
	    cdb6[2] = (sblock >> 8)  & 0xFF;
	    cdb6[3] =  sblock        & 0xFF;
	    cdb6[4] = scount > 255 ? 0 : (unsigned char)scount;
	    cdb6[5] = 0;
	    if (_scsi_read_block(id_selected, cdb6, 6, &pbuf, &psiz)) {
	      abortf("scsi: read(10) failed\n");
	    }
	    sc_dma_read_data(pbuf, psiz);
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  }
	  default:
	    printf("scsi: command2 %02x?\n", scsi_cmd_buf[0]);
	    sc_dma_complete_no_xfer();
	    _scsi_set_phase(PHASE_STATUS, 0);
	    break;
	  }
	}
	break;
      }
    }
    break;

  case PHASE_STATUS:
    if (trace_scsi > 1) printf("scsi: STATUS (%d)\n", time_in_state);
    if (time_in_state == 1) {
      _scsi_set_state();
      scsi_bus_state |= SCSI_BUS_REQ;
      sc_set_cmd_reg(scsi_units[id_selected].status[0]);
      scsi_cmd_has_been_read = 0;
    } else {
      if (scsi_cmd_has_been_read > 0) {
	scsi_units[id_selected].status[0] = 0;
	scsi_bus_state &= ~SCSI_BUS_REQ;
	_scsi_set_phase(PHASE_MESSAGE_IN, 1);
      }
    }
    break;

  case PHASE_MESSAGE_IN:
    if (trace_scsi > 1) printf("scsi: MESSAGE_IN (%d)\n", time_in_state);
    if (time_in_state <= 1) {
      sc_set_cmd_reg(0x0);
      _scsi_set_state();
      scsi_bus_state |= SCSI_BUS_REQ;
      scsi_cmd_has_been_read = 0;
    } else {
      if (scsi_cmd_has_been_read > 0) {
	scsi_bus_state &= ~SCSI_BUS_REQ;
	_scsi_set_phase(PHASE_BUS_FREE, 0);
	_scsi_set_state();
      }
    }
    break;

  case PHASE_MESSAGE_OUT:
    if (trace_scsi > 1) printf("scsi: MESSAGE_OUT (%d) cmd_written=%d\n",
                               time_in_state, scsi_cmd_has_been_written);
    if (time_in_state == 1) {
      _scsi_set_state();
      scsi_bus_state |= SCSI_BUS_REQ;
      scsi_cmd_has_been_written = 0;
      scsi_cmd_size = 0;
    } else if (scsi_cmd_has_been_written > 0) {
      /* IDENTIFY (or other 1-byte message) received -- drop REQ and
         transition to COMMAND phase to receive the CDB. */
      if (trace_scsi) printf("scsi: MSG_OUT got byte 0x%02x -> COMMAND\n",
                             scsi_cmd_buf[0] & 0xFF);
      scsi_bus_state &= ~SCSI_BUS_REQ;
      scsi_cmd_has_been_written = 0;
      scsi_cmd_size = 0;
      _scsi_set_phase(PHASE_COMMAND, 1);
    }
    break;
  }
}

void scsi_update(unsigned short *pdata, unsigned int out, unsigned int *pin, unsigned int *pirq)
{
  unsigned int irq;

  if (out & SCSI_BUS_RST) {
      _scsi_set_phase(PHASE_BUS_FREE, 0);
      _scsi_set_state();
  }

  time_in_state++;
  if (future_phase_time > 0) {
    future_phase_time--;
    if (future_phase_time == 0) {
      scsi_bus_phase = future_phase;
      time_in_state = 0;
    }
  }

  scsi_bus_data = *pdata;

  if (out & SCSI_BUS_SEL)
    scsi_bus_state |= SCSI_BUS_SEL;
  else
    scsi_bus_state &= ~SCSI_BUS_SEL;

  _scsi_check_initiator();
  _scsi_update_bus_state(out, &irq);

  scsi_irq |= irq;

  *pirq = scsi_irq;
  *pin = scsi_bus_state;
  *pdata = scsi_bus_data;

  if (trace_scsi > 1) {
    _scsi_show_state();
  }
}

void scsi_write_cmd_byte(int value)
{
  scsi_cmd_last = value;
  scsi_cmd_has_been_written++;
  if (scsi_cmd_size < 32) {
    scsi_cmd_buf[scsi_cmd_size++] = value;
  }
}

unsigned int scsi_read_cmd_byte(void)
{
  scsi_cmd_has_been_read++;
  if (scsi_cmd_has_been_read == 1) {
    if (scsi_bus_state == PHASE_STATUS) {
      sc_set_cmd_reg(0x0);
      _scsi_set_phase(PHASE_MESSAGE_IN, 0);
      _scsi_set_state();
      scsi_bus_state |= SCSI_BUS_REQ;
    }
  }
  return scsi_cmd_last;
}


/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
