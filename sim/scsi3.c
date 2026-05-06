/*
 * scsi3.c -- Sun-3 SCSI bus model + per-target state machine.
 *
 * Implements the spec § 18 timeline byte-by-byte.  The host
 * (sun3_si.c) drives initiator lines via scsi3_set_init_lines() and
 * reads target-driven signals (BSY, REQ, phase) plus the live bus
 * data byte to compute its NCR-5380 CBSR / CDR / BSR registers.
 *
 * State machine highlights:
 *   - SEL rising latches the target ID (decoded from the data lines).
 *   - SEL falling: if a known target was selected, target asserts BSY
 *     and either enters MSG_OUT (if ATN was asserted) or COMMAND, with
 *     REQ asserted on the same step.
 *   - Each ACK rising edge clocks one byte.  Target-driven phases
 *     (status, msg-in, data-in) stage the next byte BEFORE asserting
 *     REQ.  Initiator-driven phases (cmd, msg-out, data-out) latch
 *     init_data on ACK rising.
 *   - DMA path uses scsi3_dma_in / scsi3_dma_out which short-circuit
 *     the per-byte ACK handshake but follow the same byte-counter
 *     advance + phase-transition rules.
 *
 * CDB dispatch lives at the boundary between cmd-byte-received and
 * data-phase-entered.  Result payload comes from scsi_disk.c.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scsi3.h"
#include "scsi_disk.h"

extern int trace_scsi;

/* ------------------------------------------------------------------
 * Module-private state
 * ------------------------------------------------------------------ */
static int      s_phase;             /* enum scsi3_phase, BUS_FREE = -1 */
static int      s_target_bsy;
static int      s_target_req;
static uint8_t  s_bus_data;          /* live data byte on the bus */

/* Initiator-side state (for edge detection). */
static int      s_init_bsy;
static int      s_init_sel, s_init_sel_prev;
static int      s_init_ack, s_init_ack_prev;
static int      s_init_atn;
static int      s_init_rst, s_init_rst_prev;
static uint8_t  s_init_data;

/* Currently selected target.  -1 = none. */
static int      s_target_id;

/* Currently selected LUN.  Resolved either from the IDENTIFY message
   (modern path) or from CDB byte 1 bits 7:5 (legacy path; SunOS uses
   this when SCSI_EN_DISCON is off, e.g. autoconf-time scsi_slave).
   We only emulate one disk per target (LUN 0); commands to any other
   LUN return INQUIRY byte 0 = DTYPE_NOTPRESENT (0x7F) so SunOS's
   sd_make_unit walks past the slot. */
static int      s_lun;

/* Per-transaction. */
static uint8_t  s_cdb[12];
static int      s_cdb_len;          /* expected length (6 / 10 / 12) */
static int      s_cdb_pos;          /* bytes received so far */
static uint8_t  s_status;           /* status byte to return */
static uint8_t  s_msg_in;           /* msg-in byte (typically 0x00 = COMMAND_COMPLETE) */
static uint8_t  s_msg_out;          /* msg-out byte received (IDENTIFY) */
static uint8_t *s_xfer_buf;         /* points into scsi_units[id].data */
static int      s_xfer_len;
static int      s_xfer_pos;
static int      s_xfer_dir;         /* 0 = none, 1 = in, 2 = out */

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */
static const char *phase_name(int p)
{
  switch (p) {
    case SCSI3_PH_BUS_FREE: return "BUS_FREE";
    case SCSI3_PH_DATA_OUT: return "DATA_OUT";
    case SCSI3_PH_DATA_IN:  return "DATA_IN";
    case SCSI3_PH_COMMAND:  return "COMMAND";
    case SCSI3_PH_STATUS:   return "STATUS";
    case SCSI3_PH_MSG_OUT:  return "MSG_OUT";
    case SCSI3_PH_MSG_IN:   return "MSG_IN";
    default:                return "?";
  }
}

