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

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>      /* inet_ntop */
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#include <pcap.h>

#include "net.h"

struct net_iface_s {
    pcap_t *p;
    char    errbuf[PCAP_ERRBUF_SIZE];
    uint8_t mac[6];   /* our MAC, used to drop self-echoes from promisc capture */
    int     have_mac; /* mac[] was supplied at open time */
};

const char *net_backend_name(void) { return "pcap"; }

/* True if s is a non-empty string of ASCII digits. */
static int is_decimal_index(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/*
 * Resolve the user-supplied --net-iface value into the actual device
 * name pcap_open_live wants.
 *
 *   user == NULL           pick the first non-loopback adapter
 *   user is "1".."N"       use the Nth entry from pcap_findalldevs
 *                          (matches what `--net-list` printed)
 *   user is anything else  pass through as a literal name
 *
 * Returns a malloc'd string the caller must free.  Returns NULL and
 * prints a diagnostic if no interface could be resolved.
 */
static char *resolve_iface(const char *user, char *errbuf)
{
    /* Strip leading/trailing whitespace from the user-supplied name
       so a stray space (env var, copy/paste) doesn't become a literal
       part of the adapter name. */
    char *trimmed = NULL;
    if (user) {
        while (*user == ' ' || *user == '\t' || *user == '\n' || *user == '\r')
            user++;
        if (*user == '\0') {
            user = NULL;
        } else {
            trimmed = strdup(user);
            char *end = trimmed + strlen(trimmed);
            while (end > trimmed && (end[-1] == ' ' || end[-1] == '\t' ||
                                     end[-1] == '\n' || end[-1] == '\r'))
                *--end = '\0';
            user = trimmed;
        }
    }

    /* For literal device names we don't enumerate — Npcap names are
       opaque GUIDs and Linux names are short, but either way pcap
       takes them verbatim. */
    if (user && !is_decimal_index(user)) {
        char *ret = strdup(user);
        free(trimmed);
        return ret;
    }

    pcap_if_t *all = NULL;
    if (pcap_findalldevs(&all, errbuf) != 0 || !all) {
        fprintf(stderr, "net(pcap): no usable interfaces (%s)\n", errbuf);
        free(trimmed);
        return NULL;
    }

    char *picked = NULL;

    if (user) {
        int want = atoi(user);
        int n = 0;
        for (pcap_if_t *d = all; d != NULL; d = d->next) {
            if (++n == want) {
                picked = strdup(d->name);
                fprintf(stderr, "net(pcap): --net-iface=%d -> %s\n",
                        want, d->name);
                break;
            }
        }
        if (!picked)
            fprintf(stderr,
                    "net(pcap): no interface at index %d "
                    "(run --net-list to see %d available)\n", want, n);
    } else {
        /* Auto-pick: prefer the first non-loopback device. */
        for (pcap_if_t *d = all; d != NULL; d = d->next) {
            if (d->flags & PCAP_IF_LOOPBACK) continue;
            picked = strdup(d->name);
            break;
        }
        if (!picked)
            picked = strdup(all->name);
    }

    pcap_freealldevs(all);
    free(trimmed);
    return picked;
}

net_iface_t *net_open(const char *iface, const uint8_t mac[6], int promiscuous)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    char *picked = resolve_iface(iface, errbuf);
    if (!picked) return NULL;

    /* snaplen 2048 matches the 3c400 receive buffer; timeout 1ms keeps
       pcap_next_ex() from blocking the emulator's main loop. */
    pcap_t *p = pcap_open_live(picked,
                               2048,
                               promiscuous ? 1 : 0,
                               1 /* read timeout, ms */,
                               errbuf);
    if (!p) {
        fprintf(stderr, "net(pcap): pcap_open_live(%s): %s\n", picked, errbuf);
        free(picked);
        return NULL;
    }

#ifdef HAVE_PCAP_SETNONBLOCK
    if (pcap_setnonblock(p, 1, errbuf) < 0)
        fprintf(stderr, "net(pcap): pcap_setnonblock: %s (continuing)\n", errbuf);
#else
    pcap_setnonblock(p, 1, errbuf);
#endif

    fprintf(stderr, "net(pcap): bound to %s\n", picked);
    free(picked);

    net_iface_t *nh = calloc(1, sizeof(*nh));
    if (!nh) { pcap_close(p); return NULL; }
    nh->p = p;
    if (mac) {
        memcpy(nh->mac, mac, 6);
        nh->have_mac = 1;
    }
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

    /* Loop: drop self-echoes (pcap in promisc mode sees our own TX) and
       try the next packet.  Bound the inner loop so an extremely chatty
       host (our MAC spoofed back to us, weird capture loops, etc.)
       can't stall the emulator's main loop. */
    int got = 0;
    for (int attempts = 0; attempts < 16; attempts++) {
        int r = pcap_next_ex(nh->p, &hdr, &data);
        if (r == 0) return 0;          /* timeout / no packet right now */
        if (r < 0)  return -1;         /* error */

        /* Anti-echo filter: drop frames whose source MAC is our own.
           Frame layout: dst[0..5] | src[6..11] | ...  Need at least 12
           bytes to even check. */
        if (nh->have_mac && hdr->caplen >= 12 &&
            data[6]  == nh->mac[0] && data[7]  == nh->mac[1] &&
            data[8]  == nh->mac[2] && data[9]  == nh->mac[3] &&
            data[10] == nh->mac[4] && data[11] == nh->mac[5]) {
            continue;  /* our own TX; skip */
        }
        got = 1;
        break;
    }
    if (!got) return 0;  /* every attempt was a self-echo; come back next tick */

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

/* Count the number of leading 1-bits in a 32-bit netmask, e.g.
   0xffffff00 -> 24.  Used to render IPv4 netmasks as CIDR. */
static int netmask_to_cidr(uint32_t mask_host_order)
{
    int bits = 0;
    while (mask_host_order & 0x80000000u) {
        bits++;
        mask_host_order <<= 1;
    }
    return bits;
}

static void print_iface_address(const struct pcap_addr *a)
{
    if (!a || !a->addr) return;

    char addr_str[INET6_ADDRSTRLEN] = "?";

    if (a->addr->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)a->addr;
        inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));

        if (a->netmask) {
            struct sockaddr_in *m = (struct sockaddr_in *)a->netmask;
            uint32_t mask = ntohl(m->sin_addr.s_addr);
            int cidr = netmask_to_cidr(mask);

            /* Compute the network address: IP & netmask. */
            uint32_t ip_h = ntohl(sin->sin_addr.s_addr);
            uint32_t net = ip_h & mask;
            uint8_t b0 = (net >> 24) & 0xff;
            uint8_t b1 = (net >> 16) & 0xff;
            uint8_t b2 = (net >>  8) & 0xff;
            uint8_t b3 =  net        & 0xff;
            printf("     IPv4: %s/%d  (network %u.%u.%u.%u/%d)\n",
                   addr_str, cidr, b0, b1, b2, b3, cidr);
        } else {
            printf("     IPv4: %s\n", addr_str);
        }
    } else if (a->addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)a->addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str));
        printf("     IPv6: %s\n", addr_str);
    }
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

        /* libpcap attaches every IPv4 / IPv6 / link-layer address the
           OS knows about for this device.  Print the IP-level ones so
           the user can match adapter names to a network. */
        for (pcap_addr_t *a = d->addresses; a != NULL; a = a->next) {
            print_iface_address(a);
        }
    }
    if (n == 0)
        fprintf(stderr, "  (none — on Windows, install Npcap; on Linux, "
                        "libpcap usually needs CAP_NET_RAW or root)\n");
    pcap_freealldevs(all);
}
