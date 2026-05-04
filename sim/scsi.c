#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "sim.h"
#include "scsi.h"

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

#define MAX_SCSI_BLOCKS 128

struct scsi_unit_s {
  char *fname[16];
  int fd;
  int fileno;
  int eof;
  int filemark;          /* tape: pending filemark to report in next REQUEST_SENSE */
  int residual;          /* tape: residual byte count from last short read */
  int tape;
  int ro;
  unsigned char status[3];
  unsigned int block_no;
  unsigned char data[512*MAX_SCSI_BLOCKS];
} scsi_units[8];

int _scsi_set_filenum(int unit, int num);

int _scsi_test_unit_ready(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int ret;
  off_t offset;
  int xfer_size;

  u = &scsi_units[id];

  if (trace_scsi) printf("_scsi_test_unit_ready(id=%d)\n", id);

  xfer_size = 16;
  memset(&u->data, 0, 512);

  /* TUR returns GOOD when the unit is ready — independent of any prior
     CHECK CONDITION still pending in u->status[0]. */
  u->status[0] = 0x00;

  *pbuf = u->data;
  *psiz = xfer_size;

  return 0;
}

int _scsi_inquiry(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int ret;
  off_t offset;
  int spagecode, sallocationlen, xfer_size;

  u = &scsi_units[id];
  spagecode = (cmd[2] << 8) | cmd[3];
  sallocationlen = (cmd[3] << 8) | cmd[4];

  if (trace_scsi) printf("_scsi_inquiry(id=%d) spagecode %x, sallocationlen %x\n", id, spagecode, sallocationlen);

  xfer_size = 96;
  memset(&u->data, 0, 512);

  /* tape? */
  if (u->tape) {
    if (trace_scsi) printf("_scsi_inquiry() tape\n");
    /* INQUIRY response (matches RetroCore SCSIDevicePresets.EmulexMT02
       fix in commit 2c65ba52 "Sun-2: SCSI tape boot fixes").  Without
       proper SCSI-1/CCS identification, the Sun-2 PROM takes a wrong
       boot path that ends in an MMU fault during PROM bcopy. */
    u->data[0] = 0x01;        /* peripheral device type: sequential access (tape) */
    u->data[1] = 0x80;        /* RMB=1 (removable media), device-type qualifier 0 */
    u->data[2] = 0x01;        /* ANSI-approved version: SCSI-1 (X3.131-1986) */
    u->data[3] = 0x01;        /* response data format: SCSI-1 / CCS */
    u->data[4] = 91;          /* additional length (95 - 4) */
    /* vendor / product / revision strings — left zero-padded, sufficient
       for PROM identification purposes (RetroCore Vendor=Product="" works). */
  }

  *pbuf = u->data;
  *psiz = xfer_size;

  return 0;
}

int _scsi_mode_select(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int xfer_size;

  u = &scsi_units[id];

  /* MODE_SELECT(6): parameter list length is in CDB[4].  Falling back
     to a fixed 8 bytes leaves the controller's DMA count short of what
     the PROM (or standalone) requested — the host then sees a residual
     and prints "short transfer" even though we accepted the parameters. */
  xfer_size = cmd[4];
  if (xfer_size == 0) xfer_size = 8;
  if (xfer_size > (int)sizeof(u->data)) xfer_size = sizeof(u->data);
  memset(&u->data, 0, 512);

  /* tape? */
  if (u->tape) {
    if (trace_scsi) printf("_scsi_mode_select() tape len=%d\n", xfer_size);
  }

  *pbuf = u->data;
  *psiz = xfer_size;

  return 0;
}

int _scsi_mode_sense(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int ret;
  off_t offset;
  int pc, pagecode, spagecode, allocationlen, xfer_size;

  u = &scsi_units[id];
  pc = (cmd[2] & 0xc0) >> 6;
  pagecode = cmd[2] & 0x3f;
  spagecode = cmd[3];
  allocationlen = cmd[4];

  if (trace_scsi) printf("_scsi_mode_sense(id=%d) pc %x pagecode %x spagecode %x, allocationlen %x\n", id, pc, pagecode, spagecode, allocationlen);

  xfer_size = 96;
  memset(&u->data, 0, 512);

  // data len
  // medium type
  // dev spec
  // blk desc len
  // density
  // nblocks[3];
  // reserved
  // blklen[3]

  /* tape? */
  if (u->tape) {
    if (trace_scsi) printf("_scsi_mode_sense() tape\n");
//    u->data[0] = 0x01;
  }

  *pbuf = u->data;
  *psiz = xfer_size;

  return 0;
}

