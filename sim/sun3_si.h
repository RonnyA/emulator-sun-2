#ifndef SIM_SUN3_SI_H
#define SIM_SUN3_SI_H

#include <stdint.h>

void     sun3_si_init(void);
uint32_t sun3_si_read(uint32_t off, int size);
void     sun3_si_write(uint32_t off, uint32_t value, int size);

#endif
