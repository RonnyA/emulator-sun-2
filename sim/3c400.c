/* An alpha version of an 3c400 ethernet adapter for the sun2-emulator 
   2019 - Sigurbjorn B. Larusson (sibbi@dot1q.org)

   I built this drivver from scratch from the 3C400 reference manual, mostly as an interesting project, hope you find some use for it
  
   It works, you need BPF to use it.  Broadcast and BPF aren't always friends, you probably have to add a static arp entry for your host
   to the emulated sun.  You can still compile without BPF support, obviously you can't read or write to the network, but you can enable
   tracing and see what the driver is doing in the background.

   It should be trivial to port this to use some other mechanism than bpf, the bpf code itself is a very minor part of this


   To do that, add an entry into /etc/hosts with your hostname and ip address and then use
   arp -s hostname aa:bb:cc:dd:ee:ff

   You might also want to do the same on the machine you're running on but pointing to the sun machine mac-address, i.e.
   arp -s ipaddress.of.emulated.sun 08:00:20:01:06:e0  (this assumes you didn't change the default mac-address)	

   Your milage with using BPF to reach ouside of the emulated machine might vary, I've been successful in using ftp and telnet from the sun
   to the host running the emulator and telnet in the other direction 

   Since the I/O read in the emulator is basically an endless loop this driver only runs the scan for new packets every 5000 iterations of that
   loop, you can set it with a define right below here.  If you set it too low, the emulator will slow to a crawl since the packet checking code
   takes a good while to load (BPF read function uses 90% of that time).  If you set it to high, the network will be slow with lots of retransmissions

   TODO: It would be far better to use software interrupts here, BPF on the mac, at least upto version 10.14, does not support them
         but other BSD OS's do, so I'd like to test this and find out which ones it works well on, that is much better solution than 
	 skipping 4999 iterations of the io_update code.

   TODO: This code really should be a seperate thread 

   TODO: This code could probably be optimized quite a bit
  
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sim68k.h"
#include "m68k.h"
#include "sim.h"
#include "net.h"

// This is not a good place but here is the configuration for the mac-address and the BPF device and network device you wish to use
// TODO: There probably should be a configuration file to set this
// Default is 08:00:20:01:06:e0, same as the emulator default
#define MAC1 0x08;
#define MAC2 0x00;
#define MAC3 0x20;
#define MAC4 0x01;
#define MAC5 0x06;
#define MAC6 0xe0;

// Legacy fallback host interface (sigurbjornl's macOS+VMWare default).
// Only used when the user neither passes --net-iface nor sets the
// SUN2_NET_IFACE env var AND we're on a host where the backend can't
// auto-pick.  Pass NULL to net_open() to let the backend auto-pick:
// pcap picks the first non-loopback adapter, BPF uses en0.
#define BPFINTERFACE_LEGACY "vmnet8"

// Try to make sure struct is correctly represented
#pragma pack(1)

// ADDRESSING:
// 	MEBASE starts 0xe000, followed by:
//  	Status Register, two bytes, various bits for status and control	
//  	Station ROM address, offset 400, stores the ROM mac-address
//	Station RAM address, offset 600, stores the RAM mac-address (set by computer, used by adapter)
//	Transmit buffer, offset 800, stores header and packet to be transmitted
//	Receive buffer A, offset 1000, stores header and packet received in buffer A
//	Receive buffer B, offset 1800, stores header and packet received in buffer B



//  Control and status register bitfield
struct control_and_status_register {
	// The first two bytes called the control and status register represent control bits used by the controller and user
        // PA controls which packets the workstation is interested in recieving
	// Errors = Runts/Oversize, FCS (Frame Check Sequence errors), Frame (Alignment Errors)
	// Multicast is here Multicast and Broadcast
	// 0 = all packets (promiscious), 1 = all packets-errors, 2 = all packets - fcs - frame
	// 3 = mypackets + multi, 4 = mypackets + multi - errors 5 = mypackets + multi - fcs - frame
	// 6 = mypackets + broadcast, 7 = mypackets + broadcast - errors 8 = mypackets + broadcast - fcs - frame
	uint8_t pa : 4; 
	// Next up is the JINTEN bit, or JAM interrupt enable, if this is set then an interrupt occurs when the JAM bit is set (a collision occured)
	uint8_t jinten : 1;
	// It's brother the TINTEN bit, or Transmit interrupt enabled, interrupts when the transmit buffer has been emptied (TBSW becomes 0) 
	uint8_t tinten : 1;
	// Their sibling is the AINTEN bit, the AINTEN bit enables an interrupt when ABSW becomes zero (a packet has been received in the A buffer)
	uint8_t ainten : 1;
	// AINTEN's twin is the BINTEN, it enables an interrupt when BBSW becomes zero (a packet has been received in the B buffer)
	uint8_t binten : 1;
	// RESET, always reads as zero, when written to, will perform a reset which sets ABSW, BBSW and TBSW to zero, this is the only way to recover from a failure of >15 transmission errors
	uint8_t reset : 1;
	// Bit number 9, is not used
	uint8_t unused : 1;
	// RBBA holds onto whether the A or B receive buffer has been used more recently, when a packet arrives in B RBBA is set to zero
	// If a packet arrives in A, RBBA is set to 1, if both buffers are receive enabled, the buffer with the older packet will receive the new one  
	// If RBBA is 0, then the packet goes to the A buffer, if RBBA is 1, then the packet goes into the B buffer
	uint8_t rbba : 1;
	// Next up is the address memory switch, it is set to one, by software, when the mac-address of this controller has been written into the address ram
	// Once set, the driver will use that address to determine which packets are "mypackets"
	uint8_t amsw : 1;
	//  JAM bit, set to 1 if transmission error occurs, software must write a delay multplier into MEBACK and then 1 into JAM to clear JAM (retry transmission)
	uint8_t jam : 1;
	// TBSW, set to 1 when you wish to transmit a packet, MEXHDR must first be set to the offset of the transmit buffer to the start of the packet
	// Next the packet must be written to that offset and until the end of the buffer, and then finally TBSW set to 1, when the packet has been transmitted
	// TBSW will return to zero, if tinten is enabled, an interrupt will also occur 
	uint8_t tbsw : 1;
	// ABSW, set to 1 when you wish to receive a packet in receive buffer A, 
	uint8_t absw : 1;
	// BBSW, set to 1 when you wish to receive a packet in receive buffer B, note that if both AB and BB are set, then RBBA sequence describer earlier, is used to
	// select which buffer gets the data
	uint8_t bbsw : 1;
};

struct transmit_header {
	// This is the start of the transmit buffer, @MEBASE+800
	uint16_t firstbyte : 11;
	// Next five bits are unused
	uint8_t unused : 5;
};

/*
 * Receive-header bit masks — explicit bit ops on a uint16_t.  This
 * replaces the previous bitfield struct because gcc's mixed-type
 * bitfield rules put the uint8_t flag bits in a separate byte that
 * fell outside the union's uint16_t accessor — so SunOS read the
 * firstfree word with all flag bits zeroed, computed a bogus length,
 * and logged "ec0: garbled packet" for every received frame.
 *
 * Layout (matches RetroCore E3C400Chip):
 *   bit 15  FCSERROR   1 = FCS / CRC error
 *   bit 14  BROADCAST  1 = dst MAC is FF:FF:FF:FF:FF:FF
 *   bit 13  RANGEERR   1 = range error
 *   bit 12  ADDRMATCH  1 = dst MAC matches our station MAC
 *   bit 11  FRAMINGERR 1 = framing error
 *   bit 10..0  DOFF    offset of first free byte in the buffer (= 2 + frame_len + 4)
 */
