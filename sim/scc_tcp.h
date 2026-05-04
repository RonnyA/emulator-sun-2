/*
 * sun-2 emulator -- SCC tty consoles over TCP, with menu.
 *
 * One TCP listener is started by scc_tcp_start(port).  Each new client
 * is greeted by a menu of configured ttys (ttya, ttyb, plus ttye/f/g/
 * h/i/j/k/l from --scc-boards expansion cards).  The client picks a
 * tty by letter, Enter for the first idle one, or q to disconnect.
 * Once bound the connection is transparent: bytes flow both ways
 * between the chosen SCC channel and the socket.
 *
 * Telnet IAC negotiation:
 *   On connect we send IAC WILL ECHO + IAC WILL/DO SUPPRESS_GO_AHEAD
 *   so a vanilla `telnet host port` switches to remote-echo character-
 *   at-a-time mode immediately.  Inbound IAC sequences from the client
 *   are parsed and swallowed.
 *
 * Threading:
 *   - One accept thread (in server_thread_func).
 *   - One worker thread per connected client (in client_thread_func)
 *     that handles menu + transparent passthrough for that client.
 *   - The main emulator thread calls scc_tcp_poll() each io_update
 *     iteration to drain per-tty input ringbufs into the SCC chip
 *     (via scc_tty_in_push, which is only safe on the main thread).
 *   - SunOS writes to any SCC channel land in scc_tcp_send_byte(idx,
 *     byte) on the main thread; the function appends to the per-tty
 *     output ringbuf, which the worker thread drains and sends.
 *
 * tty_idx convention (matches scc.c's scc_tty_name):
 *     0  ttya = zs0 chan A
 *     1  ttyb = zs0 chan B
 *     2  ttye = zs2 chan A
 *     3  ttyf = zs2 chan B
 *     4  ttyg = zs3 chan A   ... up to 9 = ttyl = zs5 chan B.
 */

#ifndef SCC_TCP_H
#define SCC_TCP_H

#include <stdint.h>

int  scc_tcp_start(int port);
void scc_tcp_stop(void);

/* Called by the emulator main thread to drain per-tty input ringbufs
   into the SCC chips.  No-op when the server isn't running. */
void scc_tcp_poll(void);

/* Called from scc.c on every byte SunOS transmits.  tty_idx is the
   per-channel index (0..9 -- see scc_tty_name() in scc.c). */
void scc_tcp_send_byte(int tty_idx, uint8_t byte);

#endif /* SCC_TCP_H */
