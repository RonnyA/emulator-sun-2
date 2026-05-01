/*
 * sun-2 emulator
 *
 * 10/2014  Brad Parker <brad@heeltoe.com>
 */

/* IRQ connections */
#define IRQ_9513_TIMER1	7
#define IRQ_SCC         6
#define IRQ_9513_TIMER2 5
#define IRQ_SW_INT3     3
#define IRQ_SW_INT2     2
#define IRQ_SC          2
#define IRQ_SW_INT1     1

/* ROM and RAM sizes */
#define MAX_ROM 65536       // 64k ROM (Sun-2 boot ROMs: 32 KB rev-R/Q, 64 KB rev-10F)
#define MAX_RAM 0xffffff    // 16MB of RAM

extern int eprom_size;

/* Auto-type: when set via --type=STRING, the emulator will inject the
   given ASCII characters as keyboard scancodes after the PROM has had
   time to come up.  Used to drive the PROM monitor non-interactively. */
extern const char *g_autotype;
void sun2_autotype_tick(void);

/* Sun-2 hardware mode — selected by --mode= on the command line.
   Drives IDPROM machine type, bwtwo CSR JUMPER_HIRES bit, and SDL window size. */
typedef struct sun2_mode_s {
    const char *name;            /* CLI name */
    unsigned char idprom_machine;/* IDPROM byte 1 */
    int hires_jumper;            /* CSR bit 8: 0 = 1152x900, 1 = 1024x1024 */
    int width;                   /* logical framebuffer width */
    int height;                  /* logical framebuffer height */
    const char *desc;            /* short description for usage/banner */
} sun2_mode_t;

extern const sun2_mode_t *g_mode;
const sun2_mode_t *sun2_mode_lookup(const char *name);
void sun2_mode_print_list(void);
/* Build the IDPROM bytes for a given machine type, recompute byte-15 checksum. */
void idprom_setup(unsigned char machine_type);

void abortf(const char *fmt, ...);
void enable_trace(int);

unsigned int scc_read(unsigned int pa, int size);
void scc_write(unsigned int pa, unsigned int value, int size);
int scc_device_ack(int which);
void scc_update(void);
int scc_in_pop(int ch, unsigned int *pv);
void scc_in_push(int ch, int v);

unsigned int am9513_read(unsigned int pa, int size);
void am9513_write(unsigned int pa, unsigned int value, int size);
int am9513_device_ack(int which);
void am9513_update(void);

unsigned mm58167_read(unsigned pa, int size);
void mm58167_write(unsigned pa, unsigned value, int size);
void mm58167_update(void);

// 3C400 ethernet card functions usable from outsid
void e3c400_enable_trace(int level);
int e3c400_device_ack();
void e3c400_init(void);
void e3c400_update(void);
uint32_t e3c400_read(uint32_t addr, int32_t size);
void e3c400_write(uint32_t addr, uint32_t size, uint32_t value);

unsigned int sc_read(unsigned address, int size);
void sc_write(unsigned int address, int size, unsigned int value);
int sc_device_ack(void);

void sc_set_cmd_reg(unsigned int v);
unsigned int sc_get_data(void);
void sc_reset_odd_len(void);
/* After a SCSI command that did NO DMA transfer, snap DmaCount to
   0xFFFF so the PROM's "sd: short transfer" residue check sees a
   clean transfer.  Mirrors RetroCore SCSIHostAdapter.cs OnLastMessage
   when bytesReceived == 0. */
void sc_dma_complete_no_xfer(void);
void sc_dma_read_data(unsigned char *buf, int bufsiz);
void sc_dma_write_data(unsigned char *buf, int bufsiz);


unsigned int sun2_video_read(unsigned int address, int size);
unsigned int sun2_video_write(unsigned int address, int size, unsigned int value);
unsigned int sun2_kbm_read(unsigned int address, int size);
unsigned int sun2_kbm_write(unsigned int address, int size, unsigned int value);
unsigned int sun2_video_ctl_read(unsigned int address, int size);
unsigned int sun2_video_ctl_write(unsigned int address, int size, unsigned int value);
void sun2_kb_write(int value, int size);

void int_controller_set(unsigned int irq);
void int_controller_clear(unsigned int irq);

int sw_int_ack(int intr);
void sdl_poll(void);
void sun2_init(void);

void m68k_mark_buserr(void);
void m68k_set_buserr(unsigned int pc);

unsigned int cpu_map_address(unsigned int address, unsigned int fc, int m, unsigned int *mtype, unsigned int *pfault, unsigned int *ppte);

void abortf(const char *fmt, ...);

unsigned int cpu_read(int size, unsigned int address);
void cpu_write(int size, unsigned int address, unsigned int value);