#define RX_DOFF       0x07FFu
#define RX_FRAMINGERR 0x0800u
#define RX_ADDRMATCH  0x1000u
#define RX_RANGEERR   0x2000u
#define RX_BROADCAST  0x4000u
#define RX_FCSERR     0x8000u

struct mac_addr {
	uint8_t o1;
	uint8_t o2;
	uint8_t o3;
	uint8_t o4;
	uint8_t o5;
	uint8_t o6;
	uint8_t o7;
	uint8_t o8;
};

// The point of the unions is to enable us to return whole bitfields, while still being able to manipulate members within them without using binary operators

union csr {
	struct control_and_status_register csr;
	uint16_t csrword;
};

union macaddr {
	struct mac_addr addr;
};

union txhdr {
	struct transmit_header hdr;
	uint16_t header;
};

/* Just a uint16_t now; was a bitfield+union but gcc x86 didn't lay
   the uint8_t flag bitfields out where SunOS expects them. */
typedef uint16_t rxhdr_t;

static inline unsigned rxhdr_doff(const rxhdr_t *h)        { return *h & RX_DOFF; }
static inline void     rxhdr_set_doff(rxhdr_t *h, unsigned v) { *h = (*h & ~RX_DOFF) | (v & RX_DOFF); }
static inline void     rxhdr_add_doff(rxhdr_t *h, int n)   { rxhdr_set_doff(h, rxhdr_doff(h) + n); }
static inline void     rxhdr_set_flag(rxhdr_t *h, uint16_t m, int v) { *h = v ? (*h | m) : (*h & ~m); }
static inline int      rxhdr_get_flag(const rxhdr_t *h, uint16_t m) { return (*h & m) != 0; }

// Variable for the status register
union csr *mecsr;

// Retransmission backoff counter
uint16_t meback;

// This is the start of the 3c400 rom, @MEBASE+400
union macaddr *romaddr;

// This is the station address ram, @MEBASE+600, it holds the station's mac address, used by the adapter to determine which packets are "ours"
union macaddr *ramaddr;

// This is the adapter's mac-address once set
unsigned char macaddr[7];


// Transmit header
union txhdr *mexhdr;
// Transmit buffer
unsigned char *mexbuffer;

// Receive headers A and B
rxhdr_t *meahdr;
rxhdr_t *mebhdr;

// Receive buffers
unsigned char *meabuffer;
unsigned char *mebbuffer;

//  Trace positive means outputting lots of debug information on the lowlevel processing of the emulation, best enabled by using the e3c400_enable_trace function...
int trace_3c400 = 0;

// `-q` on the cli; gates the noisier always-on diagnostics.
extern int quiet;

// Active network backend handle.  NULL when networking is disabled
// or the host couldn't be opened (e.g. permissions on /dev/bpf*,
// missing libpcap, no Npcap driver, etc).
static net_iface_t *netif = NULL;

// Dump a single Ethernet frame to stderr in a one-line summary.  Caller
// provides the leading "[3c400 ...]" tag so different sites can include
// extra context (PA, buffer A/B, deliver/drop).  Used by --net-dump.
//
// Decodes:
//   ARP (0x0806)            -> "ARP req/reply  sender=IP target=IP"
//   IPv4 / TCP   (0x0800,6) -> "src.ip:port -> dst.ip:port  TCP [SYN|ACK|...]"
//   IPv4 / UDP   (0x0800,17)-> "src.ip:port -> dst.ip:port  UDP"
//   IPv4 / ICMP  (0x0800,1) -> "src.ip -> dst.ip  ICMP type=N code=N"
//   anything else            -> just src/dst MAC + ethertype
static void dump_frame(const char *tag, const unsigned char *f, int len)
{
	if (len < 14) {
		fprintf(stderr, "%s runt frame, len=%d\n", tag, len);
		return;
	}
	unsigned ethtype = (f[12] << 8) | f[13];

	/* Common L2 prefix. */
	fprintf(stderr,
		"%s len=%d  %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x  type=%04x",
		tag, len,
		f[6], f[7], f[8], f[9], f[10], f[11],          /* src first, more readable */
		f[0], f[1], f[2], f[3], f[4], f[5],
		ethtype);

	/* IPv4 */
	if (ethtype == 0x0800 && len >= 14 + 20) {
		const unsigned char *ip = f + 14;
		unsigned ihl = (ip[0] & 0x0f) * 4;
		unsigned proto = ip[9];
		unsigned tlen = (ip[2] << 8) | ip[3];
		const char *pname =
			proto == 1  ? "ICMP" :
			proto == 6  ? "TCP"  :
			proto == 17 ? "UDP"  : NULL;

		fprintf(stderr, "  %u.%u.%u.%u",
			ip[12], ip[13], ip[14], ip[15]);
		fprintf(stderr, " -> %u.%u.%u.%u",
			ip[16], ip[17], ip[18], ip[19]);
		if (pname) fprintf(stderr, "  %s", pname);
		else       fprintf(stderr, "  proto=%u", proto);
		fprintf(stderr, " tlen=%u", tlen);

		const unsigned char *l4 = f + 14 + ihl;
		int l4_avail = len - (14 + ihl);
		if (proto == 6 && l4_avail >= 14) {       /* TCP */
			unsigned sport = (l4[0] << 8) | l4[1];
			unsigned dport = (l4[2] << 8) | l4[3];
			unsigned char flags = l4[13];
			fprintf(stderr, " %u->%u flags=", sport, dport);
			if (flags & 0x01) fprintf(stderr, "F");
			if (flags & 0x02) fprintf(stderr, "S");
			if (flags & 0x04) fprintf(stderr, "R");
			if (flags & 0x08) fprintf(stderr, "P");
			if (flags & 0x10) fprintf(stderr, "A");
			if (flags & 0x20) fprintf(stderr, "U");
			if (!flags) fprintf(stderr, "-");
		} else if (proto == 17 && l4_avail >= 4) { /* UDP */
			unsigned sport = (l4[0] << 8) | l4[1];
			unsigned dport = (l4[2] << 8) | l4[3];
			fprintf(stderr, " %u->%u", sport, dport);
		} else if (proto == 1 && l4_avail >= 4) {  /* ICMP */
			fprintf(stderr, " icmp_type=%u code=%u", l4[0], l4[1]);
		}
	}
	/* ARP */
	else if (ethtype == 0x0806 && len >= 28 + 14) {
		const unsigned char *a = f + 14;
		unsigned op = (a[6] << 8) | a[7];
		const char *opn = op == 1 ? "req" : op == 2 ? "reply" : "?";
		fprintf(stderr, "  ARP %s  sender %u.%u.%u.%u  target %u.%u.%u.%u",
			opn,
			a[14], a[15], a[16], a[17],
			a[24], a[25], a[26], a[27]);
	}

	fprintf(stderr, "\n");
}
// Slowdown counter which prevents the network read code from running
// on every I/O iteration since it slows down the emulator significantly.
uint32_t bpfscan = 0;

