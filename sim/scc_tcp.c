/*
 * sun-2 emulator -- SCC tty consoles over TCP, with menu.
 *
 * NEW ARCHITECTURE (replaces the per-tty mutex + per-client thread
 * design from 88e2a16):
 *
 *   CPU/main thread <----> two SPSC ring queues <----> TCP master thread
 *
 * The CPU thread does NOTHING but enqueue/dequeue (tty_idx, byte)
 * pairs.  All sockets, accept(), select(), telnet IAC parsing,
 * menu rendering, partial-send retry, and per-client state live on
 * the TCP master thread.  Result: the CPU hot path's per-instruction
 * cost from this module drops from "10 pthread_mutex_lock+unlock
 * pairs unconditionally" to "one acquire-load + branch when both
 * rings are empty" -- which they are nearly all the time.
 *
 * Public API (scc_tcp.h) is unchanged.
 *
 * Threading:
 *   - One TCP master thread (tcp_master_thread).  Owns the listen
 *     socket and every accepted client socket.  Internal locks
 *     (none currently needed beyond the SPSC atomics and a tiny
 *     bind lock for menu-phase races) stay on this thread.
 *   - The main emulator thread calls scc_tcp_poll() each io_update;
 *     that drains g_rx_q via scc_tty_in_push (only safe on the
 *     main thread).
 *   - SunOS writes to any SCC channel land in scc_tcp_send_byte()
 *     on the main thread; that pushes to g_tx_q.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET    sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define close_sock(s) closesocket(s)
#  define sock_errno() WSAGetLastError()
#  define SOCK_EWOULDBLOCK WSAEWOULDBLOCK
#  define SOCK_EAGAIN     WSAEWOULDBLOCK
#  define SOCK_EINTR      WSAEINTR
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
   typedef int       sock_t;
#  define SOCK_INVALID (-1)
#  define close_sock(s) close(s)
#  define sock_errno() errno
#  define SOCK_EWOULDBLOCK EWOULDBLOCK
#  define SOCK_EAGAIN     EAGAIN
#  define SOCK_EINTR      EINTR
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

#include "scc_tcp.h"
#include "spsc_q.h"
#include "sim.h"

/* These come from scc.c. */
extern int          scc_tty_count(void);
extern const char  *scc_tty_name(int tty_idx);
extern void         scc_tty_in_push(int tty_idx, uint8_t byte);

#define SCC_MAX_TTYS    10
#define MAX_CLIENTS     16        /* concurrent connections incl. menu phase */
#define TX_BACKLOG_SIZE 4096      /* per-client; power of two */

/* ===== SPSC queues between CPU thread and TCP master thread ===== */

static spsc_q_t g_tx_q;   /* CPU -> master: bytes SunOS wants to send out */
static spsc_q_t g_rx_q;   /* master -> CPU: bytes from clients to feed SCC */

/* ===== Per-client state (master thread only) ===== */

typedef enum { CL_FREE = 0, CL_MENU, CL_PASSTHROUGH } client_phase_t;

typedef enum {
    IAC_NORMAL = 0, IAC_GOT_IAC, IAC_GOT_CMD,
    IAC_IN_SB, IAC_IN_SB_GOT_IAC,
} iac_state_t;

typedef struct {
    sock_t           fd;
    char             addr[64];
    client_phase_t   phase;
    int              bound_tty_idx;     /* valid iff phase == CL_PASSTHROUGH */
    iac_state_t      iac_state;
    uint8_t          iac_cmd;
    /* Outbound backlog ring -- collects bytes when send() returns
       WOULDBLOCK or when we want to defer all writes to the select-
       writable check.  Power-of-two size, head/tail are uint counters
       masked into the array. */
    uint8_t          tx_buf[TX_BACKLOG_SIZE];
    uint32_t         tx_head, tx_tail;
} client_t;

static client_t  g_clients[MAX_CLIENTS];

/* Reverse map: tty_idx -> client slot.  -1 means tty is idle. */
static int       g_tty_owner[SCC_MAX_TTYS];

