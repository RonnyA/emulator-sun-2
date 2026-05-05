/*
 * sun-2 emulator -- single-producer / single-consumer lock-free ring.
 *
 * Used between the CPU/main thread and the TCP master thread to ferry
 * (tty_idx, byte) pairs in both directions.  Each direction has its
 * own ring; one thread is the only producer for that ring, the other
 * thread is the only consumer.  No mutexes, no CAS -- just acquire/
 * release on monotonic 32-bit head/tail counters.
 *
 * Why this matters: the previous design took a pthread_mutex per tty
 * per io_update tick (10 lock/unlock pairs per emulated 68010
 * instruction, even with no clients connected).  That overhead is now
 * one acquire-load + branch in the empty case.
 *
 * Cross-platform: requires C11 <stdatomic.h>.  Confirmed working on:
 *   - Linux GCC >= 4.9 / clang >= 3.6
 *   - Windows MSYS2 MinGW-w64 GCC
 *   - macOS Apple clang / Homebrew GCC
 */

#ifndef SCC_SPSC_Q_H
#define SCC_SPSC_Q_H

#include <stdint.h>

#if defined(__STDC_NO_ATOMICS__)
#  error "spsc_q.h needs C11 <stdatomic.h>; toolchain advertises __STDC_NO_ATOMICS__"
#endif
#include <stdatomic.h>

typedef struct {
    uint8_t tty_idx;     /* 0..9, see scc_tty_name() in scc.c */
    uint8_t byte;
} scc_tcp_qentry_t;

/* 16384 entries * 2 bytes = 32 KiB per ring.  Aggregate SCC traffic on
   a Sun-2 with all 10 ttys is at most a few KB/s, so this gives over
   a second of slack -- producer drops on overflow rather than blocks. */
#define SPSC_Q_SIZE 16384u
#if (SPSC_Q_SIZE & (SPSC_Q_SIZE - 1u)) != 0
#  error "SPSC_Q_SIZE must be a power of two"
#endif
#define SPSC_Q_MASK (SPSC_Q_SIZE - 1u)

typedef struct {
    _Atomic uint32_t head;          /* producer-only writes; consumer reads w/ acquire */
    _Atomic uint32_t tail;          /* consumer-only writes; producer reads w/ acquire */
    scc_tcp_qentry_t buf[SPSC_Q_SIZE];
} spsc_q_t;

static inline void spsc_q_init(spsc_q_t *q)
{
    atomic_store_explicit(&q->head, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0u, memory_order_relaxed);
}

/* Producer-side push.  Returns 1 on success, 0 on full (caller drops). */
static inline int spsc_q_push(spsc_q_t *q, scc_tcp_qentry_t e)
{
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if ((uint32_t)(h - t) >= SPSC_Q_SIZE) return 0;
    q->buf[h & SPSC_Q_MASK] = e;
    atomic_store_explicit(&q->head, h + 1u, memory_order_release);
    return 1;
}

/* Consumer-side pop.  Returns 1 on success, 0 on empty. */
static inline int spsc_q_pop(spsc_q_t *q, scc_tcp_qentry_t *out)
{
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (h == t) return 0;
    *out = q->buf[t & SPSC_Q_MASK];
    atomic_store_explicit(&q->tail, t + 1u, memory_order_release);
    return 1;
}

/* Cheap "is there anything?" check used by the consumer fast path.
   One acquire-load + one relaxed-load + branch -- no mutex, no CAS. */
static inline int spsc_q_empty(const spsc_q_t *q)
{
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    return h == t;
}

#endif /* SCC_SPSC_Q_H */
