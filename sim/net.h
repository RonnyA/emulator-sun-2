/*
 * sun-2 emulator — network packet I/O abstraction
 *
 * Backends:
 *   net_bpf.c   — raw BPF (/dev/bpf*), default on macOS / *BSD
 *   net_pcap.c  — libpcap (Linux) / Npcap (Windows)
 *   net_stub.c  — no networking
 *
 * The backend is chosen at compile time via the NET_BACKEND_* macro
 * defined by the Makefile (NET_BACKEND_BPF / NET_BACKEND_PCAP /
 * NET_BACKEND_STUB).
 */

#ifndef SUN2_NET_H
#define SUN2_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct net_iface_s net_iface_t;

/*
 * Open the host network interface used by the emulated 3C400.
 *   iface       host interface name (e.g. "eth0", "en0", "vmnet8").
 *               If NULL, the backend picks a sensible default.
 *   mac         the 6-byte MAC the guest will use; backends may use
 *               this for filtering, but every backend must also
 *               accept broadcast frames.
 *   promiscuous non-zero to request promiscuous mode (deliver all
 *               packets seen on the wire). The 3C400 does its own
 *               software filtering anyway.
 *
 * Returns an opaque handle on success, NULL on failure (a diagnostic
 * is printed via fprintf(stderr,...)).
 */
net_iface_t *net_open(const char *iface, const uint8_t mac[6], int promiscuous);

/*
 * Send one raw ethernet frame (dst MAC | src MAC | ethertype | payload).
 * Returns the number of bytes written, or a negative value on error.
 */
int net_send(net_iface_t *nh, const void *frame, size_t len);

/*
 * Try to receive one packet. Non-blocking.
 *   buf    destination buffer for the raw ethernet frame
 *   maxlen size of buf
 * Returns:
 *   > 0 number of bytes copied to buf
 *     0 no packet available right now
 *   < 0 error
 */
int net_recv(net_iface_t *nh, void *buf, size_t maxlen);

/*
 * Release the interface and any backend resources.
 */
void net_close(net_iface_t *nh);

/*
 * Return a short, human-readable name for the active backend
 * ("bpf", "pcap", "stub"). Used by sun-2 startup banner.
 */
const char *net_backend_name(void);

/*
 * Print a list of host network interfaces the active backend can
 * see, to stdout.  Used by `--net-list` so the user can pick an
 * interface name to pass to `--net-iface`.
 *
 * On pcap (Linux + Windows) this calls pcap_findalldevs.  On BPF
 * it prints a hint to use the host's ifconfig/ip-link tool.  On
 * stub it just notes that networking is disabled.
 */
void net_list_interfaces(void);

#ifdef __cplusplus
}
#endif

#endif /* SUN2_NET_H */