/* ===== Server state ===== */
static volatile int  server_running   = 0;
static volatile int  shutdown_request = 0;
static int           server_port      = 0;
static sock_t        listen_fd        = SOCK_INVALID;
static pthread_t     master_thread;

/* Diagnostic counters.  Atomic so reads from any thread are well-defined.
   SCC_TCP_TRACE=1 in the env enables verbose stderr. */
static int            scc_tcp_trace = 0;
static _Atomic long   bytes_from_scc;
static _Atomic long   bytes_to_client;
static _Atomic long   bytes_from_client;
static _Atomic long   bytes_to_scc;

/* ===== Telnet IAC ===== */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240
#define TELOPT_ECHO 1
#define TELOPT_SGA  3

/* ===== Tiny per-client TX backlog helpers ===== */

static int cl_tx_used(const client_t *c)
{
    return (int)(c->tx_head - c->tx_tail);
}
static int cl_tx_free(const client_t *c)
{
    return TX_BACKLOG_SIZE - cl_tx_used(c);
}

/* Append `len` bytes to the client's outbound backlog.  Returns the
   number of bytes actually accepted (may be less than len if full).
   Caller decides whether a partial accept is OK. */
static int cl_tx_append(client_t *c, const uint8_t *data, int len)
{
    int can = cl_tx_free(c);
    if (can <= 0) return 0;
    if (len > can) len = can;
    for (int i = 0; i < len; i++)
        c->tx_buf[(c->tx_head + i) & (TX_BACKLOG_SIZE - 1)] = data[i];
    c->tx_head += (uint32_t)len;
    return len;
}

/* Try to flush the client's backlog to the socket.  Returns 0 on OK
   (or on WOULDBLOCK), -1 on a fatal socket error (caller should drop
   the client). */
static int cl_tx_flush(client_t *c)
{
    while (c->tx_head != c->tx_tail) {
        uint32_t off = c->tx_tail & (TX_BACKLOG_SIZE - 1);
        uint32_t end = c->tx_head & (TX_BACKLOG_SIZE - 1);
        /* If end > off the live data is contiguous up to `end`.
           Otherwise (wrap, or exactly full where end == off) the
           contiguous run goes to the end of the buffer. */
        uint32_t chunk = (end > off) ? (end - off) : (TX_BACKLOG_SIZE - off);
        int sent = send(c->fd, (const char *)&c->tx_buf[off],
                        (int)chunk, MSG_NOSIGNAL);
        if (sent < 0) {
            int e = sock_errno();
            if (e == SOCK_EWOULDBLOCK || e == SOCK_EAGAIN || e == SOCK_EINTR)
                return 0;
            return -1;
        }
        if (sent == 0) return -1;
        c->tx_tail += (uint32_t)sent;
        atomic_fetch_add_explicit(&bytes_to_client, sent, memory_order_relaxed);
        if ((uint32_t)sent < chunk) return 0;   /* socket buf full */
    }
    return 0;
}

/* Convenience: queue a literal byte buffer to a client and try to
   flush immediately.  Drops on backlog full. */
static int cl_send(client_t *c, const void *data, int len)
{
    int n = cl_tx_append(c, (const uint8_t *)data, len);
    cl_tx_flush(c);
    return n;
}

static int cl_send_str(client_t *c, const char *s)
{
    return cl_send(c, s, (int)strlen(s));
}

static void cl_send_iac3(client_t *c, uint8_t a, uint8_t b, uint8_t d)
{
    uint8_t pkt[3] = { a, b, d };
    cl_send(c, pkt, 3);
}

static void cl_send_telnet_init(client_t *c)
{
    cl_send_iac3(c, IAC, WILL, TELOPT_ECHO);
    cl_send_iac3(c, IAC, WILL, TELOPT_SGA);
    cl_send_iac3(c, IAC, DO,   TELOPT_SGA);
}

/* ===== Menu helpers ===== */

static int letter_to_idx(char ch)
{
    switch (ch) {
    case 'a': return 0; case 'b': return 1;
    case 'e': return 2; case 'f': return 3;
    case 'g': return 4; case 'h': return 5;
    case 'i': return 6; case 'j': return 7;
    case 'k': return 8; case 'l': return 9;
    default:  return -1;
    }
}

