/*
 * sun-2 emulator — BPF (BSD packet filter) network backend.
 *
 * Built when NET_BACKEND_BPF is defined.  Provides the net.h API
 * on macOS and the *BSDs.  Linux does not have /dev/bpf*; build
 * with NET_BACKEND_PCAP there instead.
 *
 * Adapted from the original 3c400.c BPF code by Sigurbjorn B. Larusson.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <net/bpf.h>
#include <net/if.h>

#include "net.h"

struct net_iface_s {
    int  fd;            /* /dev/bpfN file descriptor */
    int  buflen;        /* BPF kernel buffer size                 */
    char *rxbuf;        /* read() buffer holding bpf_hdr+frame... */
    char *rx_cursor;    /* next packet inside rxbuf, or NULL      */
    char *rx_end;       /* one past end of valid rxbuf data       */
};

const char *net_backend_name(void) { return "bpf"; }

/* Drop everything except packets addressed to us or broadcast.
   The 3c400 does its own software filtering on top of this; this
   filter just keeps host CPU load down. */
static struct bpf_insn bpf_filter_template[] = {
    BPF_STMT(BPF_LD + BPF_W + BPF_ABS, 0),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0,           0, 3),  /* mac top4 */
    BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 4),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0,           5, 0),  /* mac low2 */
    BPF_STMT(BPF_LD + BPF_W + BPF_ABS, 0),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0xFFFFFFFFu, 0, 2),
    BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 4),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0xFFFFu,     1, 0),
    BPF_STMT(BPF_RET + BPF_K, 0),
    BPF_STMT(BPF_RET + BPF_K, (u_int)-1),
};

net_iface_t *net_open(const char *iface, const uint8_t mac[6], int promiscuous)
{
    char devname[32];
    int fd = -1;
    for (int i = 0; i < 99; i++) {
        snprintf(devname, sizeof(devname), "/dev/bpf%d", i);
        fd = open(devname, O_RDWR);
        if (fd >= 0) break;
        if (errno != EBUSY) continue;
    }
    if (fd < 0) {
        fprintf(stderr, "net(bpf): cannot open /dev/bpf*: %s\n", strerror(errno));
        return NULL;
    }

    if (!iface) iface = "en0";
    struct ifreq bindif;
    memset(&bindif, 0, sizeof(bindif));
    strncpy(bindif.ifr_name, iface, sizeof(bindif.ifr_name) - 1);
    if (ioctl(fd, BIOCSETIF, &bindif) < 0) {
        fprintf(stderr, "net(bpf): BIOCSETIF %s: %s\n", iface, strerror(errno));
        close(fd);
        return NULL;
    }

    int one = 1;
    if (ioctl(fd, BIOCIMMEDIATE, &one) < 0) {
        fprintf(stderr, "net(bpf): BIOCIMMEDIATE: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    if (ioctl(fd, FIONBIO, &one) < 0) {
        fprintf(stderr, "net(bpf): FIONBIO: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    /* Patch the filter constants with the real MAC. */
    struct bpf_insn filter[sizeof(bpf_filter_template)/sizeof(bpf_filter_template[0])];
    memcpy(filter, bpf_filter_template, sizeof(filter));
    uint32_t mac_top = ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
                       ((uint32_t)mac[2] <<  8) |  (uint32_t)mac[3];
    uint32_t mac_low = ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
    filter[1].k = mac_top;
    filter[3].k = mac_low;

    if (!promiscuous) {
        struct bpf_program p;
        p.bf_len = sizeof(filter) / sizeof(filter[0]);
        p.bf_insns = filter;
        if (ioctl(fd, BIOCSETF, &p) < 0)
            fprintf(stderr, "net(bpf): BIOCSETF: %s (continuing without filter)\n",
                    strerror(errno));
    } else {
        if (ioctl(fd, BIOCPROMISC, &one) < 0)
            fprintf(stderr, "net(bpf): BIOCPROMISC: %s (continuing)\n", strerror(errno));
    }

    /* Don't let the kernel rewrite our src MAC on output. */
    if (ioctl(fd, BIOCSHDRCMPLT, &one) < 0)
        fprintf(stderr, "net(bpf): BIOCSHDRCMPLT: %s (continuing)\n", strerror(errno));

    int blen = 0;
    if (ioctl(fd, BIOCGBLEN, &blen) < 0 || blen <= 0) {
        fprintf(stderr, "net(bpf): BIOCGBLEN: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    net_iface_t *nh = calloc(1, sizeof(*nh));
    if (!nh) { close(fd); return NULL; }
    nh->fd = fd;
    nh->buflen = blen;
    nh->rxbuf = malloc(blen);
    if (!nh->rxbuf) { close(fd); free(nh); return NULL; }
    nh->rx_cursor = NULL;
    nh->rx_end = NULL;
    return nh;
}

int net_send(net_iface_t *nh, const void *frame, size_t len)
{
    if (!nh) return -1;
    ssize_t r = write(nh->fd, frame, len);
    return (int)r;
}

int net_recv(net_iface_t *nh, void *buf, size_t maxlen)
{
    if (!nh) return -1;

    /* If we don't currently have buffered packets, try to pull more. */
    if (nh->rx_cursor == NULL || nh->rx_cursor >= nh->rx_end) {
        ssize_t n = read(nh->fd, nh->rxbuf, nh->buflen);
        if (n <= 0) {
            nh->rx_cursor = NULL;
            nh->rx_end = NULL;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            return (n < 0) ? -1 : 0;
        }
        nh->rx_cursor = nh->rxbuf;
        nh->rx_end = nh->rxbuf + n;
    }

    struct bpf_hdr *bh = (struct bpf_hdr *)nh->rx_cursor;
    size_t copy = bh->bh_caplen;
    if (copy > maxlen) copy = maxlen;
    memcpy(buf, nh->rx_cursor + bh->bh_hdrlen, copy);

    nh->rx_cursor += BPF_WORDALIGN(bh->bh_hdrlen + bh->bh_caplen);
    return (int)copy;
}

void net_close(net_iface_t *nh)
{
    if (!nh) return;
    if (nh->fd >= 0) close(nh->fd);
    if (nh->rxbuf) free(nh->rxbuf);
    free(nh);
}

void net_list_interfaces(void)
{
    /* The BPF backend doesn't enumerate interfaces itself — host tools
       are better at it.  Tell the user to run ifconfig / ip link. */
    fprintf(stderr,
            "net(bpf): the BPF backend doesn't enumerate host interfaces.\n"
            "  Use one of:\n"
            "    ifconfig -a       (macOS / *BSD)\n"
            "    ip -o link show   (Linux, but Linux uses NET_BACKEND=pcap by default)\n"
            "  ...then pass the chosen name with --net-iface=NAME .\n");
}
