/*
 * sun-2 emulator — Linux TAP network backend.
 *
 * Built when NET_BACKEND_TAP is defined (Linux only).  Provides the
 * net.h API by attaching to a /dev/net/tun device opened in TAP mode
 * (raw layer-2 ethernet frames, no PI header).
 *
 * Set up the TAP device on the host with `tap.sh` (or any equivalent),
 * then run with: make NET_BACKEND=tap && sim --net-iface=tap0 ...
 *
 * Adapted from the original 3c400.c TAP code by Ronny Hansen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include "net.h"

struct net_iface_s {
    int fd;
    char name[IFNAMSIZ];
};

const char *net_backend_name(void) { return "tap"; }

net_iface_t *net_open(const char *iface, const uint8_t mac[6], int promiscuous)
{
    (void)mac;
    (void)promiscuous;

    if (!iface) iface = "tap0";

    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "net(tap): open /dev/net/tun: %s\n", strerror(errno));
        return NULL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        fprintf(stderr, "net(tap): TUNSETIFF %s: %s\n",
                iface, strerror(errno));
        close(fd);
        return NULL;
    }

    net_iface_t *nh = calloc(1, sizeof(*nh));
    if (!nh) { close(fd); return NULL; }
    nh->fd = fd;
    strncpy(nh->name, ifr.ifr_name, sizeof(nh->name) - 1);

    fprintf(stderr, "net(tap): attached to %s\n", nh->name);
    return nh;
}

int net_send(net_iface_t *nh, const void *frame, size_t len)
{
    if (!nh || nh->fd < 0) return -1;
    return (int)write(nh->fd, frame, len);
}

int net_recv(net_iface_t *nh, void *buf, size_t maxlen)
{
    if (!nh || nh->fd < 0) return -1;
    ssize_t n = read(nh->fd, buf, maxlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return (int)n;
}

void net_close(net_iface_t *nh)
{
    if (!nh) return;
    if (nh->fd >= 0) close(nh->fd);
    free(nh);
}

void net_list_interfaces(void)
{
    printf("net(tap): list with `ip link show type tun` on the host.\n");
    printf("Create one with: sudo ip tuntap add dev tap0 mode tap user $USER\n");
    printf("Then: sudo ip addr add 10.0.2.1/24 dev tap0 && sudo ip link set tap0 up\n");
}