static char idx_to_letter(int idx)
{
    static const char letters[10] = {
        'a','b','e','f','g','h','i','j','k','l'
    };
    return (idx >= 0 && idx < 10) ? letters[idx] : '?';
}

static void cl_send_menu(client_t *c)
{
    int n = scc_tty_count();
    char buf[1024];
    int p = 0;

    p += snprintf(buf + p, sizeof(buf) - p,
                  "\r\nSun-2 SCC console -- pick a tty:\r\n");

    for (int i = 0; i < n; i++) {
        char letter   = idx_to_letter(i);
        const char *name = scc_tty_name(i);
        int owner = g_tty_owner[i];
        if (owner < 0)
            p += snprintf(buf + p, sizeof(buf) - p,
                          "  [%c] %s  (idle)\r\n", letter, name);
        else
            p += snprintf(buf + p, sizeof(buf) - p,
                          "  [%c] %s  (busy: %s)\r\n",
                          letter, name, g_clients[owner].addr);
    }

    p += snprintf(buf + p, sizeof(buf) - p,
                  "  [Enter] pick first idle\r\n"
                  "  [q]     disconnect\r\n"
                  "> ");

    cl_send(c, buf, p);
}

/* Returns slot index (0..MAX_CLIENTS-1) of an idle tty's owner, or
   -1 if all configured ttys are busy.  Side-effect free. */
static int find_first_idle_tty(void)
{
    int n = scc_tty_count();
    for (int i = 0; i < n; i++)
        if (g_tty_owner[i] < 0) return i;
    return -1;
}

/* ===== Telnet IAC parser =====
   Returns 1 if `b` is real data (caller should pass it through),
   0 if it was consumed by the IAC state machine.  Replies (DO/DONT/
   WONT/WILL) are queued onto the client's backlog. */
static int iac_feed(client_t *c, uint8_t b)
{
    switch (c->iac_state) {
    case IAC_NORMAL:
        if (b == IAC) { c->iac_state = IAC_GOT_IAC; return 0; }
        return 1;
    case IAC_GOT_IAC:
        if (b == IAC) { c->iac_state = IAC_NORMAL; return 1; }
        if (b == WILL || b == WONT || b == DO || b == DONT) {
            c->iac_cmd = b; c->iac_state = IAC_GOT_CMD; return 0;
        }
        if (b == SB) { c->iac_state = IAC_IN_SB; return 0; }
        c->iac_state = IAC_NORMAL;
        return 0;
    case IAC_GOT_CMD: {
        uint8_t reply = 0;
        switch (c->iac_cmd) {
        case WILL:
            reply = (b == TELOPT_SGA) ? DO : DONT;
            cl_send_iac3(c, IAC, reply, b);
            break;
        case DO:
            if (b != TELOPT_ECHO && b != TELOPT_SGA)
                cl_send_iac3(c, IAC, WONT, b);
            break;
        case WONT: case DONT: break;
        }
        c->iac_state = IAC_NORMAL;
        return 0;
    }
    case IAC_IN_SB:
        if (b == IAC) c->iac_state = IAC_IN_SB_GOT_IAC;
        return 0;
    case IAC_IN_SB_GOT_IAC:
        if (b == SE) c->iac_state = IAC_NORMAL;
        else         c->iac_state = IAC_IN_SB;
        return 0;
    }
    return 0;
}

/* ===== Per-client lifecycle ===== */

static int find_free_client_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].phase == CL_FREE) return i;
    return -1;
}

