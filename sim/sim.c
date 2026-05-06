/*
 * sun-2 emulator
 *
 * 10/2014  Brad Parker <brad@heeltoe.com>
 *
 * Copyright (C) 2014-2019 Brad Parker <brad@heeltoe.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

/* htonl/htons live in winsock2.h on Windows and arpa/inet.h on POSIX. */
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif

#include "sim.h"
#include "net.h"
#include "scc_tcp.h"
#include "machine.h"
#include "sun3.h"

const char *g_net_iface = NULL;
int g_net_dump = 0;
int g_scc_tcp_port = 0;   /* 0 = disabled; otherwise listen on this port */
int g_no_kbd = 0;         /* --no-kbd: pretend no keyboard, so PROM uses ttya */
int g_scc_boards = 0;     /* --scc-boards=N: number of MULTIBUS expansion SCCs
                             (zs2..zs5).  Default 0 = on-board zs0 + zs1 only.
                             1 = +zs2 (ttye/f); 2 = +zs3 (ttyg/h); 3 = +zs4
                             (ttyi/j); 4 = +zs5 (ttyk/l). */

#include "scsi.h"

extern void sim68k(void);

char boot_filename[1024];
char kernel_filename[1024];
char eprom_filename[1024];
char disk_filename[1024];
char tape_filename[1024];
int quiet;

struct a_out_hdr_s {
  unsigned long a_info;	/* Use macros N_MAGIC, etc for access.  */
  unsigned int a_text;	/* Length of text, in bytes.  */
  unsigned int a_data;	/* Length of data, in bytes.  */
  unsigned int a_bss;	/* Length of uninitialized data area for file, in bytes.  */
  unsigned int a_syms;	/* Length of symbol table data in file, in bytes.  */
  unsigned int a_entry;	/* Start address.  */
  unsigned int a_trsize;/* Length of relocation info for text, in bytes.  */
  unsigned int a_drsize;/* Length of relocation info for data, in bytes.  */
};

int read_aout(int fd)
{
	struct a_out_hdr_s h;
	int r;

	r = read(fd, (char *)&h, sizeof(h));
	if (r != sizeof(h)) {
		return -1;
	}

	h.a_info = htonl(h.a_info);
	h.a_text = htonl(h.a_text);
	h.a_data = htonl(h.a_data);
	h.a_bss = htonl(h.a_bss);
	h.a_syms = htonl(h.a_syms);
	h.a_entry = htonl(h.a_entry);
	h.a_trsize = htonl(h.a_trsize);
	h.a_drsize = htonl(h.a_drsize);

	printf("a_info  %08lx\n", h.a_info);
	printf("a_text  %08x\n", h.a_text);
	printf("a_data  %08x\n", h.a_data);
	printf("a_bss   %08x\n", h.a_bss);
	printf("a_syms  %08x\n", h.a_syms);
	printf("a_entry  %08x\n", h.a_entry);
	printf("a_trsize %08x\n", h.a_trsize);
	printf("a_drsize %08x\n", h.a_drsize);

	return 0;
}

int
read_kernel(void)
{
	int fd;

	fd = open(kernel_filename, O_RDONLY | O_BINARY);
	if (fd < 0) {
		perror(kernel_filename);
		exit(1);
	}
	read_aout(fd);
	close(fd);
	return 0;
}

int eprom_size;

int
read_binary(int fd, int addr)
{
	extern unsigned char g_rom[];
	eprom_size = read(fd, g_rom, MAX_ROM);
	if (eprom_size > 0)
		printf("eprom: loaded %d bytes\n", eprom_size);
	return eprom_size;
}

int
read_eprom(void)
{
	int fd;

	fd = open(eprom_filename, O_RDONLY | O_BINARY);
	if (fd < 0) {
		perror(eprom_filename);
		exit(1);
	}
	read_binary(fd, 0x00fe0000);
	close(fd);
	return 0;
}