int _scsi_read_block(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int fd, ret;
  off_t offset;
  int sblock, scount, xfer_size;

  u = &scsi_units[id];
  sblock = ((cmd[1] & 0x1f) << 16) | (cmd[2] << 8) | cmd[3];
  scount = cmd[4];

  if (trace_scsi)
    if (trace_scsi) printf("_scsi_read_block(id=%d) sblock 0x%x, slen 0x%x\n", id, sblock, scount);

  if (scount > MAX_SCSI_BLOCKS) {
    abortf("scsi%d: buffer size exceeded\n", id);
  }

  if (u->tape) {
    sblock = u->block_no;
    /* Read at file end -> FILEMARK (RetroCore SCSITape.cs
       continue_handling_read_6 line 921, FILEMARK case): 0 data
       delivered, CHECK CONDITION, FILEMARK with residual = full
       request size.  Auto-advance to the next tape file so the next
       READ (after host's REQUEST_SENSE consumes the FILEMARK) reads
       the next file naturally.  Sun-2 PROM tpboot doesn't issue
       SPACE between files of a multi-file boot. */
    if (u->eof && u->fd > 0) {
      off_t end = lseek(u->fd, 0, SEEK_END);
      if (end == u->block_no * 512) {
        /* Read at file end -> CHECK CONDITION + FILEMARK in sense.
           DELIVER FULL xfer_size of zeros so the controller's DMA count
           ticks all the way and the host sees NO residual.  tpboot's
           "st: short transfer" check (file 1 0xa3c10..0xa3c2c) only
           triggers on bytes_transferred < requested.  By reporting full
           transfer + CHECK CONDITION, the standalone goes through
           REQUEST_SENSE, sees FILEMARK, and handles EOF silently
           (matches RetroCore SunSCSIController behavior). */
        memset(u->data, 0, 512 * scount);
        u->status[0] = 0x02;
        u->filemark = 1;
        u->residual = 0;            /* no residual = no short-transfer print */
        *pbuf = u->data;
        *psiz = 512 * scount;       /* tick full DMA count */
        if (trace_scsi) printf("scsi%d: tape READ at file end -> CHECK COND + FILEMARK (full DMA, no residual)\n", id);
        return 0;
      }
    }
  }

  offset = sblock * 512;
  xfer_size = 512*scount;

//  if (u->fd <= 0)
//    return -1;

  fd = u->fd;

  if (!u->tape)
    u->block_no = sblock;

  *pbuf = u->data;
  *psiz = xfer_size;

  if (trace_scsi)
    printf("scsi%d: read xfer; %s file %d '%s' fd=%d block=%d blocks=%d bytes=%d offset=%lld\n",
           id, u->tape ? "TAPE" : "DISK", u->fileno,
           (u->fileno >= 0 && u->fname[u->fileno]) ? u->fname[u->fileno] : "(null)",
           u->fd, sblock, scount, xfer_size, (long long)offset);

  if (fd > 0) {
    off_t lr = lseek(fd, offset, SEEK_SET);
    if (lr < 0)
      printf("scsi%d: lseek(%lld) failed: %s\n", id, (long long)offset, strerror(errno));
    ret = read(fd, u->data, xfer_size);
    if (ret < 0) {
      printf("scsi%d: read FAILED fd=%d bytes=%d: %s\n", id, fd, xfer_size, strerror(errno));
      perror( u->fname[ u->fileno ] );
      abortf("scsi read failed\n");
    }
    if (trace_scsi) {
      off_t pos = lseek(fd, 0, SEEK_CUR);
      printf("scsi%d: read got %d/%d bytes; new fpos=%lld\n",
             id, ret, xfer_size, (long long)pos);
    }
  } else {
    if (trace_scsi)
      printf("scsi%d: read with NO fd (fd=%d); zero-filling %d bytes\n", id, u->fd, xfer_size);
    memset(u->data, 0, xfer_size);
  }

  u->block_no += ret/512;

  if (trace_scsi) printf("scsi%d: read xfer; final bytes=%d\n", id, *psiz);

  if (ret < xfer_size) {
    u->eof = 1;
    if (u->tape) {
      /* Tape underlength read (RetroCore SCSITape.cs
         continue_handling_read_6, "under" branch line 887-901):
         deliver the actual bytes (NOT zero-padded), report CHECK
         CONDITION with ILI bit + residual = (requested - actual).
         PROM/standalone reads the residual via the controller's DMA
         count register, sees a short transfer, prints its EOF message
         ("st: sense error" or "st: short transfer") and stops reading
         the file.  Zero-padding to GOOD hid the residual and made the
         loaded program either keep reading garbage or trip the next
         FILEMARK path with a bogus residual. */
      /* Tape underlength read at file end: zero-pad to full requested
         size + CHECK CONDITION + FILEMARK in sense + NO residual.
         tpboot's "st: short transfer" check (file 1 0xa3c10..0xa3c2c)
         fires when bytes_transferred < requested — we avoid that by
         delivering full DMA count, then signal EOF via CHECK CONDITION
         status which routes the host to REQUEST_SENSE / FILEMARK silent
         handling.  Matches RetroCore SunSCSIController's residual
         management. */
      memset(u->data + ret, 0, xfer_size - ret);
      *psiz = xfer_size;            /* tick full DMA count */
      u->status[0] = 0x02;          /* CHECK CONDITION */
      u->filemark = 1;
      u->residual = 0;              /* no residual = no print */
      if (trace_scsi) printf("scsi%d: tape short read; got %d/%d bytes; CHECK COND FILEMARK (full DMA, no residual)\n",
             id, ret, xfer_size);
    } else {
      u->status[0] = 0x02;
      if (trace_scsi)
        printf("scsi%d: short — got %d, wanted %d; eof=1 status=0x02 [disk]\n",
               id, ret, xfer_size);
      *psiz = ret;
    }
  }

  return 0;
}