#define LOG(...) do { if (trace_scsi) fprintf(stderr, "[scsi3] " __VA_ARGS__); } while (0)

static void go_bus_free(void)
{
  s_phase       = SCSI3_PH_BUS_FREE;
  s_target_bsy  = 0;
  s_target_req  = 0;
  s_target_id   = -1;
  s_lun         = 0;
  s_msg_out     = 0;
  s_cdb_len     = 0;
  s_cdb_pos     = 0;
  s_xfer_buf    = NULL;
  s_xfer_len    = 0;
  s_xfer_pos    = 0;
  s_xfer_dir    = 0;
  LOG("-> BUS_FREE\n");
}

static int cdb_length_for(uint8_t opcode)
{
  switch ((opcode >> 5) & 7) {
    case 0: return 6;
    case 1: return 10;
    case 2: return 10;
    case 5: return 12;
    default: return 6;     /* fallback */
  }
}

/* Decode the target id from selection-time data lines.
   Sun host id is 7 (0x80); target id is encoded as (1 << id).
   The data lines carry (1 << host_id) | (1 << target_id) during
   selection.  Mask off host bit, find target bit. */
static int decode_target_id(uint8_t data)
{
  int x;
  uint8_t mask = data & 0x7F;        /* drop host bit (id 7) */
  for (x = 0; x < 7; x++) {
    if (mask & (1 << x)) return x;
  }
  return -1;
}

/* Is this target id one we will respond to?  Phase 4: id 0 only.
   Phase 6 will extend to multi-target if needed. */
static int target_present(int id)
{
  return (id == 0);
}

/* ------------------------------------------------------------------
 * Phase entry helpers — set bus-state for the new phase.
 * ------------------------------------------------------------------ */

/* Phase entry helpers.  Each:
 *   - sets the new phase + drives any target-staged data byte onto bus,
 *   - leaves REQ deasserted if we're transitioning *from* an ACK-rising
 *     edge (host still has ACK high; REQ must drop until ACK falls),
 *   - asserts REQ if entering "fresh" (e.g. SEL falling → COMMAND).
 *
 * The `req_now` parameter is the deciding factor.
 */

static void enter_msg_out(int req_now)
{
  s_phase      = SCSI3_PH_MSG_OUT;
  s_target_bsy = 1;
  s_target_req = req_now;
  LOG("-> MSG_OUT (req=%d)\n", req_now);
}

static void enter_command(int req_now)
{
  s_phase      = SCSI3_PH_COMMAND;
  s_target_bsy = 1;
  s_cdb_pos    = 0;
  s_cdb_len    = 6;          /* updated after first byte */
  s_target_req = req_now;
  LOG("-> COMMAND (req=%d)\n", req_now);
}

static void stage_data_in_byte(void)
{
  if (s_xfer_buf && s_xfer_pos < s_xfer_len) {
    s_bus_data = s_xfer_buf[s_xfer_pos];
  }
}

static void enter_data_in(int req_now)
{
  s_phase      = SCSI3_PH_DATA_IN;
  s_target_bsy = 1;
  stage_data_in_byte();
  s_target_req = req_now;
  LOG("-> DATA_IN (%d bytes, req=%d)\n", s_xfer_len, req_now);
}

static void enter_data_out(int req_now)
{
  s_phase      = SCSI3_PH_DATA_OUT;
  s_target_bsy = 1;
  s_target_req = req_now;
  LOG("-> DATA_OUT (%d bytes, req=%d)\n", s_xfer_len, req_now);
}

static void enter_status(int req_now)
{
  s_phase      = SCSI3_PH_STATUS;
  s_target_bsy = 1;
  s_bus_data   = s_status;     /* drive status onto data lines */
  s_target_req = req_now;
  LOG("-> STATUS (status=%02X, req=%d)\n", s_status, req_now);
}