char boot_filename[1024];
char kernel_filename[1024];

int
setup_kernel(char *kf, char *bf)
{
  strcpy(kernel_filename, kf);
  strcpy(boot_filename, bf);
  return 0;
}

int
setup_eeprom(char *ef)
{
  strcpy(eprom_filename, ef);
  if (!quiet) printf("eprom: %s\n", eprom_filename);
  read_eprom();
  return 0;
}

int
setup_disk(char *df)
{
  strcpy(disk_filename, df);
  if (!quiet) printf("disk: %s\n", disk_filename);
  if (scsi_set_disk_image(0, disk_filename))
    return -1;
  return 0;
}

int
tapesort_compare(const void *a, const void *b)
{
  return strcmp(* (char * const *)a, * (char * const *)b);
}

int
setup_tape(char *tf)
{
  struct dirent *d;
  DIR *dir;
  int i, n;
  char *tapefilename[32];
  char filename[1024];

  strcpy(tape_filename, tf);

  /* they need to be sorted... */
  dir = opendir(tape_filename);
  n = 0;

  if (dir) {
    while ((d = readdir(dir))) {
      /* Some libc/dirent implementations (e.g. older w64devkit MinGW)
         don't expose d_type at all; use stat() unconditionally. */
      char probe[1024];
      struct stat st;
      snprintf(probe, sizeof(probe), "%s/%s", tape_filename, d->d_name);
      if (stat(probe, &st) != 0) continue;
      if (!S_ISREG(st.st_mode)
#ifdef S_ISLNK
          && !S_ISLNK(st.st_mode)
#endif
         ) continue;
      /* only take tape files named "xx" where "xx" is two digits */
      if (strlen(d->d_name) == 2 && isdigit(d->d_name[0])) {
        tapefilename[n++] = strdup(d->d_name);
      }
    }

    closedir(dir);
    qsort(tapefilename, n, sizeof(char *), tapesort_compare);
  }

  for (i = 0; i < n; i++) {
    sprintf(filename, "%s/%s", tape_filename, tapefilename[i]);
    if (!quiet) printf("tape: file%d %s\n", i, filename);
    if (scsi_set_tape_image(4, i, filename)) {
      printf("tape: can't setup tape image %s\n", filename);
      return -1;
    }
  }

/*
	scsi_set_tape_image(4, 0, "tape/01");
	scsi_set_tape_image(4, 1, "tape/02");
	scsi_set_tape_image(4, 2, "tape/03");
	scsi_set_tape_image(4, 3, "tape/04");
	scsi_set_tape_image(4, 4, "tape/05");
	scsi_set_tape_image(4, 5, "tape/06");
	scsi_set_tape_image(4, 6, "tape/07");
	scsi_set_tape_image(4, 7, "tape/08");
	scsi_set_tape_image(4, 8, "tape/09");
	scsi_set_tape_image(4, 9, "tape/10");
*/

  return 0;
}

/* Mode table.  Each entry pins down the machine identity (IDPROM byte 1),
   the bwtwo CSR JUMPER_HIRES jumper (Sun-2 only), the corresponding logical
   FB size, and the per-machine bus/MMU vtable. */
static const sun2_mode_t sun2_modes[] = {
    /* name        idprom hi w     h     description                                                  ops */
    {"2/120",      0x01,   0, 1152, 900,  "Sun-2/120 Multibus, standard 1152x900 monitor",            &sun2_ops},
    {"2/120-hi",   0x01,   1, 1024, 1024, "Sun-2/120 Multibus, hi-res jumper (1024x1024)",            &sun2_ops},
    {"2/50",       0x02,   0, 1152, 900,  "Sun-2/50 VME, standard 1152x900 monitor",                  &sun2_ops},
    {"3/60",       0x17,   0, 1152, 900,  "Sun-3/60 desktop, MC68020 + Sun MMU (PROM banner over telnet)", &sun3_ops},
};
static const int n_sun2_modes = sizeof(sun2_modes) / sizeof(sun2_modes[0]);

