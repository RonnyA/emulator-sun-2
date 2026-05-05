/*
 * sun emulator -- Sun-3/60 machine.
 *
 * MC68020 + Sun custom MMU.  28-bit virtual address space (256 MB per
 * context).  PROM at 0x0FEF0000 (64 KB).  IDPROM machine type 0x17.
 *
 * Reference: RetroCore (C# implementation) at
 *   E:/Dev/Repos/Ronny/RetroCore/Emulated.Machines/Sun/Sun3/
 * specifically MachineSun3Memory.cs and Sun3MachineConfig.cs.
 *
 * Other references:
 *   - SunOS 4.1.3 source: sys/stand/mon/sun3/cpu.addrs.h (OBIO addresses)
 *   - SunOS 4.1.3 source: sys/stand/mon/h/sunromvec.h (PROM vector)
 *   - Bitsavers: Sun-3 Architecture Manual
 *   - Gunkies: https://gunkies.org/wiki/Sun-3
 */

#ifndef SIM_SUN3_H
#define SIM_SUN3_H

#include "machine.h"

/* Sun-3/60 machine ops vtable.  Selected by --mode=3/60. */
extern const machine_ops_t sun3_ops;

/* Set the IDPROM machine type byte.  0x17 = Sun-3/60.
   Called from main() once the --mode is parsed. */
void sun3_idprom_set_machine(unsigned char machine_type);

#endif /* SIM_SUN3_H */