int _scsi_write_block_start(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int fd, ret;
  int sblock, scount, xfer_size;

  u = &scsi_units[id];
  sblock = ((cmd[1] & 0x1f) << 16) | (cmd[2] << 8) | cmd[3];
  scount = cmd[4];

  if (trace_scsi)
    printf("scsi%d: _scsi_write_block_start() sblock 0x%x, slen 0x%x\n", id, sblock, scount);

  if (scount > MAX_SCSI_BLOCKS) {
    abortf("scsi%d: buffer size exceeded\n", id);
  }

  if (u->tape)
    sblock = u->block_no;

  xfer_size = 512*scount;

  if (u->fd <= 0) {
    printf("scsi%d: no fd open for device?\n", id);
    return -1;
  }

  fd = u->fd;

  if (!u->tape)
    u->block_no = sblock;

  *pbuf = u->data;
  *psiz = xfer_size;

  return 0;
}

int _scsi_write_block_end(int id, unsigned char *cmd, int cmd_size, unsigned char *buf, int siz)
{
  struct scsi_unit_s *u;
  int fd, ret;
  off_t offset;
  int sblock, scount, xfer_size;

  u = &scsi_units[id];
  scount = cmd[4];
  sblock = u->block_no;
  offset = sblock * 512;
  xfer_size = siz;

  if (trace_scsi) {
    printf("scsi%d: _scsi_write_block_end; write xfer; block=%d, blocks=%d, bytes=%d, fd=%d\n",
	   id, sblock, scount, xfer_size, u->fd);
  }

  fd = u->fd;

  lseek(fd, offset, SEEK_SET);
#if 1
  ret = write(fd, u->data, xfer_size);
  if (ret < 0) {
    perror( u->fname[ u->fileno ] );
    abortf("scsi write failed\n");
  }
#else
  ret = xfer_size;
#endif

  u->block_no += ret/512;
  u->eof = 0;
  u->status[0] = 0x00;

  return 0;
}