const sun2_mode_t *g_mode = &sun2_modes[0];   /* default: 2/120 1152x900 */
const char *g_autotype;

const sun2_mode_t *sun2_mode_lookup(const char *name)
{
    for (int i = 0; i < n_sun2_modes; i++) {
        if (strcmp(sun2_modes[i].name, name) == 0)
            return &sun2_modes[i];
    }
    return NULL;
}

void sun2_mode_print_list(void)
{
    for (int i = 0; i < n_sun2_modes; i++) {
        fprintf(stderr, "    %-10s  idprom=0x%02x hires=%d  %dx%d  %s\n",
                sun2_modes[i].name, sun2_modes[i].idprom_machine,
                sun2_modes[i].hires_jumper, sun2_modes[i].width, sun2_modes[i].height,
                sun2_modes[i].desc);
    }
}

void usage(void)
{
  fprintf(stderr, "sun-2 emulator (network backend: %s)\n", net_backend_name());
  fprintf(stderr, "usage:\n");
  fprintf(stderr, " --prom=FILE\n");
  fprintf(stderr, " --disk=FILE\n");
  fprintf(stderr, " --tape=DIR\n");
  fprintf(stderr, " --mode=NAME    (default: %s)\n", sun2_modes[0].name);
  sun2_mode_print_list();
  fprintf(stderr, "optionally:\n");
  fprintf(stderr, " --kernel=FILE  --boot=FILE\n");
  fprintf(stderr, " --auto-abort       send L1-A on first PROM bell-off (drops to monitor)\n");
  fprintf(stderr, " --trace-ring=N     keep last N MMU traces; dump on bus error\n");
  fprintf(stderr, " --trace-ring-addr=ADDR  only dump trace ring for bus errors at ADDR (0=any)\n");
  fprintf(stderr, " --net-iface=NAME|N bind the 3C400 to this host interface.\n");
  fprintf(stderr, "                    NAME is a literal name (e.g. eth0 or a Windows\n");
  fprintf(stderr, "                    \\Device\\NPF_{...} GUID).  N is an index into\n");
  fprintf(stderr, "                    --net-list (1-based).\n");
  fprintf(stderr, "                    (env: SUN2_NET_IFACE; default: backend auto-picks)\n");
  fprintf(stderr, " --net-list         list available host interfaces and exit\n");
  fprintf(stderr, " --net-dump         dump every 3C400 RX/TX frame to stderr\n");
  fprintf(stderr, " --scc-tcp[=PORT]   expose SCC tty consoles on TCP (default port 9900).\n");
  fprintf(stderr, "                    connect with: telnet host PORT   (or: nc host PORT)\n");
  fprintf(stderr, "                    On connect a menu lists configured ttys; pick one.\n");
  fprintf(stderr, " --scc-boards=N     attach N Multibus SCC expansion cards (0..4, default 0).\n");
  fprintf(stderr, "                    Each card adds 2 ttys: 1=>ttye/f, 2=>+ttyg/h, 3=>+ttyi/j,\n");
  fprintf(stderr, "                    4=>+ttyk/l.  SunOS GENERIC has zs2..zs5 compiled in.\n");
  fprintf(stderr, "                    NOTE: the kbd/mouse chip is zs1 (Sun-2 conventions);\n");
  fprintf(stderr, "                    there is no ttyc/ttyd on a Sun-2.\n");
  fprintf(stderr, " --no-kbd           pretend no keyboard is attached; the PROM falls back\n");
  fprintf(stderr, "                    to ttya as console.  Useful with --scc-tcp.\n");
  fprintf(stderr, " -q                 quiet (suppress bus-error/vector trace)\n");
  exit(1);
}

extern char *optarg;

char *prom_arg;
char *disk_arg;
char *tape_arg;

char *boot_arg;
char *kernel_arg;

