/*
 * scsi3.h -- Sun-3 SCSI bus model + per-target state machine.
 *
 * Used by sun3_si.c (the SI board / NCR 5380 / AM9516 UDC glue layer).
 * Independent of sim/scsi.c which retains the Sun-2 SC-chip path.
 *
 * Design notes — see E:\Dev\Emulators\68k\SUN\SunOS-4.1.3\scsi-emulation-guide.md
 * (kept in memory at reference_scsi_emulation_spec.md).
 *
 * Goals:
 *   - Phase transitions are explicit and per-byte.  No `time_in_state`
 *     ticks.  The host (sun3_si.c) drives initiator lines + ACK
 *     edges; scsi3 advances the target state machine in lockstep.
 *   - PHASE_STATUS / PHASE_MSG_IN data is staged BEFORE REQ is
 *     asserted (per spec § 3.8: target asserts data, then REQ).
 *   - Disk image / INQUIRY / SENSE backing comes from scsi_disk.c —
 *     scsi3.c only handles the bus state and CDB dispatch.
 */
#ifndef SCSI3_H
#define SCSI3_H

#include <stdint.h>

/* SCSI phase encoding per CBSR (MSG | CD | IO) — same layout as the
   real NCR 5380 phase bits.  Match scsi.h's PHASE_* values is *not*
   required: we use these directly, scsi.c's enum is unused here. */
enum scsi3_phase {
  SCSI3_PH_BUS_FREE = -1,    /* nobody owns the bus */
  SCSI3_PH_DATA_OUT = 0,     /* initiator → target (CD=0 IO=0 MSG=0) */
  SCSI3_PH_DATA_IN  = 1,     /* target → initiator (CD=0 IO=1 MSG=0) */
  SCSI3_PH_COMMAND  = 2,     /* initiator → target (CD=1 IO=0 MSG=0) */
  SCSI3_PH_STATUS   = 3,     /* target → initiator (CD=1 IO=1 MSG=0) */
  SCSI3_PH_MSG_OUT  = 6,     /* initiator → target (CD=1 IO=0 MSG=1) */
  SCSI3_PH_MSG_IN   = 7,     /* target → initiator (CD=1 IO=1 MSG=1) */
};

/* Reset the bus model to BUS FREE.  Called at startup. */
void scsi3_init(void);

/* ------------------------------------------------------------------
 * Initiator interface
 * The host (NCR 5380 emulation in sun3_si.c) drives BSY, SEL, ACK, ATN,
 * RST, plus the data byte when in an initiator-driven phase.  scsi3
 * combines those with the target's drives to produce the live bus
 * state.  Caller invokes scsi3_set_init_lines whenever ICR / data
 * changes; scsi3 advances its target state machine on edge events
 * (SEL drop with target_id valid, ACK rise/fall).
 * ------------------------------------------------------------------ */
void scsi3_set_init_lines(int bsy, int sel, int ack, int atn, int rst);
void scsi3_set_init_data (uint8_t data);

/* Live target-driven bus signals.  Read after every change. */
int     scsi3_target_bsy (void);
int     scsi3_target_req (void);
int     scsi3_phase      (void);    /* enum scsi3_phase, -1 for bus free */

/* Live data byte on the bus: target's drive in target→initiator
   phases, initiator's drive in initiator→target phases (mostly used
   to preview status / message bytes before ACK). */
uint8_t scsi3_bus_data   (void);

/* ------------------------------------------------------------------
 * DMA stream interface (used by AM9516 UDC engine in sun3_si.c).
 * Each call moves one byte and advances the target's xfer counter.
 * Returns 1 on success, 0 once the target has left the data phase.
 * ------------------------------------------------------------------ */
int     scsi3_dma_in     (uint8_t *out_byte);
int     scsi3_dma_out    (uint8_t  in_byte);

#endif /* SCSI3_H */