int _scsi_request_sense(int id, unsigned char *cmd, int cmd_size, unsigned char **pbuf, int *psiz)
{
  struct scsi_unit_s *u;
  int ret;
  off_t offset;
  int spagecode, sallocationlen, xfer_size;

  if (trace_scsi) printf("_scsi_request_sense()\n");

  u = &scsi_units[id];
  xfer_size = 16;
  memset(&scsi_units[id].data, 0, 512);

  /* tape? */
  if (u->tape) {
    /* Match RetroCore SCSIFullDevice.cs:set_sense_data byte-for-byte.
       Format: SCSI-2 fixed-format extended sense (response code 0x70+).
         byte 0: (data ? (valid?0x80:0) : 0) | (deferred ? 0x71 : 0x70).
                 RetroCore: 0xF0 when data is provided.
         byte 1: 0
         byte 2: FILEMARK(0x80) | EOM(0x40) | ILI(0x20) | sense_key (low 4)
         bytes 3..6: signed BE int32 — info (residual)
         byte 7: 10  (additional sense length)
         bytes 8..11: 0
         bytes 12..13: ASC/ASCQ (BE u16 — SCSIAdditionalSenseCodes value)
                       SKC_FILEMARK_DETECTED = 0x0001
                       SKC_NO_ADDITIONAL     = 0x0000
         bytes 14..17: 0 */
    int info = u->residual;
    /* Zero-fill all 18 bytes first (RetroCore line 667-670). */
    memset(u->data, 0, 18);
    u->data[0] = 0xF0;           /* valid + response code 0x70 */
    /* byte 2: condition bits OR'd with sense key (NO_SENSE = 0). */
    unsigned char b2 = 0;
    if (u->filemark) b2 |= 0x80;
    else if (u->residual) b2 |= 0x20;   /* ILI (underlength record) */
    u->data[2] = b2 | 0x00;             /* sense key NO_SENSE */
    /* info field, big-endian s32. */
    u->data[3] = (info >> 24) & 0xff;
    u->data[4] = (info >> 16) & 0xff;
    u->data[5] = (info >>  8) & 0xff;
    u->data[6] = (info >>  0) & 0xff;
    u->data[7] = 10;             /* additional sense length */
    /* bytes 8..11 already zero. */
    /* ASC/ASCQ at bytes 12..13. */
    if (u->filemark) {
      u->data[12] = 0x00;
      u->data[13] = 0x01;        /* SKC_FILEMARK_DETECTED */
    } else {
      u->data[12] = 0x00;
      u->data[13] = 0x00;        /* SKC_NO_ADDITIONAL_SENSE_INFORMATION */
    }
    /* Respect REQUEST SENSE allocation length (CDB byte 4).
       Allocation length 0 means "default" (most implementations use 4).
       We always have 18 bytes of valid sense; deliver up to whatever the
       host asked for. */
    int alloc_len = cmd[4];
    if (alloc_len == 0) alloc_len = 4;     /* SCSI-1 default */
    if (alloc_len > 18) alloc_len = 18;
    xfer_size = alloc_len;
    int delivered_filemark = u->filemark;
    if (trace_scsi) printf("scsi%d: REQUEST_SENSE [tape] file=%d eof=%d filemark=%d residual=%d alloc=%d -> sense %02x %02x %02x .info=%d ascq=%02x\n",
           id, u->fileno, u->eof, u->filemark, u->residual, alloc_len,
           u->data[0], u->data[1], u->data[2], info, u->data[13]);
    /* SCSI: REQUEST SENSE clears the pending sense after delivery. */
    u->filemark = 0;
    u->residual = 0;
    u->eof = 0;
    u->status[0] = 0x00;        /* GOOD */

    /* Auto-advance to next tape file once host has consumed the
       FILEMARK sense.  Sun-2 PROM tpboot doesn't issue SPACE FILEMARK
       between files; without this, next READ short-reads 0 bytes from
       the same already-EOF file → infinite FILEMARK loop. */
    if (delivered_filemark) {
      if (u->fname[u->fileno + 1]) {
        int next = u->fileno + 1;
        if (trace_scsi) printf("scsi%d: post-FILEMARK -> advance file %d -> %d ('%s')\n",
               id, u->fileno, next, u->fname[next]);
        _scsi_set_filenum(id, next);
      } else {
        if (trace_scsi) printf("scsi%d: post-FILEMARK and no more files\n", id);
      }
    }
  }

  *pbuf = u->data;
  *psiz = xfer_size;

  if (trace_scsi) printf("scsi%d: sense data (%d) %02x %02x %02x %02x\n",
			 id, xfer_size, u->data[0], u->data[1], u->data[2], u->data[3]);

  return 0;
}


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

  if ((scsi_bus_state & (SCSI_BUS_BSY | SCSI_BUS_MSG | SCSI_BUS_CD | SCSI_BUS_REQ)) == 
      (SCSI_BUS_BSY | SCSI_BUS_REQ))
    {
printf("scsi: clear REQ\n");
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
      scsi_bus_state |= SCSI_BUS_BSY;
      _scsi_set_phase(PHASE_COMMAND, 1);
      if (trace_scsi > 1) printf("scsi: PHASE_SELECTION, no SEL -> COMMAND\n");
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
	    } else {
	      scsi_units[id_selected].status[0] = 0x02;
	    }
	    _scsi_set_phase(PHASE_STATUS, 0);
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
    printf("scsi: MESSAGE_OUT\n");
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

