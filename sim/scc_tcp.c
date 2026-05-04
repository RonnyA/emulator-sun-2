/*
 * sun-2 emulator -- SCC tty consoles over TCP, with menu.
 *
 * See scc_tcp.h for the design overview.
 *
 * Pattern lifted from nd100x's telnetserver (same author): on connect,
 * present a list of available terminals, let the user pick one, then
 * tunnel that channel transparently.  Differences vs. nd100x:
 *   - up to 10 ttys (ttya, ttyb, ttye..ttyl); SunOS Sun-2 skips ttyc/d
 *     because zs1 is the kbd/mouse chip.
 *   - busy slots show the connected client address; second connection
 *     to a busy tty is refused with a message and re-enters the menu.
 *   - one accept thread + per-client worker thread (instead of a
 *     single-client server-thread model).
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
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
   typedef int       sock_t;
#  define SOCK_INVALID (-1)
#  define close_sock(s) close(s)
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

#include "scc_tcp.h"
#include "sim.h"

/* These come from scc.c. */
extern int          scc_tty_count(void);
extern const char  *scc_tty_name(int tty_idx);
extern void         scc_tty_in_push(int tty_idx, uint8_t byte);

/* ===== Per-tty state ===== */
#define SCC_MAX_TTYS      10
#define SCC_TCP_IN_SIZE   256
#define SCC_TCP_OUT_SIZE  4096

typedef struct scc_tcp_tty_s {
    /* IN: TCP client -> SunOS.  Worker thread writes here, main thread
       drains via scc_tcp_poll(). */
    uint8_t          in_buf[SCC_TCP_IN_SIZE];
    int              in_head, in_tail;
    pthread_mutex_t  in_lock;

    /* OUT: SunOS -> TCP client.  Main thread (scc_tcp_send_byte) writes
       here, worker thread drains and sends. */
    uint8_t          out_buf[SCC_TCP_OUT_SIZE];
    int              out_head, out_tail;
    pthread_mutex_t  out_lock;

    /* Bound client.  SOCK_INVALID if no client.  client_addr is a
       printable form for the menu's "busy: X" tag. */
    sock_t           client_fd;
    char             client_addr[64];
} scc_tcp_tty_t;

static scc_tcp_tty_t g_ttys[SCC_MAX_TTYS];
static pthread_mutex_t g_bind_lock = PTHREAD_MUTEX_INITIALIZER;

/* ===== Server state ===== */
static volatile int  server_running   = 0;
static volatile int  shutdown_request = 0;
static int           server_port      = 0;
static sock_t        listen_fd        = SOCK_INVALID;
static pthread_t     accept_thread;

/* Diagnostic counters (env SCC_TCP_TRACE=1 enables verbose stderr). */
static int  scc_tcp_trace = 0;
static long bytes_from_scc;
static long bytes_to_client;
static long bytes_from_client;
static long bytes_to_scc;

/* ===== Ring-buffer helpers (caller holds the relevant lock) ===== */
static int rb_put(uint8_t *buf, int size, int *head, int tail, uint8_t b)
{
    int next = (*head + 1) % size;
    if (next == tail) return 0;
    buf[*head] = b;
    *head = next;
    return 1;
}

static int rb_get_block(uint8_t *buf, int size, int head, int *tail,
                        uint8_t *out, int max)
{
    int n = 0;
    while (n < max && *tail != head) {
        out[n++] = buf[*tail];
        *tail = (*tail + 1) % size;
    }
    return n;
}

/* ===== Public API: main-thread side ===== */

void scc_tcp_send_byte(int tty_idx, uint8_t byte)
{
    if (!server_running) return;
    if (tty_idx < 0 || tty_idx >= SCC_MAX_TTYS) return;
    scc_tcp_tty_t *t = &g_ttys[tty_idx];
    /* Skip if no one is listening on this tty -- avoids filling the
       ringbuf with megabytes of unsent data while ttya runs at 9600. */
    if (t->client_fd == SOCK_INVALID) return;

    pthread_mutex_lock(&t->out_lock);
    rb_put(t->out_buf, SCC_TCP_OUT_SIZE, &t->out_head, t->out_tail, byte);
    pthread_mutex_unlock(&t->out_lock);
    bytes_from_scc++;
    if (scc_tcp_trace)
        fprintf(stderr, "scc-tcp: send_byte %s 0x%02x\n",
                scc_tty_name(tty_idx), byte);
}

void scc_tcp_poll(void)
{
    int i;
    if (!server_running) return;
    for (i = 0; i < SCC_MAX_TTYS; i++) {
        scc_tcp_tty_t *t = &g_ttys[i];
        pthread_mutex_lock(&t->in_lock);
        while (t->in_tail != t->in_head) {
            uint8_t b = t->in_buf[t->in_tail];
            t->in_tail = (t->in_tail + 1) % SCC_TCP_IN_SIZE;
            scc_tty_in_push(i, b);
            bytes_to_scc++;
        }
        pthread_mutex_unlock(&t->in_lock);
    }
}

