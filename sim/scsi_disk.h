/*
 * scsi_disk.h -- shared SCSI device backing layer (data side).
 *
 * Pure data layer: parses CDBs, formats INQUIRY / MODE_SENSE /
 * READ_CAPACITY / SENSE responses, reads & writes the backing disk
 * or tape image file.  No bus state, no phase state machine — those
 * live in scsi.c (Sun-2 SC chip path) and scsi3.c (Sun-3 SI / NCR 5380
 * path).  Both bus drivers call into this module after parsing the
 * CDB to build the data payload they will then deliver byte-by-byte.
 */
#ifndef SCSI_DISK_H
#define SCSI_DISK_H

#define MAX_SCSI_BLOCKS 128

struct scsi_unit_s {
  char *fname[16];
  int fd;
  int fileno;
  int eof;
  int filemark;          /* tape: pending filemark to report in REQUEST_SENSE */
  int residual;          /* tape: residual byte count from last short read */
  int tape;
  int ro;
  /* Geometry parsed from the Sun disk label at sector 0. */
  unsigned int disk_cyl;
  unsigned int disk_alt_cyl;
  unsigned int disk_hd;
  unsigned int disk_sec;
  unsigned int disk_sec_size;
  unsigned long long disk_total_blocks;
  char disk_label_text[40];     /* "Sun1.0G cyl 1703 alt 2 hd 15 sec 80" */
  unsigned char status[3];
  unsigned int block_no;
  unsigned char data[512 * MAX_SCSI_BLOCKS];

  /* Pending REQUEST SENSE state for direct-access disks.  Set by the
     bus layer (scsi3.c / scsi.c) when a command returns CHECK
     CONDITION; consumed + cleared by the next REQUEST SENSE.
     Defaults to NO_SENSE / no additional info. */
  unsigned char sense_key;     /* low 4 bits of byte 2 */
  unsigned char sense_asc;     /* byte 12 */
  unsigned char sense_ascq;    /* byte 13 */
};

extern struct scsi_unit_s scsi_units[8];

/* Sense-key constants (scsi/generic/sense.h:85-100). */
#define SCSI_SK_NO_SENSE        0x00
#define SCSI_SK_RECOVERABLE     0x01
#define SCSI_SK_NOT_READY       0x02
#define SCSI_SK_ILLEGAL_REQUEST 0x05
#define SCSI_SK_UNIT_ATTENTION  0x06

/* Common Additional Sense Codes. */
#define SCSI_ASC_NO_ADDITIONAL_INFO       0x00
#define SCSI_ASC_LOGICAL_UNIT_NOT_SUPPORTED 0x25
#define SCSI_ASC_INVALID_OPCODE           0x20

/* Helper: mark a CHECK CONDITION on the unit so the kernel's
   subsequent REQUEST SENSE returns the right key/ASC/ASCQ. */
static inline void scsi_disk_set_sense(int id, unsigned char key,
                                       unsigned char asc, unsigned char ascq)
{
  scsi_units[id].sense_key  = key;
  scsi_units[id].sense_asc  = asc;
  scsi_units[id].sense_ascq = ascq;
  scsi_units[id].status[0]  = 0x02;      /* CHECK CONDITION */
}

/* SCSI command backing.  Each fills *pbuf/*psiz with the response
   payload (for data-in CDBs) or accepts the host buffer (for data-out
   CDBs), and returns 0 on success, non-zero on hard failure.
   Result status byte is staged in scsi_units[id].status[0]. */
int _scsi_test_unit_ready(int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_inquiry        (int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_request_sense  (int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_mode_sense     (int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_mode_select    (int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_read_block     (int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_read_capacity  (int id, unsigned char *cmd, int cmd_size,
                          unsigned char **pbuf, int *psiz);
int _scsi_write_block_start(int id, unsigned char *cmd, int cmd_size,
                            unsigned char **pbuf, int *psiz);
int _scsi_write_block_end  (int id, unsigned char *cmd, int cmd_size,
                            unsigned char *buf, int siz);

/* Tape file slot management. */
int _scsi_set_filenum(int unit, int num);
int _scsi_next_file  (int unit);

/* Image attachment (also declared in scsi.h for callers that don't
   pull in this header). */
int scsi_set_disk_image(int unit, char *fname);
int scsi_set_tape_image(int unit, int fileno, char *fname);

#endif /* SCSI_DISK_H */