//#ifdef __APPLE__
//int SDL_main(int argc, char **argv)
//#else
int main(int argc, char **argv)
//#endif
{
  int c;
  int digit_optind = 0;

  /* Disable stdio buffering so trace output reaches the terminal (or
     a tee/redirect) immediately.  MSVC/MinGW's CRT treats _IOLBF as
     _IOFBF for non-tty handles, so even line-buffered stdout would
     hide startup banners and perror() messages until the process
     exits — which made it look like the SCSI disk wasn't opening
     when in fact the printfs were just stuck in the FILE buffer. */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  if (argc <= 1) {
    usage();
  }

  while (1) {
    int this_option_optind = optind ? optind : 1;
    int option_index = 0;
    static struct option long_options[] = {
      {"prom",            optional_argument, 0,  'p' },
      {"disk",            optional_argument, 0,  'd' },
      {"tape",            optional_argument, 0,  't' },
      {"kernel",          optional_argument, 0,  'k' },
      {"boot",            optional_argument, 0,  'b' },
      {"mode",            required_argument, 0,  'm' },
      {"type",            required_argument, 0,  'T' },
      {"auto-abort",      no_argument,       0,  'A' },
      {"trace-ring",      required_argument, 0,  'R' },
      {"trace-ring-addr", required_argument, 0,  'X' },
      {"net-iface",       required_argument, 0,  'i' },
      {"net-list",        no_argument,       0,  'L' },
      {"net-dump",        no_argument,       0,  'D' },
      {"scc-tcp",         optional_argument, 0,  'S' },
      {"scc-boards",      required_argument, 0,  'B' },
      {"no-kbd",          no_argument,       0,  'K' },
      {"trace-si",        no_argument,       0,  'I' },
      {"trace-scsi",      no_argument,       0,  'C' },
      {0,                 0,                 0,   0  }
    };

    c = getopt_long(argc, argv, "d:k:p:t:m:T:i:qAR:X:LDS::KB:IC", long_options, &option_index);
    if (c == -1)
      break;

    if (0) {
      printf("option_index %d, c %c 0x%x, optarg %p\n", option_index, c, c, optarg);
      printf("option %s", long_options[option_index].name);
    }

    if (c == 0) {
      usage();
    }

    switch (c) {
    case 'p':
      prom_arg = strdup(optarg);
      break;

    case 'd':
      disk_arg = strdup(optarg);
      break;

    case 't':
      tape_arg = strdup(optarg);
      break;

    case 'k':
      kernel_arg = strdup(optarg);
      break;

    case 'b':
      boot_arg = strdup(optarg);
      break;

    case 'm': {
      const sun2_mode_t *m = sun2_mode_lookup(optarg);
      if (!m) {
        fprintf(stderr, "unknown --mode='%s'.  Valid modes:\n", optarg);
        sun2_mode_print_list();
        exit(1);
      }
      g_mode = m;
      break;
    }

    case 'T':
      g_autotype = strdup(optarg);
      printf("autotype: will type %zu chars after boot delay\n", strlen(optarg));
      break;

    case 'q':
      quiet++;
      break;

    case 'A': {
      extern void sun3_set_auto_abort(int);
      sun2_set_auto_abort(1);
      sun3_set_auto_abort(1);
      printf("auto-abort enabled (Sun-2: first PROM bell-off; "
             "Sun-3: post-keyboard-reset delay → L1-A drops to monitor)\n");
      break;
    }

    case 'R': {
      extern void trace_ring_init(int);
      int n = (int)strtol(optarg, NULL, 0);
      if (n <= 0) { fprintf(stderr, "--trace-ring=N requires N>0\n"); exit(1); }
      trace_ring_init(n);
      break;
    }

    case 'X': {
      extern unsigned int trace_ring_trigger_addr;
      trace_ring_trigger_addr = (unsigned int)strtoul(optarg, NULL, 0);
      printf("trace-ring: trigger addr set to 0x%x (0=any bus error)\n", trace_ring_trigger_addr);
      break;
    }

    case 'I': {
      extern int trace_si;
      trace_si = 1;
      setvbuf(stderr, NULL, _IONBF, 0);
      setvbuf(stdout, NULL, _IONBF, 0);
      fprintf(stderr, "trace-si: enabled (unbuffered)\n");
      break;
    }

    case 'C': {
      extern int trace_scsi;
      trace_scsi = 1;
      fprintf(stderr, "trace-scsi: enabled\n");
      break;
    }

    case 'i':
      g_net_iface = strdup(optarg);
      break;

    case 'L':
      net_list_interfaces();
      exit(0);

    case 'D':
      g_net_dump = 1;
      break;

    case 'S':
      /* --scc-tcp        (no arg)   -> default port 9900
         --scc-tcp=9912   (with arg) -> explicit port */
      g_scc_tcp_port = optarg ? atoi(optarg) : 9900;
      if (g_scc_tcp_port <= 0 || g_scc_tcp_port > 65535) {
        fprintf(stderr, "--scc-tcp: invalid port '%s'\n", optarg ? optarg : "");
        exit(1);
      }
      break;

    case 'K':
      g_no_kbd = 1;
      break;

    case 'B':
      g_scc_boards = atoi(optarg);
      if (g_scc_boards < 0 || g_scc_boards > 4) {
        fprintf(stderr, "--scc-boards: must be 0..4 (got '%s')\n", optarg);
        exit(1);
      }
      break;

    case '?':
      usage();
      break;

    default:
      printf("?? getopt returned character code 0%o ??\n", c);
    }
  }

  if (optind < argc) {
    fprintf(stderr, "extra arguments?\n");
    while (optind < argc)
      printf("%s ", argv[optind++]);
    printf("\n");
    usage();
  }

  /* Fall back to SUN2_NET_IFACE env var if --net-iface wasn't given.
     Strip leading/trailing whitespace — a stray space (e.g. from
     `SUN2_NET_IFACE=7 ` in a shell) would otherwise be passed to
     pcap_open_live and turn into "no such adapter '7 '". */
  if (g_net_iface == NULL) {
    const char *e = getenv("SUN2_NET_IFACE");
    if (e) {
      while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r') e++;
      if (*e) {
        char *dup = strdup(e);
        char *end = dup + strlen(dup);
        while (end > dup && (end[-1] == ' ' || end[-1] == '\t' ||
                             end[-1] == '\n' || end[-1] == '\r'))
          *--end = '\0';
        if (dup[0]) g_net_iface = dup; else free(dup);
      }
    }
  }

  if (!quiet)
    printf("mode: %s (idprom_machine=0x%02x, hires_jumper=%d, fb=%dx%d) — %s\n",
           g_mode->name, g_mode->idprom_machine, g_mode->hires_jumper,
           g_mode->width, g_mode->height, g_mode->desc);

  /* Activate the per-machine bus / MMU vtable.  Default before this
     line is &sun2_ops (set in sim68k.c at file scope) so older command
     lines without --mode keep working unchanged. */
  if (g_mode->ops) g_machine = g_mode->ops;

  /* Machine-specific IDPROM setup. */
  if (g_machine->family == MACH_SUN3)
    sun3_idprom_set_machine(g_mode->idprom_machine);
  else
    idprom_setup(g_mode->idprom_machine);

  if (kernel_arg && boot_arg) {
    setup_kernel(kernel_arg, boot_arg);
    read_kernel();
  } else {
    if (prom_arg == NULL || disk_arg == NULL)
      usage();

    setup_eeprom(prom_arg);

    if (setup_disk(disk_arg))
      exit(1);

    if (tape_arg) {
      if (setup_tape(tape_arg))
        exit(1);
    }
  }

  sim68k();

  exit(0);
}

/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