// Polynomial used
uint32_t poly = 0xedb88320;

// This is slow but functional using bit math
unsigned crc32(unsigned char *data, int length) {
	// Init is fixed here
	uint32_t crc = 0xffffffff;

	while(--length >= 0) {
		unsigned char current = *data++;
		int bit;

		for (bit = 8; --bit >= 0; current >>= 1) {
			if ((crc ^ current) & 1) {
				crc >>= 1;
				crc ^= poly;
			} else
				crc >>= 1;
		}
	}
	return ~crc;
}

// Function to enable and disable tracing
void e3c400_enable_trace(int level) {
	trace_3c400=level;
}

// Ack interrupt request
int32_t e3c400_device_ack() {
	if(trace_3c400)
		printf("3C400 IRQ Device ACK:\n");

	// Clear interrupt and return
	int_controller_clear(IRQ_SW_INT3);
	return M68K_INT_ACK_AUTOVECTOR;
}

// Functions to interact with the network
// At the moment only BPF is supported but even if not present, the functions will "work" but obviously no packets can be received or sent
//
// If you want to use BPF, you should put yourself in the group that owns /dev/bpf* and chmod it to 0660 (if it isn't already)
// The alternative is to run the emulator as root, which is A VERY BAD IDEA(TM)
// You should also set the bpf device and network device (near the top of this file)

// Check whether this packet is desirable, depending on the adapter mode, and PA settings.
//
// Receive header bit polarity (matches RetroCore's E3C400Chip and what
// the SunOS if_ec driver expects):
//   broadcast    = 1  -> dst MAC is FF:FF:FF:FF:FF:FF (broadcast)
//   addressmatch = 1  -> dst MAC matches our station MAC
// (The previous "reversed" convention had these inverted, which made
// SunOS log "ec0: garbled packet" on every received frame.)
uint8_t is_mypacket(unsigned char buffer) {
	if(buffer == 'A') {
		if(meabuffer[0] == 0xff && meabuffer[1] == 0xff && meabuffer[2] == 0xff && meabuffer[3] == 0xff && meabuffer[4] == 0xff && meabuffer[5] == 0xff) {
			rxhdr_set_flag(meahdr, RX_BROADCAST, 1);
			if(trace_3c400) printf("buffer A: broadcast packet\n");
		} else {
			rxhdr_set_flag(meahdr, RX_BROADCAST, 0);
			if(trace_3c400) printf("buffer A: regular packet\n");
		}
		if(meabuffer[0] == macaddr[0] && meabuffer[1] == macaddr[1] && meabuffer[2] == macaddr[2] &&
		   meabuffer[3] == macaddr[3] && meabuffer[4] == macaddr[4] && meabuffer[5] == macaddr[5]) {
			rxhdr_set_flag(meahdr, RX_ADDRMATCH, 1);
			if(trace_3c400) printf("buffer A: destined to us\n");
		} else {
			rxhdr_set_flag(meahdr, RX_ADDRMATCH, 0);
			if(trace_3c400) printf("buffer A: not for us\n");
		}
		// PA 0/1/2 = promiscuous; everything is delivered.
		if(mecsr->csr.pa == 0 || mecsr->csr.pa == 1 || mecsr->csr.pa == 2) {
			if(trace_3c400) printf("Promiscuous mode, deliver\n");
			return 1;
		}
		// Otherwise deliver only frames addressed to us or broadcast.
		if(rxhdr_get_flag(meahdr, RX_ADDRMATCH) || rxhdr_get_flag(meahdr, RX_BROADCAST)) {
			if(trace_3c400) printf("Deliver to CPU\n");
			return 1;
		}
	} else if(buffer == 'B') {
		if(mebbuffer[0] == 0xff && mebbuffer[1] == 0xff && mebbuffer[2] == 0xff && mebbuffer[3] == 0xff && mebbuffer[4] == 0xff && mebbuffer[5] == 0xff) {
			rxhdr_set_flag(mebhdr, RX_BROADCAST, 1);
			if(trace_3c400) printf("buffer B: broadcast packet\n");
		} else {
			rxhdr_set_flag(mebhdr, RX_BROADCAST, 0);
			if(trace_3c400) printf("buffer B: regular packet\n");
		}
		if(mebbuffer[0] == macaddr[0] && mebbuffer[1] == macaddr[1] && mebbuffer[2] == macaddr[2] &&
		   mebbuffer[3] == macaddr[3] && mebbuffer[4] == macaddr[4] && mebbuffer[5] == macaddr[5]) {
			rxhdr_set_flag(mebhdr, RX_ADDRMATCH, 1);
			if(trace_3c400) printf("buffer B: destined to us\n");
		} else {
			rxhdr_set_flag(mebhdr, RX_ADDRMATCH, 0);
			if(trace_3c400) printf("buffer B: not for us\n");
		}
		if(mecsr->csr.pa == 0 || mecsr->csr.pa == 1 || mecsr->csr.pa == 2) {
			if(trace_3c400) printf("Promiscuous mode, deliver\n");
			return 1;
		}
		if(rxhdr_get_flag(mebhdr, RX_ADDRMATCH) || rxhdr_get_flag(mebhdr, RX_BROADCAST)) {
			if(trace_3c400) printf("Deliver to CPU\n");
			return 1;
		}
	}
	return 0;
}

// Function to find which buffer is free
// Returns A, B, or Z (if no buffer is available)
unsigned char find_buffer() {
	// Both ABSW and BBSW are set?
	if(mecsr->csr.absw && mecsr->csr.bbsw) {
		// Check value of RBBA
		if(mecsr->csr.rbba) {
			// RBBA is set, B buffer should get packet
			return 'B';
		} else {
			return 'A';
		}
	// ABSW is set
	} else if(mecsr->csr.absw) {
		return 'A';
	// BBSW
	} else if(mecsr->csr.bbsw) {
		return 'B';
	} else {
		return 'Z';
	}
}