static void set_nonblocking(sock_t fd)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void client_close(int slot)
{
    client_t *c = &g_clients[slot];
    if (c->phase == CL_PASSTHROUGH && c->bound_tty_idx >= 0) {
        int idx = c->bound_tty_idx;
        if (g_tty_owner[idx] == slot) g_tty_owner[idx] = -1;
        fprintf(stderr, "scc-tcp: %s client %s disconnected\n",
                scc_tty_name(idx), c->addr);
    }
    if (c->fd != SOCK_INVALID) close_sock(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = SOCK_INVALID;
    c->phase = CL_FREE;
    c->bound_tty_idx = -1;
}

/* On menu-phase byte input, returns:
     0 = stay in menu
     1 = transitioned to passthrough
    -1 = client requested disconnect */
static int menu_handle_byte(int slot, uint8_t b)
{
    client_t *c = &g_clients[slot];
    if (b == 'q' || b == 'Q') {
        cl_send_str(c, "\r\nbye\r\n");
        return -1;
    }
    if (b == '\r' || b == '\n') {
        int idle = find_first_idle_tty();
        if (idle < 0) {
            cl_send_str(c, "\r\nno idle ttys -- try again or q to quit\r\n");
            return 0;
        }
        g_tty_owner[idle] = slot;
        c->bound_tty_idx = idle;
        c->phase = CL_PASSTHROUGH;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "\r\n[bound to %s -- Ctrl-] q  to disconnect from telnet]\r\n",
                 scc_tty_name(idle));
        cl_send_str(c, msg);
        fprintf(stderr, "scc-tcp: %s <- client %s\n",
                scc_tty_name(idle), c->addr);
        return 1;
    }
    int lower = tolower((unsigned char)b);
    if (lower >= 'a' && lower <= 'l') {
        int idx = letter_to_idx((char)lower);
        if (idx < 0 || idx >= scc_tty_count()) {
            cl_send_str(c, "\r\nno such tty\r\n");
            cl_send_menu(c);
            return 0;
        }
        if (g_tty_owner[idx] >= 0) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "\r\n%s busy, try another\r\n",
                     scc_tty_name(idx));
            cl_send_str(c, msg);
            cl_send_menu(c);
            return 0;
        }
        g_tty_owner[idx] = slot;
        c->bound_tty_idx = idx;
        c->phase = CL_PASSTHROUGH;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "\r\n[bound to %s -- Ctrl-] q  to disconnect from telnet]\r\n",
                 scc_tty_name(idx));
        cl_send_str(c, msg);
        fprintf(stderr, "scc-tcp: %s <- client %s\n",
                scc_tty_name(idx), c->addr);
        return 1;
    }
    /* Anything else: silently re-prompt. */
    cl_send_menu(c);
    return 0;
}

/* Drain everything currently in g_tx_q.  Each entry's tty_idx maps
   to a client via g_tty_owner; if no client is bound, the byte is
   discarded (matches the old client_fd==INVALID short-circuit). */
static void drain_cpu_tx(void)
{
    scc_tcp_qentry_t e;
    while (spsc_q_pop(&g_tx_q, &e)) {
        atomic_fetch_add_explicit(&bytes_from_scc, 1, memory_order_relaxed);
        if (e.tty_idx >= SCC_MAX_TTYS) continue;
        int slot = g_tty_owner[e.tty_idx];
        if (slot < 0) continue;     /* nobody listening on this tty */
        client_t *c = &g_clients[slot];
        if (c->phase != CL_PASSTHROUGH) continue;
        cl_tx_append(c, &e.byte, 1);
        if (scc_tcp_trace)
            fprintf(stderr, "scc-tcp: send_byte %s 0x%02x\n",
                    scc_tty_name(e.tty_idx), e.byte);
    }
    /* Try to flush every client that has anything to send. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].phase == CL_FREE) continue;
        if (g_clients[i].tx_head == g_clients[i].tx_tail) continue;
        if (cl_tx_flush(&g_clients[i]) < 0)
            client_close(i);
    }
}

static void handle_accept(void)
{
    for (;;) {
        struct sockaddr_in caddr;
#ifdef _WIN32
        int alen = sizeof(caddr);
#else
        socklen_t alen = sizeof(caddr);
#endif
        sock_t cfd = accept(listen_fd, (struct sockaddr *)&caddr, &alen);
        if (cfd == SOCK_INVALID) {
            int e = sock_errno();
            if (e == SOCK_EWOULDBLOCK || e == SOCK_EAGAIN) return;
            if (shutdown_request) return;
            /* Transient -- log and stop draining for this tick. */
            fprintf(stderr, "scc-tcp: accept() transient error %d\n", e);
            return;
        }
        int slot = find_free_client_slot();
        if (slot < 0) {
            const char *msg = "\r\nserver full -- try again later\r\n";
            send(cfd, msg, (int)strlen(msg), MSG_NOSIGNAL);
            close_sock(cfd);
            continue;
        }
        set_nonblocking(cfd);
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&one, sizeof(one));

        client_t *c = &g_clients[slot];
        memset(c, 0, sizeof(*c));
        c->fd = cfd;
        c->phase = CL_MENU;
        c->bound_tty_idx = -1;
        c->iac_state = IAC_NORMAL;
        snprintf(c->addr, sizeof(c->addr), "%s:%u",
                 inet_ntoa(caddr.sin_addr),
                 (unsigned)ntohs(caddr.sin_port));

        cl_send_telnet_init(c);
        cl_send_menu(c);
    }
}

