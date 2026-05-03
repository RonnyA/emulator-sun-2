/*
 * sun-2 emulator — libpcap / Npcap network backend.
 *
 * Built when NET_BACKEND_PCAP is defined.  Provides the net.h API
 * on Linux (libpcap) and Windows (Npcap, with the libpcap-compatible
 * SDK installed under C:\Program Files\Npcap or via vcpkg/MSYS2).
 *
 * Untested on real hardware in the current refactor — the protocol
 * surface is small (open + send + non-blocking recv) so this should
 * be straightforward, but treat as alpha-quality network support
 * the same way the original BPF code was advertised.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pcap.h>

#include "net.h"

struct net_iface_s {
    pcap_t *p;
    char    errbuf[PCAP_ERRBUF_SIZE];
};

const char *net_backend_name(void) { return "pcap"; }

/* If iface == NULL, pick the first non-loopback device pcap can see. */
static char *pcap_pick_default(char *errbuf)
{
    pcap_if_t *all = NULL;
    if (pcap_findalldevs(&all, errbuf) != 0 || !all)
        return NULL;

    char *picked = NULL;
    for (pcap_if_t *d = all; d != NULL; d = d->next) {
        if (d->flags & PCAP_IF_LOOPBACK) continue;
        picked = strdup(d->name);
        break;
    }
    if (!picked && all)
        picked = strdup(all->name);

    pcap_freealldevs(all);
    return picked;
}

net_iface_t *net_open(const char *iface, const uint8_t mac[6], int promiscuous)
{
    (void)mac; /* pcap doesn't need the MAC; the 3c400 filters in software */

    char errbuf[PCAP_ERRBUF_SIZE];
    char *picked = NULL;

    if (!iface) {
        picked = pcap_pick_default(errbuf);
        if (!picked) {
            fprintf(stderr, "net(pcap): no usable interfaces (%s)\n", errbuf);
            return NULL;
        }
        iface = picked;
    }

    /* snaplen 2048 matches the 3c400 receive buffer; timeout 1ms keeps
       pcap_next_ex() from blocking the emulator's main loop. */
    pcap_t *p = pcap_open_live(iface,
                               2048,
                               promiscuous ? 1 : 0,
                               1 /* read timeout, ms */,
                               errbuf);
    if (!p) {
        fprintf(stderr, "net(pcap): pcap_open_live(%s): %s\n", iface, errbuf);
        free(picked);
        return NULL;
    }

#ifdef HAVE_PCAP_SETNONBLOCK
    if (pcap_setnonblock(p, 1, errbuf) < 0)
        fprintf(stderr, "net(pcap): pcap_setnonblock: %s (continuing)\n", errbuf);
#else
    pcap_setnonblock(p, 1, errbuf);
#endif

    fprintf(stderr, "net(pcap): bound to %s\n", iface);
    free(picked);

    net_iface_t *nh = calloc(1, sizeof(*nh));
    if (!nh) { pcap_close(p); return NULL; }
    nh->p = p;
    return nh;
}

int net_send(net_iface_t *nh, const void *frame, size_t len)
{
    if (!nh || !nh->p) return -1;
    int r = pcap_sendpacket(nh->p, (const u_char *)frame, (int)len);
    return (r == 0) ? (int)len : -1;
}

int net_recv(net_iface_t *nh, void *buf, size_t maxlen)
{
    if (!nh || !nh->p) return -1;

    struct pcap_pkthdr *hdr;
    const u_char *data;
    int r = pcap_next_ex(nh->p, &hdr, &data);
    if (r == 0) return 0;          /* timeout / no packet */
    if (r < 0)  return -1;         /* error */

    size_t copy = hdr->caplen;
    if (copy > maxlen) copy = maxlen;
    memcpy(buf, data, copy);
    return (int)copy;
}

void net_close(net_iface_t *nh)
{
    if (!nh) return;
    if (nh->p) pcap_close(nh->p);
    free(nh);
}

void net_list_interfaces(void)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_if_t *all = NULL;
    if (pcap_findalldevs(&all, errbuf) != 0 || !all) {
        fprintf(stderr, "net(pcap): pcap_findalldevs: %s\n", errbuf);
        return;
    }
    printf("Available network interfaces (pass to --net-iface):\n");
    int n = 0;
    for (pcap_if_t *d = all; d != NULL; d = d->next) {
        n++;
        const char *flags = (d->flags & PCAP_IF_LOOPBACK) ? " [loopback]" : "";
        printf("  %d. %s%s\n", n, d->name, flags);
        if (d->description && d->description[0])
            printf("     %s\n", d->description);
    }
    if (n == 0)
        fprintf(stderr, "  (none — on Windows, install Npcap; on Linux, "
                        "libpcap usually needs CAP_NET_RAW or root)\n");
    pcap_freealldevs(all);
}