// Function to handle packets that are outbound
void handle_outgoing_packets() {
	// Integer to store first byte of packet to transmit
	uint16_t firstbyte;
	uint32_t crc;
	// Loop variable
	int p=0;
	/// Check if we have an outgoing paccket
	if(mecsr->csr.tbsw) {
		// Read packet header first_byte value
		firstbyte = mexhdr->hdr.firstbyte;
		if(trace_3c400)
			printf("3C400 Write packet, offset %u, size %u\n",firstbyte,2048-firstbyte);
		// We calculate the CRC32 in case we need to use it later
		// The CRC includes the entire frame, minus the 4 bytes of CRC 32, and the 12 bytes of spacing set at the end
		crc=crc32(&mexbuffer[firstbyte],2048-16-firstbyte);
		// Print it all out if we're tracing
		for(p=firstbyte;p<2048;p++) {
			if(trace_3c400) 
				printf("%02x ",mexbuffer[p]);
		}
		if(trace_3c400)
			printf("\n");
		// Send packet through whichever backend is active.
		if(netif) {
			int sendlen = 2048 - firstbyte;
			if (g_net_dump)
				dump_frame("[3c400 TX]", &mexbuffer[firstbyte], sendlen);
			int byteswritten = net_send(netif, &mexbuffer[firstbyte], sendlen);
			if(trace_3c400)
				printf("Bytes written to net(%s): %d\n", net_backend_name(), byteswritten);
		}
		// Set tbsw to zero so that other packets can be sent
		mecsr->csr.tbsw = 0;
		// Check whether interrupt on transmit sent is set
		if(mecsr->csr.tinten)
			// interrupt the cpu to tell it we "sent" the packet
			int_controller_set(IRQ_SW_INT3);
	}
}

// Function to handle packets that are inbound
void handle_incoming_packet() {
	// Temp variable
	int p=0;
	// Both ABSW and BBSW are set?
	if(mecsr->csr.absw && mecsr->csr.bbsw) {
		// Check value of RBBA
		if(mecsr->csr.rbba) {
			// Are we interested in this packet?
			if(is_mypacket('B')) {
				if(trace_3c400) {
					printf("Packet delivered to buffer B, trace follows\n");
					// Print it all out if we're tracing
					for(p=0;p<rxhdr_doff(mebhdr);p++) {
						if(trace_3c400) 
							printf("%02x ",mebbuffer[p]);
					}
				}
				// Increase firstfree by 6 since the OS skips the last 6 bytes of the packet in the buffer
				rxhdr_add_doff(mebhdr, 6);
				// Set BBSW to zero to indicate we have a packet
				mecsr->csr.bbsw = 0;
				// Clear RBBA bit to indicate the most recent packet is in B
				mecsr->csr.rbba = 0;
				// We have a packet, check if we should interrupt
				if(mecsr->csr.binten) 
					// Interrupt CPU to tell it about the packet
					int_controller_set(IRQ_SW_INT3);
			} 
		} else {
			// Are we interested in this packet?
			if(is_mypacket('A')) {
				if(trace_3c400) {
					printf("Packet delivered to buffer A, trace follows\n");
					// Print it all out if we're tracing
					for(p=0;p<rxhdr_doff(meahdr);p++) {
						if(trace_3c400) 
							printf("%02x ",meabuffer[p]);
					}
					printf("\nMeaHDR firstfree: %u\n",rxhdr_doff(meahdr));
				}
				// Increase firstfree by 6 since the OS skips the last 6 bytes of the packet in the buffer
				rxhdr_add_doff(meahdr, 6);
				// Set ABSW to zero to indicate we have a packet
				mecsr->csr.absw = 0;
				// Set RBBA bit to indicate that most recent packet is in A
				mecsr->csr.rbba = 1;
				// We have a packet, check if we should interrupt
				if(mecsr->csr.ainten) 
					// Interrupt CPU to tell it about the packet
					int_controller_set(IRQ_SW_INT3);
			}
		}
	// Is ABSW set?
	} else if(mecsr->csr.absw) {
		// Are we interested in this packet?
		if(is_mypacket('A')) {
			if(trace_3c400) {
				printf("Packet delivered to buffer A, trace follows\n");
				// Print it all out if we're tracing
				for(p=0;p<rxhdr_doff(meahdr);p++) {
					if(trace_3c400) 
						printf("%02x ",meabuffer[p]);
				}
			}
			// Increase firstfree by 6 since the OS skips the last 6 bytes of the packet in the buffer
			rxhdr_add_doff(meahdr, 6);
			// Set ABSW to zero to indicate we have a packet
			mecsr->csr.absw = 0;
			// Set RBBA to 1 to indicate most recent packet is in A
			mecsr->csr.rbba = 1;
			// Check if we should interrupt the CPU
			if(mecsr->csr.ainten) 
				// Interrupt CPU to tell it about the packet
				int_controller_set(IRQ_SW_INT3);
		}
	// Is BBSW set?
	} else if(mecsr->csr.bbsw) {
		// Are we interested in this packet?
		if(is_mypacket('B')) {
			if(trace_3c400) {
				printf("Packet delivered to buffer A, trace follows\n");
				// Print it all out if we're tracing
				for(p=0;p<rxhdr_doff(meahdr);p++) {
					if(trace_3c400) 
						printf("%02x ",meabuffer[p]);
				}
			}
			// Increase firstfree by 6 since the OS skips the last 6 bytes of the packet in the buffer
			rxhdr_add_doff(mebhdr, 6);
			// Set BBSW to zero to indicate we have a packet
			mecsr->csr.bbsw = 0;
			// Clear RBBA bit to indicate the most recent packet is in B
			mecsr->csr.rbba = 0;
			// We have a packet, check if we should interrupt
			if(mecsr->csr.binten)
				// Interrupt CPU to tell it about the packet
				int_controller_set(IRQ_SW_INT3);
		}
	}
}