static void enter_msg_in(int req_now)
{
  s_phase      = SCSI3_PH_MSG_IN;
  s_target_bsy = 1;
  s_bus_data   = s_msg_in;     /* COMMAND_COMPLETE = 0x00 */
  s_target_req = req_now;
  LOG("-> MSG_IN (msg=%02X, req=%d)\n", s_msg_in, req_now);
}

/* ------------------------------------------------------------------
 * CDB dispatch — called when COMMAND phase has received a full CDB.
 * Calls into scsi_disk.c to build the data payload, then transitions
 * to the appropriate next phase.
 * ------------------------------------------------------------------ */

static void dispatch_cdb(void)
{
  uint8_t op = s_cdb[0];
  unsigned char *buf = NULL;
  int len = 0;
  int rc = 0;

  s_xfer_buf = NULL;
  s_xfer_len = 0;
  s_xfer_pos = 0;
  s_xfer_dir = 0;
  s_status   = 0x00;
  s_msg_in   = 0x00;          /* COMMAND_COMPLETE */

  /* LUN resolution: prefer the LUN from IDENTIFY (set in MSG_OUT
     handler).  If no IDENTIFY was sent (PROM / autoconf-time polled
     scsi_slave with SCSI_EN_DISCON off), fall back to CDB byte 1
     bits 7:5 -- the SCSI-1 in-CDB LUN field (sys/scsi/impl/commands.h
     declares cdb.lun:3 as the high 3 bits of byte 1). */
  if ((s_msg_out & 0x80) == 0) {
    s_lun = (s_cdb[1] >> 5) & 0x07;
  }

  if (trace_scsi) {
    fprintf(stderr, "[scsi3] CDB %02X", op);
    int i;
    for (i = 1; i < s_cdb_len; i++) fprintf(stderr, " %02X", s_cdb[i]);
    fprintf(stderr, " (lun=%d)\n", s_lun);
  }

  /* Non-existent LUN: we model only LUN 0 per target.  Per
     scsi/generic/inquiry.h DPQ_NEVER (0x60) | DTYPE_UNKNOWN (0x1F)
     = 0x7F advertises "type never supported on this LUN".  SunOS
     sd_make_unit (sd.c:221-229) checks inq_dtype and skips the slot
     when it is not DTYPE_DIRECT, so the kernel walks past the
     phantom unit without printing an attach line.  For non-INQUIRY
     commands we return CHECK CONDITION; the kernel will issue
     REQUEST SENSE which our backing returns as NO_SENSE -- harmless
     for the autoconf path which only cares about the INQUIRY result. */
  if (s_lun != 0) {
    if (op == 0x12) {
      /* INQUIRY: synthesise a "not present" response.  Use the unit
         buffer so the data-in xfer drives bytes the kernel can read. */
      _scsi_inquiry(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      buf[0] = 0x7F;                  /* DPQ_NEVER | DTYPE_UNKNOWN */
      buf[1] = 0;
      if (s_cdb[4] && s_cdb[4] < len) len = s_cdb[4];
      s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      s_status = 0x00;
      LOG("dispatch op=%02X lun=%d -> DTYPE_NOTPRESENT INQUIRY\n", op, s_lun);
      enter_data_in(0);
      return;
    }
    /* Other commands to a non-existent LUN: reject with CHECK COND. */
    s_status = 0x02;
    LOG("dispatch op=%02X lun=%d -> CHECK CONDITION\n", op, s_lun);
    enter_status(0);
    return;
  }

  switch (op) {
    case 0x00: /* TEST UNIT READY — no data */
      rc = _scsi_test_unit_ready(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x01: /* REZERO UNIT — no data */
      s_status = 0x00;
      break;

    case 0x03: /* REQUEST SENSE — data in */
      rc = _scsi_request_sense(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x08: /* READ(6) — data in */
      rc = _scsi_read_block(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      if (rc == 0) {
        s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      }
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x0A: /* WRITE(6) — data out (note: write_block_start sets up buffer; write_block_end actually writes after data received) */
      rc = _scsi_write_block_start(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      if (rc == 0) {
        s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 2;
      }
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x12: /* INQUIRY — data in */
      rc = _scsi_inquiry(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      /* Honour CDB[4] allocation length (Phase 4 fix). */
      if (s_cdb[4] && s_cdb[4] < len) len = s_cdb[4];
      s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x15: /* MODE SELECT — data out */
      rc = _scsi_mode_select(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      if (rc == 0) {
        s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 2;
      }
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x1A: /* MODE SENSE — data in */
      rc = _scsi_mode_sense(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      if (rc == 0) {
        /* Honour allocation length too. */
        if (s_cdb[4] && s_cdb[4] < len) len = s_cdb[4];
        s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      }
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x1B: /* START_STOP_UNIT — no data */
      s_status = 0x00;
      break;

    case 0x25: /* READ_CAPACITY (Group 1) — 8 bytes data in */
      rc = _scsi_read_capacity(s_target_id, s_cdb, s_cdb_len, &buf, &len);
      if (rc == 0) {
        s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      }
      s_status = scsi_units[s_target_id].status[0];
      break;

    case 0x28: /* READ(10) — data in */
    {
      /* Translate 10-byte CDB to a synthetic 6-byte form for scsi_disk. */
      uint32_t lba = ((uint32_t)s_cdb[2] << 24) | ((uint32_t)s_cdb[3] << 16)
                   | ((uint32_t)s_cdb[4] <<  8) |  (uint32_t)s_cdb[5];
      uint16_t cnt = ((uint16_t)s_cdb[7] <<  8) |  (uint16_t)s_cdb[8];
      unsigned char cdb6[6];
      cdb6[0] = 0x08;
      cdb6[1] = (lba >> 16) & 0x1F;
      cdb6[2] = (lba >> 8)  & 0xFF;
      cdb6[3] =  lba        & 0xFF;
      cdb6[4] = cnt > 255 ? 0 : (uint8_t)cnt;
      cdb6[5] = 0;
      rc = _scsi_read_block(s_target_id, cdb6, 6, &buf, &len);
      if (rc == 0) {
        s_xfer_buf = buf; s_xfer_len = len; s_xfer_dir = 1;
      }
      s_status = scsi_units[s_target_id].status[0];
      break;
    }

    default:
      LOG("UNHANDLED CDB %02X\n", op);
      s_status = 0x02;        /* CHECK CONDITION */
      break;
  }

  /* Decide next phase based on xfer_dir.  req_now=0 because we
     reached here from on_ack_rise(last CDB byte) — host still has
     ACK held.  on_ack_fall() will assert REQ for the new phase. */
  if (s_xfer_dir == 1 && s_xfer_len > 0) {
    enter_data_in(0);
  } else if (s_xfer_dir == 2 && s_xfer_len > 0) {
    enter_data_out(0);
  } else {
    enter_status(0);
  }
}

/* ------------------------------------------------------------------
 * Per-byte handshake — driven by ACK edges
 * ------------------------------------------------------------------ */

/* Per-byte handshake.  Real SCSI:
 *   target asserts REQ → host reads CDR → host asserts ACK
 *   target sees ACK → drops REQ
 *   host sees REQ drop → drops ACK
 *   target sees ACK drop → next byte: drives data + asserts REQ
 *
 * In our model: on_ack_rise drops REQ and possibly transitions phase
 * (req_now=0 in the new phase).  on_ack_fall asserts REQ once the
 * new phase has data ready.
 */

static void on_ack_rise(void)
{
  LOG("ACK rise in phase=%s\n", phase_name(s_phase));
  switch (s_phase) {
    case SCSI3_PH_MSG_OUT:
      s_msg_out = s_init_data;
      /* IDENTIFY messages: bit 7 set, top of byte 0x80 / 0xC0 etc.
         LUN is in bits 0..2 (per SunOS scsi/generic/message.h:88).
         For non-IDENTIFY messages we leave s_lun alone (CDB byte 1
         will be the fallback path). */
      if (s_msg_out & 0x80)
        s_lun = s_msg_out & 0x07;
      LOG("MSG_OUT byte=%02X (lun=%d)\n", s_msg_out, s_lun);
      s_target_req = 0;
      enter_command(0);
      break;

    case SCSI3_PH_COMMAND:
      s_cdb[s_cdb_pos++] = s_init_data;
      if (s_cdb_pos == 1) {
        s_cdb_len = cdb_length_for(s_init_data);
      }
      s_target_req = 0;
      if (s_cdb_pos >= s_cdb_len) {
        dispatch_cdb();           /* enters STATUS / DATA_IN / DATA_OUT with req_now=0 */
      }
      break;

    case SCSI3_PH_DATA_OUT:
      if (s_xfer_buf && s_xfer_pos < s_xfer_len) {
        s_xfer_buf[s_xfer_pos++] = s_init_data;
      }
      s_target_req = 0;
      if (s_xfer_pos >= s_xfer_len) {
        if (s_cdb[0] == 0x0A || s_cdb[0] == 0x2A) {
          _scsi_write_block_end(s_target_id, s_cdb, s_cdb_len, s_xfer_buf, s_xfer_len);
          s_status = scsi_units[s_target_id].status[0];
        }
        enter_status(0);
      }
      break;

    case SCSI3_PH_DATA_IN:
      s_xfer_pos++;
      s_target_req = 0;
      if (s_xfer_pos >= s_xfer_len) {
        enter_status(0);
      }
      break;

    case SCSI3_PH_STATUS:
      s_target_req = 0;
      enter_msg_in(0);
      break;

    case SCSI3_PH_MSG_IN:
      s_target_req = 0;
      go_bus_free();
      break;

    default:
      break;
  }
}

static void on_ack_fall(void)
{
  /* Initiator released ACK; target re-asserts REQ if there's another
     byte to transfer in the current phase. */
  switch (s_phase) {
    case SCSI3_PH_COMMAND:
      if (s_cdb_pos < s_cdb_len) s_target_req = 1;
      break;
    case SCSI3_PH_MSG_OUT:
      /* MSG_OUT is single-byte for IDENTIFY; on_ack_rise already
         transitioned us to COMMAND, so we won't reach here. */
      break;
    case SCSI3_PH_DATA_OUT:
      if (s_xfer_pos < s_xfer_len) s_target_req = 1;
      break;
    case SCSI3_PH_DATA_IN:
      if (s_xfer_pos < s_xfer_len) {
        stage_data_in_byte();
        s_target_req = 1;
      }
      break;
    case SCSI3_PH_STATUS:
      s_target_req = 1;     /* status byte already staged in enter_status */
      break;
    case SCSI3_PH_MSG_IN:
      s_target_req = 1;     /* msg-in byte already staged */
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void scsi3_init(void)
{
  go_bus_free();
  s_init_bsy = s_init_sel = s_init_ack = s_init_atn = s_init_rst = 0;
  s_init_sel_prev = s_init_ack_prev = s_init_rst_prev = 0;
  s_init_data = 0;
  s_bus_data  = 0;
  s_lun       = 0;
  s_msg_out   = 0;
}

void scsi3_set_init_lines(int bsy, int sel, int ack, int atn, int rst)
{
  int prev_sel = s_init_sel, prev_ack = s_init_ack, prev_rst = s_init_rst;

  s_init_bsy = bsy;
  s_init_sel = sel;
  s_init_ack = ack;
  s_init_atn = atn;
  s_init_rst = rst;

  /* RST rising — bus reset.  Goes to BUS_FREE, drops everything. */
  if (rst && !prev_rst) {
    LOG("RST asserted -> BUS_FREE\n");
    go_bus_free();
    return;
  }

  /* SEL rising — peek target id from data lines and assert BSY if
     present.  Real SCSI: target sees its own bit on data while SEL is
     asserted, asserts BSY in response.  Initiator polls cbsr.BSY,
     sees it, then drops SEL.  We must therefore assert target_bsy
     while SEL is still held — not wait for SEL falling. */
  if (sel && !prev_sel) {
    int tid = decode_target_id(s_init_data);
    if (target_present(tid)) {
      s_target_id = tid;
      s_target_bsy = 1;
      LOG("SEL: target %d (BSY asserted)\n", tid);
    } else {
      s_target_id = -1;
      LOG("SEL: target id=%d (data=%02X) not present\n", tid, s_init_data);
    }
  }

  /* SEL falling — target enters MSG_OUT (if ATN) or COMMAND, fresh
     (no ACK held), so REQ asserts immediately. */
  if (!sel && prev_sel) {
    if (s_target_id >= 0) {
      if (s_init_atn) enter_msg_out(1);
      else            enter_command(1);
    } else {
      go_bus_free();
    }
  }

  /* ACK edges (only meaningful while in a phase that uses REQ/ACK). */
  if (ack && !prev_ack) on_ack_rise();
  if (!ack && prev_ack) on_ack_fall();

  s_init_sel_prev = sel;
  s_init_ack_prev = ack;
  s_init_rst_prev = rst;
}

void scsi3_set_init_data(uint8_t data)
{
  s_init_data = data;
  /* In initiator-driven phases the bus-data shadow follows init_data
     so bus reads see the host's drive. */
  if (s_phase == SCSI3_PH_COMMAND ||
      s_phase == SCSI3_PH_MSG_OUT ||
      s_phase == SCSI3_PH_DATA_OUT) {
    s_bus_data = data;
  }
  /* Some hosts (notably the Sun-3 PROM with non-arbitrated selection)
     write ODR _after_ asserting SEL.  Re-decode the target id while
     SEL is held so we don't latch -1 on the SEL rising edge from a
     stale data value.  Assert BSY too (initiator polls for it). */
  if (s_init_sel && s_phase == SCSI3_PH_BUS_FREE && s_target_id < 0) {
    int tid = decode_target_id(data);
    if (target_present(tid)) {
      s_target_id = tid;
      s_target_bsy = 1;
      LOG("late SEL data: target %d (BSY asserted)\n", tid);
    }
  }
}

int     scsi3_target_bsy(void) { return s_target_bsy; }
int     scsi3_target_req(void) { return s_target_req; }
int     scsi3_phase    (void) { return s_phase; }
uint8_t scsi3_bus_data (void) { return s_bus_data; }

int scsi3_dma_in(uint8_t *out_byte)
{
  if (s_phase != SCSI3_PH_DATA_IN || !s_xfer_buf) return 0;
  if (s_xfer_pos >= s_xfer_len)                    return 0;

  *out_byte = s_xfer_buf[s_xfer_pos++];

  if (s_xfer_pos >= s_xfer_len) {
    /* DMA path: ACK semantics don't apply.  Target enters STATUS
       with REQ asserted; host reads the status byte fresh via PIO. */
    enter_status(1);
  }
  return 1;
}

int scsi3_dma_out(uint8_t in_byte)
{
  if (s_phase != SCSI3_PH_DATA_OUT || !s_xfer_buf) return 0;
  if (s_xfer_pos >= s_xfer_len)                     return 0;

  s_xfer_buf[s_xfer_pos++] = in_byte;

  if (s_xfer_pos >= s_xfer_len) {
    if (s_cdb[0] == 0x0A || s_cdb[0] == 0x2A) {
      _scsi_write_block_end(s_target_id, s_cdb, s_cdb_len, s_xfer_buf, s_xfer_len);
      s_status = scsi_units[s_target_id].status[0];
    }
    enter_status(1);
  }
  return 1;
}
