/*
 * sun-2 emulator
 * 10/2014  Brad Parker <brad@heeltoe.com>
 *
 * utiity routines
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

/* Note: we deliberately do NOT include sim.h above the sun2_dprintf
   definition — including sim.h would activate the printf-wrapping
   macro from <sim.h> in TRACE_PRINTF builds, and the wrapped-printf
   inside dumpbuffer below would then call back into sun2_dprintf
   for every byte (no infinite recursion since it goes via vfprintf,
   but we keep it clean by including sim.h *after* sun2_dprintf is
   defined so the macro only wraps the helper functions further down). */
static const char *basename_of(const char *file)
{
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

int sun2_dprintf(const char *file, int line, const char *fmt, ...)
{
    fprintf(stdout, "[%s:%d] ", basename_of(file), line);
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* Wrapped perror() under TRACE_PRINTF: prints [file:line] arg: errstr to
   stderr in a single fprintf so the prefix can't be lost to interleaving. */
void sun2_dperror(const char *file, int line, const char *s)
{
    int e = errno;
    fprintf(stderr, "[%s:%d] perror(%s): %s\n",
            basename_of(file), line,
            s ? s : "(null)",
            strerror(e));
}

/* From here on, including sim.h activates the TRACE_PRINTF wrapper for
   every printf() call below — so dumpbuffer's hex dumps are also tagged
   with their source line in trace builds. */
#include "sim.h"

void dumpbuffer(char *buf, int len)
{
    unsigned char *b = (unsigned char *)buf;
    int offset = 0;
    int i;
    char lb[17];

    while (len > 0) {
        printf("%03x: ", offset);
        for (i = 0; i < 8; i++) {
            if (i < len) {
                printf("%02x ", b[i]);
                lb[i] = (b[i] >= ' ' && b[i] <= '~') ? b[i] : '.';
            } else {
                printf("xx ");
                lb[i] = 'x';
            }
        }
        lb[8] = 0;
        
        printf(" %s\n", lb);
        len -= 8;
        b += 8;
        offset += 8;
    }
}

void abortf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  exit(1);
}


/* Local Variables:  */
/* mode: c           */
/* c-basic-offset: 2 */
/* End:              */