int _scsi_set_filenum(int unit, int num)
{
  int fd;
  char *fname;
  struct scsi_unit_s *u;

  u = &scsi_units[unit];

  if (trace_scsi)
    printf("scsi%d: set file %d '%s' (was %d)%s\n",
           unit, num, u->fname[num] ? u->fname[num] : "(null)",
           u->fileno, u->tape ? " [tape]" : "");

  /* if open, close active file */
  if (u->fd > 0) {
    if (trace_scsi)
      printf("scsi%d: close fd=%d (file %d '%s')\n",
             unit, u->fd, u->fileno,
             (u->fileno >= 0 && u->fname[u->fileno]) ? u->fname[u->fileno] : "(null)");
    if (close(u->fd) < 0)
      printf("scsi%d: close failed: %s\n", unit, strerror(errno));
    u->fd = 0;
  }

  fname = u->fname[num];
  if (fname == NULL) {
    /* SunOS asked us to seek to a file slot we never configured (e.g.
       no tape mounted, or the boot loader iterated past the last tape
       file).  Return failure quietly -- perror() with a NULL fname is
       UB and used to print "perror((null)): Invalid argument" once
       per such request. */
    return -1;
  }
  fd = open(fname, (u->ro ? O_RDONLY : O_RDWR) | O_BINARY);
  if (fd < 0) {
    printf("scsi%d: open('%s', %s) FAILED: %s\n",
           unit, fname, u->ro ? "O_RDONLY" : "O_RDWR", strerror(errno));
    perror(fname);
    return -1;
  }

  if (trace_scsi) {
    struct stat st;
    if (fstat(fd, &st) == 0)
      printf("scsi%d: open('%s', %s) OK fd=%d size=%lld%s\n",
             unit, fname, u->ro ? "O_RDONLY" : "O_RDWR", fd,
             (long long)st.st_size, u->tape ? " [tape]" : "");
    else
      printf("scsi%d: open('%s') OK fd=%d (fstat failed: %s)\n",
             unit, fname, fd, strerror(errno));
  }

  u->fd = fd;
  u->fileno = num;
  u->eof = 0;
  u->block_no = 0;

  return 0;
}

int _scsi_next_file(int unit)
{
  int current;
  struct scsi_unit_s *u = &scsi_units[unit];

  current = u->fileno;
  if (u->fname[current+1]) {
    if (trace_scsi)
      printf("scsi%d: next-file %d -> %d ('%s')\n",
             unit, current, current+1, u->fname[current+1]);
    _scsi_set_filenum(unit, current+1);
  } else {
    if (trace_scsi)
      printf("scsi%d: next-file %d -> NONE (end of media)\n", unit, current);
  }

  return 0;
}


int scsi_set_disk_image(int unit, char *fname)
{
  scsi_units[unit].fname[0] = strdup(fname);
  scsi_units[unit].fd = 0;
  scsi_units[unit].fileno = -1;
  scsi_units[unit].tape = 0;
  scsi_units[unit].ro = 0;

  /* */
//  trace_scsi = 1;

  return _scsi_set_filenum(unit, 0);
}

int scsi_set_tape_image(int unit, int fileno, char *fname)
{
  struct stat st;
  if (stat(fname, &st) == 0)
    printf("scsi%d: tape register file %d '%s' size=%lld\n",
           unit, fileno, fname, (long long)st.st_size);
  else
    printf("scsi%d: tape register file %d '%s' (stat failed: %s)\n",
           unit, fileno, fname, strerror(errno));

  scsi_units[unit].fname[fileno] = strdup(fname);
  scsi_units[unit].fd = 0;
  scsi_units[unit].fileno = -1;
  scsi_units[unit].tape = 1;
  scsi_units[unit].ro = 1;

//trace_scsi = 1;
//trace_sc = 1;

  return _scsi_set_filenum(unit, 0);
}

/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