// Init the controller, malloc, set reasonable defaults for status registers, this is run by the emulator on startup
void e3c400_init(void) {
	// Local loop variables
	uint32_t p=0; uint32_t j=0;

	// Retransmit counter set to 0
	meback = 0;

	// Set mac-addr
	macaddr[0] = 0; macaddr[1] = 0; macaddr[2] = 0; macaddr[3] = 0; macaddr[4] = 0; macaddr[5] = 0; macaddr[6] = '\0'; 

	// Reset loop variables
	p=0; j=0;

	// Enable trace if you want to debug something
	e3c400_enable_trace(0);
		
	// Allocate memory 
	// Variable for the status register
	mecsr = malloc(sizeof(union csr));

	// This is the start of the 3c400 rom, @MEBASE+400
	romaddr = malloc(sizeof(union macaddr));

	// This is the station address ram, @MEBASE+600, it holds the station's mac address, used by the adapter to determine which packets are "ours"
	ramaddr = malloc(sizeof(union macaddr));

	// Transmit header
	mexhdr = malloc(sizeof(union txhdr));
	// Transmit buffer
	mexbuffer = malloc(2048*sizeof(char *));

	// Receive headers A and B
	meahdr = malloc(sizeof(rxhdr_t));
	mebhdr = malloc(sizeof(rxhdr_t));

	// Receive buffers
	meabuffer = malloc(2048*sizeof(char *));
	mebbuffer = malloc(2048*sizeof(char *));

	// Set reasonable defaults, receive all our packets - error, interrupt on jam, transmit, and receive buffers a&b
	// Set rest to 0
	mecsr->csr.pa = 7;
	mecsr->csr.jinten = 0;
	mecsr->csr.tinten = 0;
	mecsr->csr.ainten = 1;
	mecsr->csr.binten = 1;
	mecsr->csr.reset = 0;
	mecsr->csr.unused = 0;
	mecsr->csr.amsw = 0;
	mecsr->csr.jam = 0;
	mecsr->csr.tbsw = 0;
	mecsr->csr.absw = 0;
	mecsr->csr.tbsw = 0;

	//  Rom address set to MAC addresses previously defined in this file
	romaddr->addr.o1 = MAC1; romaddr->addr.o2 = MAC2; romaddr->addr.o3 = MAC3; romaddr->addr.o4 = MAC4;
	romaddr->addr.o5 = MAC5; romaddr->addr.o6 = MAC6; romaddr->addr.o7 = 0x00; romaddr->addr.o8 = 0x00;

	// Ram address set to all-zeroes, software will overwrite it
	ramaddr->addr.o1 = 0x00; ramaddr->addr.o2 = 0x00; ramaddr->addr.o3 = 0x00; ramaddr->addr.o4 = 0x00; 
	ramaddr->addr.o5 = 0x00; ramaddr->addr.o6 = 0x00; ramaddr->addr.o7 = 0x00; ramaddr->addr.o8 = 0x00;

	// Transmit buffer first byte set to zero
	mexhdr->hdr.firstbyte = 0;
	mexhdr->hdr.unused = 0;
	// Fill in the transmit buffer itself with zeros
	for(p=0;p<2046;p++)	
		mexbuffer[p] = 0;

	// Receive buffer A hdr — initial values (overwritten when a real
	// packet lands, so semantically just "no packet here yet").
	// Bit polarity matches RetroCore E3C400Chip and SunOS if_ec:
	//   broadcast / addressmatch = 1 means "yes, this packet is".
	rxhdr_set_doff(meahdr, 16);
	rxhdr_set_flag(meahdr, RX_FRAMINGERR, 0);
	rxhdr_set_flag(meahdr, RX_ADDRMATCH, 0);
	rxhdr_set_flag(meahdr, RX_RANGEERR, 0);
	rxhdr_set_flag(meahdr, RX_BROADCAST, 0);
	rxhdr_set_flag(meahdr, RX_FCSERR, 0);
	//  Fill in the receive buffer itself with zeros
	for(p=0;p<2046;p++)	
		meabuffer[p] = 0;

	
	// Receive buffer B hdr — initial values (see receive buffer A above).
	rxhdr_set_doff(mebhdr, 16);
	rxhdr_set_flag(mebhdr, RX_FRAMINGERR, 0);
	rxhdr_set_flag(mebhdr, RX_ADDRMATCH, 0);
	rxhdr_set_flag(mebhdr, RX_RANGEERR, 0);
	rxhdr_set_flag(mebhdr, RX_BROADCAST, 0);
	rxhdr_set_flag(mebhdr, RX_FCSERR, 0);
	//  Fill in the receive buffer itself with zeros
	for(p=0;p<2046;p++)	
		mebbuffer[p] = 0;

	// Open the host network interface through whichever backend was
	// linked in (BPF on macOS/BSD, libpcap on Linux, Npcap on Windows,
	// or stub when networking is disabled at build time).
	//
	// Interface selection priority (highest first):
	//   1. --net-iface=NAME on the command line  (sets g_net_iface)
	//   2. SUN2_NET_IFACE env var                (also lands in g_net_iface)
	//   3. NULL → backend picks a default
	//      pcap: first non-loopback adapter
	//      bpf:  "en0"
	//
	// The legacy "vmnet8" hardcode is gone — it was a sigurbjornl
	// convention from a specific macOS+VMWare setup, not a portable
	// default.  Use --net-list to see what's available on this host.
	uint8_t mac[6] = { romaddr->addr.o1, romaddr->addr.o2, romaddr->addr.o3,
	                   romaddr->addr.o4, romaddr->addr.o5, romaddr->addr.o6 };
	const char *iface = g_net_iface;  /* may be NULL → backend default */
	netif = net_open(iface, mac, /*promiscuous=*/1);
	if (!netif)
		printf("3C400: networking disabled (net(%s) open failed for '%s')\n",
		       net_backend_name(), iface ? iface : "<auto>");
	else
		printf("3C400: net(%s) bound to %s\n",
		       net_backend_name(), iface ? iface : "<auto>");
}


