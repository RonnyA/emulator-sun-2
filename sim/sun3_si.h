#ifndef SIM_SUN3_SI_H
#define SIM_SUN3_SI_H

#include <stdint.h>

void     sun3_si_init(void);
uint32_t sun3_si_read(uint32_t off, int size);
void     sun3_si_write(uint32_t off, uint32_t value, int size);

/* Called from sc.c when scsi.c does sc_dma_read_data / sc_dma_write_data
   under Sun-3 mode — stash the buffer for replay on UDC chain trigger. */
void     sun3_si_stash_dma_read(unsigned char *buf, int siz);
void     sun3_si_stash_dma_write(unsigned char *buf, int siz);

#endif