/* ===== Telnet IAC parser =====
 * RFC 854/857/858.  Active during BOTH the menu phase and the
 * passthrough phase: in either case we want IAC sequences swallowed
 * so they don't leak into the user's keystrokes (during menu) or
 * into SunOS's tty input (during passthrough).
 */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240

#define TELOPT_ECHO 1
#define TELOPT_SGA  3

typedef enum {
    IAC_NORMAL = 0,
    IAC_GOT_IAC,
    IAC_GOT_CMD,
    IAC_IN_SB,
    IAC_IN_SB_GOT_IAC,
} iac_state_t;

static void send_iac3(sock_t cfd, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t pkt[3] = { a, b, c };
    send(cfd, (const char *)pkt, 3, MSG_NOSIGNAL);
}

static void send_telnet_init(sock_t cfd)
{
    send_iac3(cfd, IAC, WILL, TELOPT_ECHO);
    send_iac3(cfd, IAC, WILL, TELOPT_SGA);
    send_iac3(cfd, IAC, DO,   TELOPT_SGA);
}

/* Returns 1 if `b` is real data (pass through to caller), 0 if it
   was consumed by IAC. */
static int iac_feed(sock_t cfd, iac_state_t *st, uint8_t *cmd, uint8_t b)
{
    switch (*st) {
    case IAC_NORMAL:
        if (b == IAC) { *st = IAC_GOT_IAC; return 0; }
        return 1;
    case IAC_GOT_IAC:
        if (b == IAC) { *st = IAC_NORMAL; return 1; }
        if (b == WILL || b == WONT || b == DO || b == DONT) {
            *cmd = b; *st = IAC_GOT_CMD; return 0;
        }
        if (b == SB) { *st = IAC_IN_SB; return 0; }
        *st = IAC_NORMAL;
        return 0;
    case IAC_GOT_CMD: {
        uint8_t reply = 0;
        switch (*cmd) {
        case WILL:
            reply = (b == TELOPT_SGA) ? DO : DONT;
            send_iac3(cfd, IAC, reply, b);
            break;
        case DO:
            if (b != TELOPT_ECHO && b != TELOPT_SGA)
                send_iac3(cfd, IAC, WONT, b);
            break;
        case WONT: case DONT: break;
        }
        *st = IAC_NORMAL;
        return 0;
    }
    case IAC_IN_SB:
        if (b == IAC) *st = IAC_IN_SB_GOT_IAC;
        return 0;
    case IAC_IN_SB_GOT_IAC:
        if (b == SE) *st = IAC_NORMAL;
        else         *st = IAC_IN_SB;
        return 0;
    }
    return 0;
}

/* ===== Menu helpers ===== */