// Perform our daily tasks
void e3c400_update(void) {
	// Temporary loop variable
	int p=0;
	// Receive scratch buffer (one ethernet frame).
	unsigned char framebuf[2048];
	int framelen = 0;
	// Which receive buffer (A or B) we should drop the next packet into.
	unsigned char whichbuffer;
	// Check the reset bit
	if(mecsr->csr.reset) {
		// Reset the Transmit and Receive enable bits
		mecsr->csr.tbsw=0;
		mecsr->csr.absw=0;
		mecsr->csr.bbsw=0;
		if(trace_3c400)
			printf("3C400 Performing board reset\n");
		// Disable reset bit
		mecsr->csr.reset=0;
		// Set PA to 7, my packets, and broadcast packets, but no errors
		mecsr->csr.pa = 7;
	}
	// Check JAM bit and interrupt CPU if needed
	if(mecsr->csr.jam) {
		printf("3C400: JAM bit was set\n");
		// Is JINTEN set?  Interrupt CPU
		if(mecsr->csr.jinten) {
			if(trace_3c400)
				printf("3C400: JAM interrupt sent to CPU\n");
			// Interrupt the CPU to tell it about it
			int_controller_set(IRQ_SW_INT3);
		}
	}
	// Check the AMSW switch
	if(mecsr->csr.amsw) {
		// RAM address of the controller should now be in the ram addr, let's read it into the mac-addr array
		memcpy(&macaddr[0],&ramaddr->addr,6);
		if(trace_3c400) {
			printf("3C400: Mac-address set to");
			for(p=0;p<6;p++)
				printf(":%02x",macaddr[p]);
			printf("\n");
		}
		// Set the amsw back down
		mecsr->csr.amsw=0;
			
	}
	//  Check for outgoing packets and send them
	handle_outgoing_packets();
	// Increment the scan counter by 1 for each update loop
	bpfscan++;
	// Only poll the host network every Nth update — reading packets from
	// libpcap / BPF is the dominant cost in this loop on most hosts.  On
	// modern processors a stride of 5000 gives roughly 10 Mbit/s sustained
	// throughput; lower it if you need lower latency at the cost of CPU.
	if (netif && (mecsr->csr.absw || mecsr->csr.bbsw) && (bpfscan % 5000 == 0)) {
		// Drain whatever the backend has buffered.
		while (mecsr->csr.absw || mecsr->csr.bbsw) {
			framelen = net_recv(netif, framebuf, sizeof(framebuf));
			if (framelen <= 0) break;       /* 0 = no packet, <0 = error */

			if (trace_3c400)
				printf("3C400: %d bytes received from net(%s)\n",
				       framelen, net_backend_name());

			/* Match RetroCore E3C400Chip.ReceivePacket():
			   - clamp the copy to 2046 bytes (the physical buffer
			     size minus the 2-byte status word the real card
			     prepends)
			   - pad short frames up to the 60-byte Ethernet runt
			     minimum; the kernel driver expects to see at least
			     a full minimum-size frame */
			int copy_len = (framelen > 2046) ? 2046 : framelen;
			int reported_len = (copy_len < 60) ? 60 : copy_len;

			/* Snapshot ABSW/BBSW so we can tell after handle_incoming_packet
			   whether SunOS actually got this frame (=card released the
			   buffer to host) or is_mypacket dropped it (=card kept it). */
			unsigned char absw_before = mecsr->csr.absw;
			unsigned char bbsw_before = mecsr->csr.bbsw;
			unsigned char pa_at_rx    = mecsr->csr.pa;

			whichbuffer = find_buffer();
			if (whichbuffer == 'A' || whichbuffer == 'B') {
				unsigned char *rxbuf = (whichbuffer == 'A') ? meabuffer : mebbuffer;
				rxhdr_t       *rxhdr = (whichbuffer == 'A') ? meahdr    : mebhdr;

				/* Copy frame bytes; zero-pad short frames to the 60-byte
				   Ethernet runt minimum so length math downstream matches
				   what a real NIC would have padded. */
				memcpy(rxbuf, framebuf, copy_len);
				if (reported_len > copy_len)
					memset(rxbuf + copy_len, 0, reported_len - copy_len);

				/* pcap strips the 4-byte Ethernet FCS before delivering
				   the frame (per tcpdump.org / Wireshark wiki: "Most
				   Ethernet interfaces don't supply the FCS").  The 3C400
				   delivers the FCS in its rx ring and SunOS's if_ec
				   driver validates it — that's what the original "+6"
				   comment "OS skips last 6 bytes" meant: 2-byte rxhdr +
				   4-byte FCS.  Without a valid FCS appended, SunOS sees
				   zeros at that offset, validation fails, and it logs
				   "ec0: garbled packet" for every frame.

				   Recompute the CRC32 over the frame data we just copied
				   (post-padding so the FCS covers the whole 60-byte
				   minimum) and append it in little-endian wire order. */
				uint32_t fcs = crc32(rxbuf, reported_len);
				rxbuf[reported_len + 0] = (uint8_t)(fcs       & 0xff);
				rxbuf[reported_len + 1] = (uint8_t)(fcs >> 8  & 0xff);
				rxbuf[reported_len + 2] = (uint8_t)(fcs >> 16 & 0xff);
				rxbuf[reported_len + 3] = (uint8_t)(fcs >> 24 & 0xff);

				rxhdr_set_doff(rxhdr, reported_len);
				handle_incoming_packet();
			} else {
				/* Both rx slots full — kernel hasn't drained yet.
				   This is normal under load; the real card just
				   loses the frame. */
				if (trace_3c400)
					printf("3C400: no rx buffer free, frame dropped\n");
				if (g_net_dump) {
					char tag[64];
					snprintf(tag, sizeof(tag), "[3c400 RX-NOBUF pa=%d]", pa_at_rx);
					dump_frame(tag, framebuf, framelen);
				}
				return;
			}

			/* Decide whether handle_incoming_packet actually released
			   the buffer to SunOS (=accepted) or kept it (=is_mypacket
			   said not for us). */
			int delivered = 0;
			if (whichbuffer == 'A' && absw_before && !mecsr->csr.absw) delivered = 1;
			if (whichbuffer == 'B' && bbsw_before && !mecsr->csr.bbsw) delivered = 1;

			if (g_net_dump) {
				/* For DELIVER lines, also show the 16-bit rxhdr value
				   SunOS will read: bit 14=broadcast, bit 12=addrmatch,
				   bit 15=fcserr, bits 0-10=firstfree.  Mismatch with
				   what the driver expects = "ec0: garbled". */
				unsigned hdr_val = (whichbuffer == 'A')
				                 ? (*meahdr)
				                 : (whichbuffer == 'B' ? (*mebhdr) : 0);
				char tag[96];
				snprintf(tag, sizeof(tag),
				         "[3c400 %s pa=%d buf=%c hdr=%04x ff=%u]",
				         delivered ? "DELIVER" : "DROP   ",
				         pa_at_rx, whichbuffer,
				         hdr_val, hdr_val & 0x07FF);
				dump_frame(tag, framebuf, framelen);
			}
		}
	}
}
	