static void handle_client_recv(int slot)
{
    client_t *c = &g_clients[slot];
    uint8_t buf[256];
    int n = recv(c->fd, (char *)buf, (int)sizeof(buf), 0);
    if (n == 0) { client_close(slot); return; }
    if (n < 0) {
        int e = sock_errno();
        if (e == SOCK_EWOULDBLOCK || e == SOCK_EAGAIN || e == SOCK_EINTR) return;
        client_close(slot);
        return;
    }
    if (c->phase == CL_PASSTHROUGH)
        atomic_fetch_add_explicit(&bytes_from_client, n, memory_order_relaxed);

    for (int i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (!iac_feed(c, b)) continue;          /* IAC swallowed */
        if (c->phase == CL_MENU) {
            int r = menu_handle_byte(slot, b);
            if (r < 0) { client_close(slot); return; }
            /* If we transitioned to passthrough mid-buffer, the
               remaining bytes in `buf` are passthrough data. */
        } else if (c->phase == CL_PASSTHROUGH) {
            scc_tcp_qentry_t qe = { (uint8_t)c->bound_tty_idx, b };
            if (!spsc_q_push(&g_rx_q, qe)) {
                /* RX queue overflow: drop.  Should never happen with
                   a 16K-entry queue and SunOS reading promptly. */
                if (scc_tcp_trace)
                    fprintf(stderr, "scc-tcp: g_rx_q full, dropping byte\n");
            }
        }
    }
    if (scc_tcp_trace && c->phase == CL_PASSTHROUGH)
        fprintf(stderr, "scc-tcp: %s recv %d\n",
                scc_tty_name(c->bound_tty_idx), n);
}

/* ===== Master thread ===== */