/* Map menu letter (a, b, e, f, g, h, i, j, k, l) -> tty_idx 0..9. */
static int letter_to_idx(char c)
{
    switch (c) {
    case 'a': return 0;
    case 'b': return 1;
    case 'e': return 2;
    case 'f': return 3;
    case 'g': return 4;
    case 'h': return 5;
    case 'i': return 6;
    case 'j': return 7;
    case 'k': return 8;
    case 'l': return 9;
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

static void send_menu(sock_t cfd)
{
    int n = scc_tty_count();
    int i;
    char buf[1024];
    int p = 0;

    p += snprintf(buf + p, sizeof(buf) - p,
                  "\r\nSun-2 SCC console -- pick a tty:\r\n");

    for (i = 0; i < n; i++) {
        scc_tcp_tty_t *t = &g_ttys[i];
        char letter = idx_to_letter(i);
        const char *name = scc_tty_name(i);

        /* Snapshot busy state with the bind lock held briefly. */
        sock_t cfd_snapshot;
        char addr_snapshot[64];
        pthread_mutex_lock(&g_bind_lock);
        cfd_snapshot = t->client_fd;
        memcpy(addr_snapshot, t->client_addr, sizeof(addr_snapshot));
        pthread_mutex_unlock(&g_bind_lock);

        if (cfd_snapshot == SOCK_INVALID)
            p += snprintf(buf + p, sizeof(buf) - p,
                          "  [%c] %s  (idle)\r\n", letter, name);
        else
            p += snprintf(buf + p, sizeof(buf) - p,
                          "  [%c] %s  (busy: %s)\r\n",
                          letter, name, addr_snapshot);
    }

    p += snprintf(buf + p, sizeof(buf) - p,
                  "  [Enter] pick first idle\r\n"
                  "  [q]     disconnect\r\n"
                  "> ");

    send(cfd, buf, p, MSG_NOSIGNAL);
}

static void send_str(sock_t cfd, const char *s)
{
    send(cfd, s, (int)strlen(s), MSG_NOSIGNAL);
}

/* Read one byte through the IAC parser, skipping IAC sequences.  Used
   during menu interaction.  Returns the byte, or -1 on disconnect/error. */
static int read_byte_iac(sock_t cfd, iac_state_t *st, uint8_t *cmd)
{
    for (;;) {
        char c;
        int n = recv(cfd, &c, 1, 0);
        if (n <= 0) return -1;
        if (iac_feed(cfd, st, cmd, (uint8_t)c))
            return (uint8_t)c;
    }
}

/* Try to bind this client to tty_idx.  Returns 1 if bound, 0 if busy. */
static int try_bind(int tty_idx, sock_t cfd, const char *addr)
{
    int ok = 0;
    pthread_mutex_lock(&g_bind_lock);
    if (g_ttys[tty_idx].client_fd == SOCK_INVALID) {
        g_ttys[tty_idx].client_fd = cfd;
        snprintf(g_ttys[tty_idx].client_addr,
                 sizeof(g_ttys[tty_idx].client_addr), "%s", addr);
        /* Drop any stale ringbuf data from a previous client. */
        g_ttys[tty_idx].in_head  = g_ttys[tty_idx].in_tail  = 0;
        g_ttys[tty_idx].out_head = g_ttys[tty_idx].out_tail = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&g_bind_lock);
    return ok;
}

static void unbind(int tty_idx)
{
    pthread_mutex_lock(&g_bind_lock);
    g_ttys[tty_idx].client_fd = SOCK_INVALID;
    g_ttys[tty_idx].client_addr[0] = '\0';
    pthread_mutex_unlock(&g_bind_lock);
}

static int find_first_idle(void)
{
    int n = scc_tty_count();
    int i;
    pthread_mutex_lock(&g_bind_lock);
    int found = -1;
    for (i = 0; i < n; i++) {
        if (g_ttys[i].client_fd == SOCK_INVALID) { found = i; break; }
    }
    pthread_mutex_unlock(&g_bind_lock);
    return found;
}

/* ===== Per-client worker thread ===== */

typedef struct client_args_s {
    sock_t cfd;
    char   addr[64];
} client_args_t;

static void passthrough(sock_t cfd, int tty_idx, iac_state_t *iac_st,
                        uint8_t *iac_cmd)
{
    scc_tcp_tty_t *t = &g_ttys[tty_idx];

    while (!shutdown_request) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cfd, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        int r = select((int)(cfd + 1), &rfds, NULL, NULL, &tv);
        if (r < 0) break;

        if (r > 0 && FD_ISSET(cfd, &rfds)) {
            char buf[64];
            int n = recv(cfd, buf, (int)sizeof(buf), 0);
            if (n <= 0) break;
            bytes_from_client += n;
            pthread_mutex_lock(&t->in_lock);
            for (int i = 0; i < n; i++) {
                if (iac_feed(cfd, iac_st, iac_cmd, (uint8_t)buf[i]))
                    rb_put(t->in_buf, SCC_TCP_IN_SIZE,
                           &t->in_head, t->in_tail, (uint8_t)buf[i]);
            }
            pthread_mutex_unlock(&t->in_lock);
            if (scc_tcp_trace)
                fprintf(stderr, "scc-tcp: %s recv %d\n",
                        scc_tty_name(tty_idx), n);
        }

        /* Drain the OUT ringbuf for this tty to the client. */
        uint8_t out[256];
        pthread_mutex_lock(&t->out_lock);
        int outlen = rb_get_block(t->out_buf, SCC_TCP_OUT_SIZE,
                                  t->out_head, &t->out_tail,
                                  out, (int)sizeof(out));
        pthread_mutex_unlock(&t->out_lock);
        if (outlen > 0) {
            int sent = send(cfd, (const char *)out, outlen, MSG_NOSIGNAL);
            if (sent < 0) break;
            bytes_to_client += sent;
            if (scc_tcp_trace)
                fprintf(stderr, "scc-tcp: %s send %d\n",
                        scc_tty_name(tty_idx), sent);
        }
    }
}

static void *client_thread_func(void *arg)
{
    client_args_t *args = (client_args_t *)arg;
    sock_t cfd  = args->cfd;
    char   addr[64];
    snprintf(addr, sizeof(addr), "%s", args->addr);
    free(args);

    iac_state_t iac_st  = IAC_NORMAL;
    uint8_t     iac_cmd = 0;

    /* Telnet negotiation first so a real telnet client switches to
       remote-echo mode before we render the menu. */
    send_telnet_init(cfd);

    /* Menu loop. */
    int tty_idx = -1;
    while (tty_idx < 0 && !shutdown_request) {
        send_menu(cfd);
        int b = read_byte_iac(cfd, &iac_st, &iac_cmd);
        if (b < 0) goto done;       /* disconnect */

        if (b == 'q' || b == 'Q') {
            send_str(cfd, "\r\nbye\r\n");
            goto done;
        }
        if (b == '\r' || b == '\n') {
            int idle = find_first_idle();
            if (idle < 0) {
                send_str(cfd, "\r\nno idle ttys -- try again or q to quit\r\n");
                continue;
            }
            if (!try_bind(idle, cfd, addr)) {
                /* Race: someone else grabbed it.  Re-show menu. */
                send_str(cfd, "\r\nrace lost, retrying\r\n");
                continue;
            }
            tty_idx = idle;
            break;
        }
        b = tolower(b);
        if (b >= 'a' && b <= 'l') {
            int idx = letter_to_idx((char)b);
            if (idx < 0 || idx >= scc_tty_count()) {
                send_str(cfd, "\r\nno such tty\r\n");
                continue;
            }
            if (!try_bind(idx, cfd, addr)) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "\r\n%s busy, try another\r\n",
                         scc_tty_name(idx));
                send_str(cfd, msg);
                continue;
            }
            tty_idx = idx;
            break;
        }
        /* anything else: silent re-prompt */
    }

    if (tty_idx < 0) goto done;

    /* Bound!  Announce and drop into passthrough. */
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "\r\n[bound to %s -- Ctrl-] q  to disconnect from telnet]\r\n",
                 scc_tty_name(tty_idx));
        send_str(cfd, msg);
    }
    fprintf(stderr, "scc-tcp: %s <- client %s\n", scc_tty_name(tty_idx), addr);

    passthrough(cfd, tty_idx, &iac_st, &iac_cmd);

    fprintf(stderr, "scc-tcp: %s client %s disconnected\n",
            scc_tty_name(tty_idx), addr);
    unbind(tty_idx);