// Handle reading bytes from the emulated e3c400
uint32_t e3c400_read(uint32_t addr, int32_t size) {
	if(trace_3c400)
		printf("3C400 Read addr: %x, size: %d\n",addr,size);

	// Temporary variables
	int p=0; int offset=0; uint32_t retvalue; uint32_t retarray[8];

	// Reading and writing from the status register, the status word, and the MEBACK retransmission counter are repeated throughout that space
	if(addr >= 0xe0000 && addr <0xe0400 && size==2) {
		if(trace_3c400)
			printf("3C400 Status Register read, value: %u\n",mecsr->csrword);
		return mecsr->csrword;
	// Receive buffer A read
	} else if(addr >= 0xe1002 & addr <0xe1800 && size <=4) {
		// Initial offset calculated
		offset= addr - 0xe1002;
		switch(size) {
			case 4:
				retvalue =  meabuffer[offset] <<24;
				retvalue += meabuffer[offset+1] <<16;
				retvalue += meabuffer[offset+2] <<8;
				retvalue += meabuffer[offset+3];
				break;
			 case 2:
				retvalue = meabuffer[offset] << 8;
				retvalue += meabuffer[offset+1];
				break;
			 case 1:
				retvalue = meabuffer[offset];
				break;
			default:
				printf("3C400: rx buffer A, unmatched size %d at addr: %x \n",size,addr);
				retvalue=0;
				break;
		}
		if(trace_3c400) {
			if(size == 4)
				printf("Inside receive buffer A total length: %u, read addr: %x offset: %u value: %08x\n",rxhdr_doff(meahdr),addr,offset,retvalue);
			if(size == 2)
				printf("Inside receive buffer A total length: %u, read addr: %x offset: %u value: %04x\n",rxhdr_doff(meahdr),addr,offset,retvalue);
			if(size == 1)
				printf("Inside receive buffer A total length: %u, read addr: %x offset: %u value: %02x\n",rxhdr_doff(meahdr),addr,offset,retvalue);
		}
		// Return the value
		return retvalue;
	// Receive buffer B read
	} else if(addr >= 0xe1802 & addr <0xe2000 && size <2046) {
		// Initial offset calculated
		offset= addr - 0xe1802;

		switch(size) {
			case 4:
				retvalue = mebbuffer[offset] << 24;
				retvalue += mebbuffer[offset+1] << 16;
				retvalue += mebbuffer[offset+2] << 8;
				retvalue += mebbuffer[offset+3];
				break;
			 case 2:
				retvalue = mebbuffer[offset] << 8;
				retvalue += mebbuffer[offset+1];
				break;
			 case 1:
				retvalue = mebbuffer[offset];
				break;
			default:
				printf("3C400: rx buffer B, unmatched size %d at addr: %x \n",size,addr);
				retvalue=0;
				break;
		}
		if(trace_3c400) {
			if(size == 4)
				printf("Inside receive buffer B read, addr: %x offset: %u value: %08x\n",addr,offset,retvalue);
			if(size == 2)
				printf("Inside receive buffer B read, addr: %x offset: %u value: %04x\n",addr,offset,retvalue);
			if(size == 1)
				printf("Inside receive buffer B read, addr: %x offset: %u value: %02x\n",addr,offset,retvalue);
		}
		// Return the value
		return retvalue;
 	// Reading the rom address, 8 bytes, 6 of which represent the mac-address, the final two 6adding
	// This then repeats through the rom-address space from 0xe0400 and until 0xe0600
	} else if(addr >= 0xe0400 && addr < 0xe0600) {
		// Reading two bytes?
		if(size==2) {
			// Start
			if(addr % 8 == 0) {
				retarray[0] = romaddr->addr.o1;
				retarray[1] = romaddr->addr.o2;
			// Second
			} else if(addr % 2 == 0) {
				retarray[0] = romaddr->addr.o3;
				retarray[1] = romaddr->addr.o4;
			// Third
			} else if(addr % 4 == 0) {
				retarray[0] = romaddr->addr.o5;
				retarray[1] = romaddr->addr.o6;
			// These are present but always zero...
			} else if(addr % 6 == 0) {
				retarray[0] = romaddr->addr.o7;
				retarray[1] = romaddr->addr.o8;
			}
		// Reading four bytes?
		} else if(size==4) {
			// Start-3
			if(addr % 8 == 0) {
				retarray[0] = romaddr->addr.o1;
				retarray[1] = romaddr->addr.o2;
				retarray[2] = romaddr->addr.o3;
				retarray[3] = romaddr->addr.o4;
			// 2-6
			} else if(addr % 2 == 0) {
				retarray[0] = romaddr->addr.o3;
				retarray[1] = romaddr->addr.o4;
				retarray[2] = romaddr->addr.o5;
				retarray[3] = romaddr->addr.o6;
			// 4-8
			} else if(addr % 4 == 0) {
				retarray[0] = romaddr->addr.o5;
				retarray[1] = romaddr->addr.o6;
				retarray[2] = romaddr->addr.o7;
				retarray[3] = romaddr->addr.o8;
			}
		} else {
			printf("3C400: Uncaught Fetching of %u bytes from rom ethernet address\n",size);
			return 0;
		}
 	// Reading the ram address, 8 bytes, 6 of which represent the mac-address, the final two padding
	// This then repeats through the rom-address space from 0xe0600 and untili 0xe0800
	} else if(addr >= 0xe0600 && addr < 0xe0800) {
		// Reading two bytes?
		if(size==2) {
			// Start
			if(addr % 8 == 0) {
				retarray[0] = ramaddr->addr.o1;
				retarray[1] = ramaddr->addr.o2;
			// Second
			} else if(addr % 2 == 0) {
				retarray[0] = ramaddr->addr.o3;
				retarray[1] = ramaddr->addr.o4;
			// Third
			} else if(addr % 4 == 0) {
				retarray[0] = ramaddr->addr.o5;
				retarray[1] = ramaddr->addr.o6;
			// These are present but always zero...
			} else if(addr % 6 == 0) {
				retarray[0] = ramaddr->addr.o7;
				retarray[1] = ramaddr->addr.o8;
			}
		// Reading four bytes?
		} else if(size==4) {
			// Start-3
			if(addr % 8 == 0) {
				retarray[0] = ramaddr->addr.o1;
				retarray[1] = ramaddr->addr.o2;
				retarray[2] = ramaddr->addr.o3;
				retarray[3] = ramaddr->addr.o4;
			// 2-6
			} else if(addr % 2 == 0) {
				retarray[0] = ramaddr->addr.o3;
				retarray[1] = ramaddr->addr.o4;
				retarray[2] = ramaddr->addr.o5;
				retarray[3] = ramaddr->addr.o6;
			// 4-8
			} else if(addr % 4 == 0) {
				retarray[0] = ramaddr->addr.o5;
				retarray[1] = ramaddr->addr.o6;
				retarray[2] = ramaddr->addr.o7;
				retarray[3] = ramaddr->addr.o8;
			}
		} else {
			printf("3C400: Uncaught Fetching of %u bytes from ram ethernet address\n",size);
			return 0;
		}
	} else {
		// Transmit and receive buffer headers
		switch(addr) {
			// Transmit buffer header
			case 0xe0800:
				// Reading the entire header
				if(size==2)
					return mexhdr->header;
				else
					printf("3C400: Uncaught Fetching of %u bytes from transmit header\n",size);
				return -1;
				break;
			// Receive buffer header A
			case 0xe1000:
				// Reading the entire header
				if(size==2)
					return (*meahdr);
				else
					printf("3C400: Uncaught Fetching of %u bytes from receive header A\n",size);
				return -1;
				break;
			// Receive buffer header B
			case 0xe1800:
				// Reading the entire header
				if(size==2)
					return (*mebhdr);
				else
					printf("3C400: Uncaught Fetching of %u bytes from receive header B\n",size);
				return -1;
				break;
			default:
				printf("3C400: Reaching Uncaught read instruction for address %x, size: %d\n",addr,size);
				return -1;
				break;
		}
	} // End address range if
  // This is unreachable
  return -1;
}

