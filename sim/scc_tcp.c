/*
 * sun-2 emulator — SCC channel-A console over TCP
 *
 * Single-client TCP server that bridges:
 *
 *   client ─────► input_buf  ─(scc_tcp_poll, main thread)─►  scc_in_push(ch=0)
 *   client ◄───── output_buf ◄─(scc_tcp_send_byte, emu thread)── scc_wr_data ch=0/1
 *
 * Use `--scc-tcp` (default port 9900) or `--scc-tcp=PORT` on the
 * sim.exe command line.  Connect with:
 *
 *   telnet localhost 9900
 *   nc     localhost 9900
 *
 * The server is raw bytes both ways — no telnet IAC negotiation.
 * Connecting with `telnet` may emit a few negotiation bytes at the
 * start (the client trying to do option negotiation that we ignore);
 * SunOS's tty discipline mostly ignores them.  `nc` avoids that.
 *
 * Pattern lifted from nd100x's telnetserver.c (same author) but
 * stripped to single client + no menu + no IAC parser.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

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

/* SCC primitives we hand bytes to.  Implemented in scc.c.  Not
   thread-safe to call directly from our worker thread, so input
   from the network is staged in input_buf and drained on the main
   thread via scc_tcp_poll(). */
extern void scc_in_push(int ch, int v);
extern void scc_throw_interrupt(int ch, int which);

/* ───── ring buffers ───── */
#define SCC_TCP_IN_SIZE   256
#define SCC_TCP_OUT_SIZE  4096

static uint8_t input_buf[SCC_TCP_IN_SIZE];
static volatile int input_head, input_tail;
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t output_buf[SCC_TCP_OUT_SIZE];
static volatile int output_head, output_tail;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

/* ───── server state ───── */
static volatile int  server_running = 0;
static volatile int  shutdown_request = 0;
static int           server_port = 0;
static sock_t        listen_fd = SOCK_INVALID;
static volatile sock_t client_fd = SOCK_INVALID;   /* 0 or 1 connected client */
static pthread_t     server_thread;

/* ───── ring-buffer helpers (caller holds the relevant lock) ───── */
static int rb_put(uint8_t *buf, int size, volatile int *head, int tail, uint8_t b)
{
    int next = (*head + 1) % size;
    if (next == tail) return 0;        /* full — drop */
    buf[*head] = b;
    *head = next;
    return 1;
}

static int rb_get_block(uint8_t *buf, int size, int head, volatile int *tail,
                        uint8_t *out, int max)
{
    int n = 0;
    while (n < max && *tail != head) {
        out[n++] = buf[*tail];
        *tail = (*tail + 1) % size;
    }
    return n;
}

/* ───── public API ───── */

/* Diagnostic counters — when SCC_TCP_TRACE=1 is set in the environment,
   every call to scc_tcp_send_byte and every TCP recv/send is logged
   to stderr.  Helps prove which side of the pipe is silent. */
static int  scc_tcp_trace = 0;
static long bytes_from_scc;     /* scc_wr_data → scc_tcp_send_byte */
static long bytes_to_client;    /* server thread → send() */
static long bytes_from_client;  /* server thread ← recv() */
static long bytes_to_scc;       /* scc_tcp_poll → scc_in_push */

/* SCC channel that maps to ttya (per RetroCore MachineSun2Memory.cs:
   serial port SCC offset 0x06 = DA = data channel A = ttya).  In our
   scc.c enumeration this is index 1 (`w+1` from `case 6:`).  Channel 0
   in our enumeration is ttyb, which is binary noise during PROM probe
   — don't forward it to the TCP client. */
#define SCC_TTYA_CH 1

void scc_tcp_send_byte(int ch, uint8_t byte)
{
    /* Only forward ttya (ch 1).  ttyb on ch 0 is rarely interesting and
       it just clutters the telnet stream with PROM probe bytes. */
    if (ch != SCC_TTYA_CH) return;
    if (!server_running)   return;

    pthread_mutex_lock(&output_lock);
    rb_put(output_buf, SCC_TCP_OUT_SIZE, &output_head, output_tail, byte);
    pthread_mutex_unlock(&output_lock);
    bytes_from_scc++;
    if (scc_tcp_trace)
        fprintf(stderr, "scc-tcp: send_byte ch=%d 0x%02x\n", ch, byte);
}