done:
    close_sock(cfd);
    return NULL;
}

/* ===== Accept thread ===== */

static void *server_thread_func(void *arg)
{
    (void)arg;
    while (!shutdown_request) {
        struct sockaddr_in caddr;
#ifdef _WIN32
        int alen = sizeof(caddr);
#else
        socklen_t alen = sizeof(caddr);
#endif
        sock_t cfd = accept(listen_fd, (struct sockaddr *)&caddr, &alen);
        if (cfd == SOCK_INVALID) break;

        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&one, sizeof(one));

        client_args_t *args = (client_args_t *)malloc(sizeof(*args));
        if (!args) { close_sock(cfd); continue; }
        args->cfd = cfd;
        snprintf(args->addr, sizeof(args->addr), "%s:%u",
                 inet_ntoa(caddr.sin_addr),
                 (unsigned)ntohs(caddr.sin_port));

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread_func, args) != 0) {
            fprintf(stderr, "scc-tcp: pthread_create for client failed\n");
            close_sock(cfd);
            free(args);
            continue;
        }
        /* Detach: workers clean up themselves on disconnect. */
        pthread_detach(th);
    }
    return NULL;
}

/* ===== Lifecycle ===== */

int scc_tcp_start(int port)
{
    int i;
    if (server_running) return 0;

    {
        const char *e = getenv("SCC_TCP_TRACE");
        scc_tcp_trace = (e && *e && *e != '0');
    }

    /* Init per-tty state.  All ttys start unbound. */
    for (i = 0; i < SCC_MAX_TTYS; i++) {
        scc_tcp_tty_t *t = &g_ttys[i];
        t->in_head = t->in_tail = 0;
        t->out_head = t->out_tail = 0;
        pthread_mutex_init(&t->in_lock,  NULL);
        pthread_mutex_init(&t->out_lock, NULL);
        t->client_fd = SOCK_INVALID;
        t->client_addr[0] = '\0';
    }

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

    server_port      = port;
    server_running   = 1;
    shutdown_request = 0;

    if (pthread_create(&accept_thread, NULL, server_thread_func, NULL) != 0) {
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
    int i;
    if (!server_running) return;
    shutdown_request = 1;

    /* Close every active client to unstick its worker, plus the listen
       socket to unstick the accept thread. */
    pthread_mutex_lock(&g_bind_lock);
    for (i = 0; i < SCC_MAX_TTYS; i++) {
        if (g_ttys[i].client_fd != SOCK_INVALID) {
            close_sock(g_ttys[i].client_fd);
            g_ttys[i].client_fd = SOCK_INVALID;
        }
    }
    pthread_mutex_unlock(&g_bind_lock);

    if (listen_fd != SOCK_INVALID) {
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
    }

    pthread_join(accept_thread, NULL);
    server_running = 0;

#ifdef _WIN32
    WSACleanup();
#endif
}