static void *tcp_master_thread(void *arg)
{
    (void)arg;

    while (!shutdown_request) {
        /* 1. Drain CPU's TX queue first so any newly-queued bytes
              get a chance to start landing in the client TX backlogs
              before we block in select(). */
        drain_cpu_tx();

        /* 2. Build the fdset. */
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        sock_t max_fd = 0;
        if (listen_fd != SOCK_INVALID) {
            FD_SET(listen_fd, &rfds);
            if (listen_fd > max_fd) max_fd = listen_fd;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (c->phase == CL_FREE || c->fd == SOCK_INVALID) continue;
            FD_SET(c->fd, &rfds);
            if (c->tx_head != c->tx_tail) FD_SET(c->fd, &wfds);
            if (c->fd > max_fd) max_fd = c->fd;
        }

        /* 3. Short timeout so we wake quickly to drain g_tx_q.
              5 ms is invisible at terminal speeds and bounds the
              worst-case latency of an SCC-emitted byte to the wire. */
        struct timeval tv = { 0, 5 * 1000 };
        int r = select((int)max_fd + 1, &rfds, &wfds, NULL, &tv);
        if (r < 0) {
            int e = sock_errno();
            if (e == SOCK_EINTR) continue;
            if (shutdown_request) break;
            /* Listen socket may have been closed by stop(); treat
               that as shutdown. */
            if (listen_fd == SOCK_INVALID) break;
            continue;
        }

        /* 4. Accept any pending connections. */
        if (listen_fd != SOCK_INVALID && FD_ISSET(listen_fd, &rfds))
            handle_accept();

        /* 5. Read from every client that has data. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (c->phase == CL_FREE || c->fd == SOCK_INVALID) continue;
            if (FD_ISSET(c->fd, &rfds))
                handle_client_recv(i);
        }

        /* 6. Flush any client whose backlog has waiting data and is
              writable.  cl_tx_flush() also ran inside drain_cpu_tx,
              but writable readiness changes here. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (c->phase == CL_FREE || c->fd == SOCK_INVALID) continue;
            if (c->tx_head == c->tx_tail) continue;
            if (FD_ISSET(c->fd, &wfds)) {
                if (cl_tx_flush(c) < 0)
                    client_close(i);
            }
        }
    }

    /* Clean up: close all client sockets. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (c->phase != CL_FREE && c->fd != SOCK_INVALID)
            close_sock(c->fd);
    }
    return NULL;
}

/* ===== Public API ===== */

void scc_tcp_send_byte(int tty_idx, uint8_t byte)
{
    if (!server_running) return;
    if (tty_idx < 0 || tty_idx >= SCC_MAX_TTYS) return;
    scc_tcp_qentry_t e = { (uint8_t)tty_idx, byte };
    /* On full queue we just drop -- matches the old per-tty ringbuf
       behavior where a full out_buf silently lost bytes. */
    spsc_q_push(&g_tx_q, e);
}

void scc_tcp_poll(void)
{
    /* Hot path: empty-queue check is one acquire-load + branch.
       This is what replaced the 10 mutex_lock+unlock pairs. */
    if (spsc_q_empty(&g_rx_q)) return;
    scc_tcp_qentry_t e;
    while (spsc_q_pop(&g_rx_q, &e)) {
        atomic_fetch_add_explicit(&bytes_to_scc, 1, memory_order_relaxed);
        scc_tty_in_push(e.tty_idx, e.byte);
    }
}

int scc_tcp_start(int port)
{
    if (server_running) return 0;

    {
        const char *e = getenv("SCC_TCP_TRACE");
        scc_tcp_trace = (e && *e && *e != '0');
    }

    spsc_q_init(&g_tx_q);
    spsc_q_init(&g_rx_q);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        memset(&g_clients[i], 0, sizeof(g_clients[i]));
        g_clients[i].fd = SOCK_INVALID;
        g_clients[i].phase = CL_FREE;
        g_clients[i].bound_tty_idx = -1;
    }
    for (int i = 0; i < SCC_MAX_TTYS; i++)
        g_tty_owner[i] = -1;

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "scc-tcp: WSAStartup failed\n");
            return -1;
        }
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == SOCK_INVALID) {
        fprintf(stderr, "scc-tcp: socket() failed\n");
        return -1;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&one, sizeof(one));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port        = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        fprintf(stderr, "scc-tcp: bind() port %d failed\n", port);
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
        return -1;
    }
    if (listen(listen_fd, 4) < 0) {
        fprintf(stderr, "scc-tcp: listen() failed\n");
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
        return -1;
    }
    set_nonblocking(listen_fd);

    server_port      = port;
    server_running   = 1;
    shutdown_request = 0;

    if (pthread_create(&master_thread, NULL, tcp_master_thread, NULL) != 0) {
        fprintf(stderr, "scc-tcp: pthread_create failed\n");
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
        server_running = 0;
        return -1;
    }

    fprintf(stderr, "scc-tcp: listening on port %d (menu-driven; %d ttys)\n",
            port, scc_tty_count());
    return 0;
}

void scc_tcp_stop(void)
{
    if (!server_running) return;
    shutdown_request = 1;

    /* Closing listen_fd unblocks any select() waiting on it (the
       master thread's 5 ms select will return on its own anyway,
       but this makes shutdown deterministic). */
    if (listen_fd != SOCK_INVALID) {
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
    }

    pthread_join(master_thread, NULL);
    server_running = 0;

#ifdef _WIN32
    WSACleanup();
#endif
}
