/*
 * sun emulator -- per-machine bus / MMU / device-tick vtable.
 *
 * The CPU loop in sim68k.c is machine-agnostic.  Per-instruction bus
 * access (Musashi calls), device updates (io_update), startup (io_init),
 * and IRQ acknowledge (cpu_irq_ack) all dispatch through g_machine.
 *
 * g_machine is set ONCE at startup from the --mode CLI flag and never
 * changes.  Reads of g_machine are therefore essentially free in the
 * hot path -- a single global pointer load amortised across the whole
 * cpu_read / cpu_write call cost.
 *
 * To add a new machine: provide a const machine_ops_t with the function
 * pointers filled in, list it in sim.c's mode table, and the existing
 * CPU loop just works.
 */

#ifndef SIM_MACHINE_H
#define SIM_MACHINE_H

typedef enum {
    MACH_SUN2 = 0,
    MACH_SUN3 = 1,
} machine_family_t;

typedef struct machine_ops_s {
    const char       *name;            /* "sun2" / "sun3" */
    machine_family_t  family;
    int               m68k_cpu_type;   /* M68K_CPU_TYPE_68010 / _68020 */

    /* Lifecycle. */
    void (*init)(void);                /* devices + IDPROM + timers etc. */
    void (*reset)(void);               /* CPU pulse-reset hook */

    /* Bus access (function code is read from the global g_fc). */
    unsigned int (*cpu_read)(int size, unsigned int va);
    void         (*cpu_write)(int size, unsigned int va, unsigned int value);

    /* Per-instruction device tick (called from the top-level io_update
       inside the CPU loop).  Generic concerns -- SCC-TCP polling and
       SDL throttling -- live in the top-level io_update, not here. */
    void (*device_tick)(void);

    /* Interrupt-ack vector for a given IRQ level. */
    int  (*irq_ack)(int level);

    /* Keyboard SCC TX handler -- called from the SCC shim when the CPU
       writes a byte to the keyboard channel (zs1 chan A).  Per-machine
       because Sun-3 needs a different reset response (queue 0xFF/0x04/
       0x7F + write Type-4 byte directly to RAM at VA 0xFFFFE013) and
       different auto-abort trigger (post-reset delay countdown vs.
       Sun-2's bell-off). */
    void (*kb_write)(int value);
} machine_ops_t;

extern const machine_ops_t *g_machine;

/* Provided by sim68k.c (Sun-2). */
extern const machine_ops_t sun2_ops;

#endif /* SIM_MACHINE_H */