// Handle writing bytes to the emulated 3c400
void e3c400_write(uint32_t addr, uint32_t size, uint32_t value) {
	// Offset is here
	int offset=0; 
  	// Write values to the e3c400, this is limited to the control and status register as well as the transport buffeer
	if(trace_3c400)
		printf("3C400 Write addr: %x, value: %u, size: %d\n",addr,value,size);

	if(addr == 0xe0000 && size==2) {
		// PA is a 4-bit field at bits 0-3 of the CSR.  Previously this
		// did `(value & 0xf) << 4` which shifted the bits into 4-7;
		// after bitfield truncation `pa` was always 0 → permanent
		// promiscuous mode → SunOS saw every packet on the host LAN
		// and logged "ec0: garbled" for each one not addressed to it.
		// Matches RetroCore E3C400Chip CSR_PA = 0x000F.
		mecsr->csr.pa = value & 0xf;
		if(trace_3c400)
			printf("3C400 Setting PA to %u\n", value & 0xf);
		// Interrupt settings for jam, a/b receive buffers, transmit bufffer
		mecsr->csr.jinten = (value & (1 << 4))>>4;
		if(trace_3c400)
			printf("3C400 Setting JINTEN to %u\n",(value & (1 << 4))>>4);
		mecsr->csr.tinten = (value & (1 << 5))>>5;
		if(trace_3c400)
			printf("3C400 Setting TINTEN to %u\n",(value & (1 << 5))>>5);
		mecsr->csr.ainten = (value & (1 << 6))>>6;
		if(trace_3c400)
			printf("3C400 Setting AINTEN to %u\n",(value & (1 << 6))>>6);
		mecsr->csr.binten = (value & (1 << 7))>>7;
		if(trace_3c400)
			printf("3C400 Setting BINTEN to %u\n",(value & (1 << 7))>>7);
		// Reset flag, this resets the controller
		mecsr->csr.reset = (value & (1 << 8))>>8;
		if(trace_3c400)
			printf("3C400 Setting RESET to %u\n",(value & (1 << 8))>>8);
		// AMSW, this is set to 1 when our mac-address has been written into the 3c400 RAM
		mecsr->csr.amsw = (value & (1 << 11))>>11;
		if(trace_3c400)
			printf("3C400 Setting AMSW to %u\n",(value & (1 << 11))>>11);
		// JAM bit
		mecsr->csr.jam = (value & (1 << 12))>>12;
		if(trace_3c400)
			printf("3C400 Setting JAM to %u\n",(value & (1 << 12))>>12);
		// Transmit buffer, set to 1 when packet is in tx buffer and ready to be transmitted
		// Not possible to set to 0 after it's set to one, it's set to 0 when the packet has been transmitted
		if(mecsr->csr.tbsw == 0) {
			mecsr->csr.tbsw = (value & (1 << 13))>>13;
			if(trace_3c400)
				printf("3C400 Setting TBSW to %u\n",(value & (1 << 13))>>13);
		} else {
			printf("3C400: Setting transmit buffer bit with active transfer in progress\n");
		}
		// Receive buffer, set to 1 when you want packet to be delivered to this buffer
		// It's not possible to set absw when it's 1, it will be cleared if a packet arrives in the A buffer
		if(mecsr->csr.absw == 0) {
			mecsr->csr.absw = (value & (1 << 14))>>14;
			if(trace_3c400)
				printf("3C400 Setting ABSW to %u\n",(value & (1 << 14)>>14));
		} 
		// It's not possible to set bbsw when it's 1, it will be cleared if a packet arrives in the B buffer
		if(mecsr->csr.bbsw == 0) {
			mecsr->csr.bbsw = (value & (1 << 15))>>15;
			if(trace_3c400)
				printf("3C400 Setting BBSW to %u\n",(value & (1 << 15)>>15));
		} 
	} else if(addr == 0xe0600 && size ==4) {
		// Setting the RAM address,first two bytes
		ramaddr->addr.o1 = (value & 0xFFFFFFFF)>>24;;
		ramaddr->addr.o2 = (value & 0xFFFFFF)>>16;
		ramaddr->addr.o3 = (value & 0xFFFF)>>8;
		ramaddr->addr.o4 = value & 0xFF;
		if(trace_3c400)
			printf("3C400 setting RAM mac-address to %02x:%02x:%02x:%02x:%02x:%02x\n",ramaddr->addr.o1,ramaddr->addr.o2,ramaddr->addr.o3,ramaddr->addr.o4,ramaddr->addr.o5,ramaddr->addr.o6);
	} else if(addr == 0xe0604 && size ==2) {
		// Setting the RAM address,last two bytes
		ramaddr->addr.o5 = (value & 0xFFFFFFFF)>>8;
		ramaddr->addr.o6 = value & 0xFf;
	} else if(addr == 0xe0800 && size ==2) {
		// Mex HDR write
		mexhdr->hdr.firstbyte = value;
	} else if(addr >= 0xe0802 & addr <0xe1000 && size <=4) {
		// Initial offset calculated
		offset= addr - 0xe0800;

		switch(size) {
			case 4:
				mexbuffer[offset]   = (value & 0xFFFFFFFF)>>24;;
				mexbuffer[offset+1] = (value & 0xFFFFFF)>>16;
				mexbuffer[offset+2] = (value & 0xFFFF)>>8;
				mexbuffer[offset+3] = value & 0xFF;
				break;
			 case 2:
				mexbuffer[offset] = (value & 0xFFFF)>>8;
				mexbuffer[offset+1] = value & 0xFF;
				break;
			 case 1:
				mexbuffer[offset] = value & 0xFF;
				break;
			default:
				printf("3C400: tx buffer, unmatched size %d at addr: %x \n",size,addr);
				break;
		}
	// Receive buffer A header (mirror of the read at 0xe1000)
	} else if(addr == 0xe1000 && size == 2) {
		(*meahdr) = value;
	// Receive buffer A data (mirror of the read at 0xe1002..0xe17ff).
	// SunOS's ec0 driver clears this region on init and after consuming
	// each frame; without these write handlers, the buffer retained
	// stack-uninit data and a later read confused the driver into
	// kernel panic.
	} else if(addr >= 0xe1002 & addr <0xe1800 && size <=4) {
		offset = addr - 0xe1002;
		switch(size) {
			case 4:
				meabuffer[offset]   = (value >> 24) & 0xff;
				meabuffer[offset+1] = (value >> 16) & 0xff;
				meabuffer[offset+2] = (value >>  8) & 0xff;
				meabuffer[offset+3] =  value        & 0xff;
				break;
			case 2:
				meabuffer[offset]   = (value >> 8) & 0xff;
				meabuffer[offset+1] =  value       & 0xff;
				break;
			case 1:
				meabuffer[offset]   =  value       & 0xff;
				break;
			default:
				printf("3C400: rx buffer A, unmatched size %d at addr: %x\n",size,addr);
				break;
		}
	// Receive buffer B header (mirror of the read at 0xe1800)
	} else if(addr == 0xe1800 && size == 2) {
		(*mebhdr) = value;
	// Receive buffer B data (mirror of the read at 0xe1802..0xe1fff)
	} else if(addr >= 0xe1802 & addr <0xe2000 && size <=4) {
		offset = addr - 0xe1802;
		switch(size) {
			case 4:
				mebbuffer[offset]   = (value >> 24) & 0xff;
				mebbuffer[offset+1] = (value >> 16) & 0xff;
				mebbuffer[offset+2] = (value >>  8) & 0xff;
				mebbuffer[offset+3] =  value        & 0xff;
				break;
			case 2:
				mebbuffer[offset]   = (value >> 8) & 0xff;
				mebbuffer[offset+1] =  value       & 0xff;
				break;
			case 1:
				mebbuffer[offset]   =  value       & 0xff;
				break;
			default:
				printf("3C400: rx buffer B, unmatched size %d at addr: %x\n",size,addr);
				break;
		}
	}  else if(!quiet) {
		printf("3C400: Uncaught write to addr %x, size %u\n",addr,size);
	}
}