void scc_tcp_poll(void)
{
    if (!server_running) return;

    pthread_mutex_lock(&input_lock);
    while (input_tail != input_head) {
        uint8_t b = input_buf[input_tail];
        input_tail = (input_tail + 1) % SCC_TCP_IN_SIZE;
        /* Push to ttya (= our SCC channel 1, see SCC_TTYA_CH comment
           above).  Throw the rx-char interrupt so the PROM/SunOS knows
           there's a byte ready in the channel-1 FIFO. */
        scc_in_push(SCC_TTYA_CH, b);
        /* which=2 = RX char available (per scc_throw_interrupt's
           encoding -- which=1 is TX-empty, which=2 is RX).  This
           used to incorrectly send TX-empty here; SunOS would never
           realize a byte had arrived and console input was dead. */
        scc_throw_interrupt(SCC_TTYA_CH, 2);
        bytes_to_scc++;
        if (scc_tcp_trace)
            fprintf(stderr, "scc-tcp: poll -> scc_in_push(%d, 0x%02x)\n",
                    SCC_TTYA_CH, b);
    }
    pthread_mutex_unlock(&input_lock);
}

/* ───── server thread ───── */

static void serve_client(sock_t cfd)
{
    /* select() loop: 50 ms timeout so we can drain output_buf without
       blocking on read forever. */
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
            if (n <= 0) break;        /* client disconnected */
            bytes_from_client += n;
            pthread_mutex_lock(&input_lock);
            for (int i = 0; i < n; i++)
                rb_put(input_buf, SCC_TCP_IN_SIZE, &input_head, input_tail,
                       (uint8_t)buf[i]);
            pthread_mutex_unlock(&input_lock);
            if (scc_tcp_trace)
                fprintf(stderr, "scc-tcp: recv %d bytes from client\n", n);
        }

        /* Drain output ringbuf to client. */
        uint8_t out[256];
        pthread_mutex_lock(&output_lock);
        int outlen = rb_get_block(output_buf, SCC_TCP_OUT_SIZE,
                                  output_head, &output_tail,
                                  out, (int)sizeof(out));
        pthread_mutex_unlock(&output_lock);
        if (outlen > 0) {
            int sent = send(cfd, (const char *)out, outlen, MSG_NOSIGNAL);
            if (sent < 0) break;      /* client gone */
            bytes_to_client += sent;
            if (scc_tcp_trace)
                fprintf(stderr, "scc-tcp: send %d bytes to client\n", sent);
        }
    }
}

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
        if (cfd == SOCK_INVALID) {
            /* listen_fd closed by scc_tcp_stop, bail out. */
            break;
        }

        /* Disable Nagle for snappier interactive typing. */
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&one, sizeof(one));

        client_fd = cfd;
        char addr[64];
        snprintf(addr, sizeof(addr), "%s:%u",
                 inet_ntoa(caddr.sin_addr), (unsigned)ntohs(caddr.sin_port));
        fprintf(stderr, "scc-tcp: client connected from %s\n", addr);

        /* Greeting */
        const char *hi =
            "\r\n[connected to sun-2 SCC console — Ctrl-] q  to disconnect from telnet]\r\n";
        send(cfd, hi, (int)strlen(hi), MSG_NOSIGNAL);

        serve_client(cfd);

        close_sock(cfd);
        client_fd = SOCK_INVALID;
        fprintf(stderr, "scc-tcp: client %s disconnected\n", addr);
    }
    return NULL;
}

/* ───── lifecycle ───── */

int scc_tcp_start(int port)
{
    if (server_running) return 0;

    /* Verbose tracing on every byte through the pipe — useful for
       proving "no SunOS data" vs "TCP server broken". */
    {
        const char *e = getenv("SCC_TCP_TRACE");
        scc_tcp_trace = (e && *e && *e != '0');
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
    /* SIGPIPE on send-to-closed-socket would kill us; ignore globally.
       On Linux we'd rely on MSG_NOSIGNAL but macOS doesn't have it. */
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
    if (listen(listen_fd, 1) < 0) {
        fprintf(stderr, "scc-tcp: listen() failed\n");
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
        return -1;
    }

    server_port      = port;
    server_running   = 1;
    shutdown_request = 0;

    if (pthread_create(&server_thread, NULL, server_thread_func, NULL) != 0) {
        fprintf(stderr, "scc-tcp: pthread_create failed\n");
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
        server_running = 0;
        return -1;
    }

    fprintf(stderr, "scc-tcp: listening on port %d (raw TCP, single client)\n",
            port);
    return 0;
}

void scc_tcp_stop(void)
{
    if (!server_running) return;
    shutdown_request = 1;

    /* Drop client + listen socket so the thread's accept/recv unblock. */
    if (client_fd != SOCK_INVALID) {
        close_sock(client_fd);
        client_fd = SOCK_INVALID;
    }
    if (listen_fd != SOCK_INVALID) {
        close_sock(listen_fd);
        listen_fd = SOCK_INVALID;
    }

    pthread_join(server_thread, NULL);
    server_running = 0;

#ifdef _WIN32
    WSACleanup();
#endif
}
