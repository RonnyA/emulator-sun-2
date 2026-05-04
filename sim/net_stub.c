/*
 * sun-2 emulator — null network backend.
 *
 * Built when NET_BACKEND_STUB is defined.  The 3c400 device still
 * appears to the guest, but every send is silently dropped and
 * receive never reports a packet.  Use this when libpcap/Npcap is
 * not available at build time.
 */

#include <stdio.h>
#include <stdlib.h>

#include "net.h"

struct net_iface_s {
    int dummy;
};

const char *net_backend_name(void) { return "stub"; }

net_iface_t *net_open(const char *iface, const uint8_t mac[6], int promiscuous)
{
    (void)iface; (void)mac; (void)promiscuous;
    fprintf(stderr,
            "net(stub): networking is disabled in this build "
            "(no libpcap/BPF). 3C400 will see no packets.\n");
    net_iface_t *nh = calloc(1, sizeof(*nh));
    return nh;
}

int net_send(net_iface_t *nh, const void *frame, size_t len)
{
    (void)nh; (void)frame;
    return (int)len;
}

int net_recv(net_iface_t *nh, void *buf, size_t maxlen)
{
    (void)nh; (void)buf; (void)maxlen;
    return 0;
}

void net_close(net_iface_t *nh)
{
    free(nh);
}

void net_list_interfaces(void)
{
    fprintf(stderr,
            "net(stub): networking is disabled in this build.\n"
            "  Rebuild with NET_BACKEND=pcap (after `make fetch-npcap-sdk` on Windows)\n"
            "  or NET_BACKEND=bpf (macOS/BSD) to enumerate interfaces.\n");
}
