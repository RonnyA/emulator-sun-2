/*
 * sun-2 emulator — SCC channel-A console over TCP
 *
 * When started, exposes a TCP listener on the configured port.  One
 * client at a time can connect.  Bytes from the client are pushed
 * into SCC channel 0 (= SunOS /dev/console), and bytes SunOS writes
 * to channel 0 are forwarded out to the client.
 *
 *   telnet host PORT      ← simplest
 *   nc     host PORT      ← rawer; no IAC handshake noise at start
 *
 * The server itself is dumb: raw bytes both ways.  When connecting
 * with telnet, the client may emit a few IAC negotiation bytes
 * (0xFF...) before any real data; SunOS's tty discipline will
 * mostly ignore them.
 *
 * Threading model:
 *   - scc_tcp_start() spawns a server thread.
 *   - The server thread does accept() + the per-client read/write
 *     loop.  Reads put bytes in an input ring buffer; writes drain
 *     an output ring buffer.
 *   - The main emulator thread calls scc_tcp_poll() each iteration
 *     of the io_update loop to drain the input ring buffer into
 *     scc_in_push() (which is not thread-safe on its own).
 *   - SunOS writes to SCC channel 0 / 1 land in scc_tcp_send_byte(),
 *     which appends to the output ring buffer (mutex-protected).
 */

#ifndef SCC_TCP_H
#define SCC_TCP_H

#include <stdint.h>

/* Start the TCP listener on `port`.  No-op if already running.
   Default port suggested by the CLI is 9900.  Returns 0 on success,
   -1 on failure (port already in use, socket setup error). */
int  scc_tcp_start(int port);

/* Tear down the TCP listener and any active client connection.
   Safe to call even if scc_tcp_start was never called. */
void scc_tcp_stop(void);

/* Called by the main emulator thread (from io_update) to deliver
   any TCP-arrived bytes to SCC channel 0's input FIFO.  No-op when
   no server is running. */
void scc_tcp_poll(void);

/* Called from scc_wr_data() in scc.c when SunOS writes a byte to
   channel 0 or 1 — we forward it out the TCP client.  No-op when
   no client is connected. */
void scc_tcp_send_byte(int ch, uint8_t byte);

#endif /* SCC_TCP_H */
