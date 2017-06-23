/*
 * jduck's fake AP for WiFi hax.
 *
 * by Joshua J. Drake (@jduck) on 2017-06-13
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* hi-res time */
#include <time.h>

/* internet networking / packet sending */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/ether.h>
#include <linux/if.h>
#include <linux/if_packet.h>

/* packet capturing */
#include <pcap/pcap.h>


/* global hardcoded parameters */
#define SNAPLEN 4096
#define BEACON_INTERVAL 500
#define DEFAULT_CHANNEL 1


/* some bits borrowed from tcpdump! thanks guys! */
#define T_MGMT 0x0  /* management */
#define T_CTRL 0x1  /* control */
#define T_DATA 0x2  /* data */
#define T_RESV 0x3  /* reserved */

#define ST_ASSOC_REQ 0
#define ST_ASSOC_RESP 1
#define ST_PROBE_REQ 4
#define ST_PROBE_RESP 5
#define ST_BEACON 8
#define ST_AUTH 11

#define CF_RETRY 8

#define IEID_SSID 0
#define IEID_RATES 1
#define IEID_DSPARAMS 3

#define IEEE80211_RADIOTAP_RATE 2

#define IEEE80211_BROADCAST_ADDR ((u_int8_t *)"\xff\xff\xff\xff\xff\xff")


const char *dot11_types[4] = { "mgmt", "ctrl", "data", "resv" };
const char *dot11_subtypes[4][16] = {
	/* mgmt */
	{ "assoc-req", "assoc-resp",
		"re-assoc-req", "re-assoc-resp",
		"probe-req", "probe-resp",
		"6?", "7?",
		"beacon", "atim",
		"dis-assoc", "auth",
		"de-auth", "action",
		"14?", "15?" },
	/* ctrl */
	{ "0?", "1?", "2?", "3?", "4?", "5?", "6?", "7?",
		"8?", "block-ack", "ps-poll", "rts", "cts", "ack", "cf-end", "cf-end-ack" },
	/* data */
	{ "0?", "1?", "2?", "3?", "4?", "5?", "6?", "7?", "8?", "9?", "10?", "11?", "12?", "13?", "14?", "15?" },
	/* reserved */
	{ "0?", "1?", "2?", "3?", "4?", "5?", "6?", "7?", "8?", "9?", "10?", "11?", "12?", "13?", "14?", "15?" }
};

int g_sock;
char g_iface[64];

u_int8_t g_bssid[ETH_ALEN];
u_int8_t g_ssid[32];
u_int8_t g_ssid_len;
u_int8_t g_channel = DEFAULT_CHANNEL;

u_int8_t g_pkt[4096];
size_t g_pkt_len = 0;
struct timespec last_retransmit;

/* global options */
int g_send_beacons = 0;
struct timespec last_beacon;

typedef enum {
	S_AWAITING_PROBE_REQ = 0,
	S_SENT_PROBE_RESP = 1,
	S_SENT_AUTH = 2,
	S_SENT_ASSOC_RESP = 3,
	S_ESTABLISHED = 4
} state_t;

state_t g_state = S_AWAITING_PROBE_REQ;


struct ieee80211_radiotap_header {
	u_int8_t it_version;      /* set to 0 */
	u_int8_t it_pad;
	u_int16_t it_len;         /* entire length */
	u_int32_t it_present;     /* fields present */
} __attribute__((__packed__));
typedef struct ieee80211_radiotap_header radiotap_t;

struct ieee80211_frame_header {
	u_int version:2;
	u_int type:2;
	u_int subtype:4;
	u_int8_t ctrlflags;
	u_int16_t duration;
	u_int8_t dst_mac[ETH_ALEN];
	u_int8_t src_mac[ETH_ALEN];
	u_int8_t bssid[ETH_ALEN];
	u_int frag:4;
	u_int seq:12;
} __attribute__((__packed__));
typedef struct ieee80211_frame_header dot11_frame_t;

struct ieee80211_beacon {
	u_int64_t timestamp;
	u_int16_t interval;
	u_int16_t caps;
} __attribute__((__packed__));
typedef struct ieee80211_beacon beacon_t;

struct ieee80211_information_element {
	u_int8_t id;
	u_int8_t len;
	u_int8_t data[0];
} __attribute__((__packed__));
typedef struct ieee80211_information_element ie_t;

struct ieee80211_authentication {
	u_int16_t algorithm;
	u_int16_t seq;
	u_int16_t status;
} __attribute__((__packed__));
typedef struct ieee80211_authentication auth_t;

struct ieee80211_assoc_request {
	u_int16_t caps;
	u_int16_t interval;
} __attribute__((__packed__));
typedef struct ieee80211_assoc_request assoc_req_t;

struct ieee80211_assoc_response {
	u_int16_t caps;
	u_int16_t status;
	u_int16_t id;
} __attribute__((__packed__));
typedef struct ieee80211_assoc_response assoc_resp_t;


void timespec_diff(struct timespec *newer, struct timespec *older, struct timespec *diff);

char *mac_string(u_int8_t *mac);
void hexdump(const u_char *ptr, u_int len);
char *ssid_string(ie_t *ie);

ie_t *get_ssid_ie(const u_int8_t *data, u_int32_t left);
u_int16_t get_sequence(void);

int start_pcap(pcap_t **pcap);
int open_raw_socket(void);
int set_channel(void);

int handle_packet(const u_char *data, u_int32_t left);
int process_periodic_tasks(void);

int process_radiotap(const u_char **ppkt, u_int32_t *pleft);
dot11_frame_t *get_dot11_frame(const u_char **ppkt, u_int32_t *pleft);
int process_probe_request(dot11_frame_t *d11, const u_char *data, u_int32_t left);
int process_auth_request(dot11_frame_t *d11, const u_char *data, u_int32_t left);
int process_assoc_request(dot11_frame_t *d11, const u_char *data, u_int32_t left);

int send_beacon();
int send_probe_response(u_int8_t *dst_mac);
int send_auth_response(u_int8_t *dst_mac);
int send_assoc_response(u_int8_t *dst_mac);


void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [options] <ssid>\n", argv0);
	fprintf(stderr, "\nsupported options:\n\n"
			"-b             send beacons regularly (default: off)\n"
			"-c <channel>   use the specified channel (default: %d)\n"
			"-i <interface> interface to use for monitoring/injection (default: %s)\n"
			"-m <mac addr>  use the specified mac address (default: from phys)\n"
			, DEFAULT_CHANNEL, g_iface);
}


/*
 * The main function of this program simply checks prelimary arguments and
 * and launches the attack.
 */
int main(int argc, char *argv[])
{
	char *argv0;
	int ret = 0, c;
	pcap_t *pch = NULL;
	struct pcap_pkthdr *pchdr = NULL;
	const u_char *inbuf = NULL;
	int pcret;

	/* initalize stuff */
	srand(getpid());
	strcpy(g_iface, "mon0");

	argv0 = "jfap";
	if (argv && argc > 0 && argv[0])
		argv0 = argv[0];

	if (argc < 2) {
		usage(argv0);
		return 1;
	}

	while ((c = getopt(argc, argv, "bc:i:m:")) != -1) {
		switch (c) {
			case '?':
			case 'h':
				usage(argv0);
				return 1;

			case 'b':
				g_send_beacons = 1;
				break;

			case 'c':
				{
					int tmp = atoi(optarg);
					if (tmp < 1 || tmp > 12) {
						fprintf(stderr, "[!] invalid channel: %s\n", optarg);
						return 1;
					}

					g_channel = tmp;
				}
				break;

			case 'i':
				strncpy(g_iface, optarg, sizeof(g_iface) - 1);
				break;

			case 'm':
				{
					struct ether_addr *pe;

					pe = ether_aton(optarg);
					if (!pe) {
						fprintf(stderr, "[!] invalid mac address: %s\n", optarg);
						return 1;
					}
					memcpy(g_bssid, pe->ether_addr_octet, ETH_ALEN);
				}
				break;

			default:
				fprintf(stderr, "[!] invalid option '%c'! try -h ...\n", c);
				return 1;
				/* not reached */
				break;
		}
	}

	/* adjust params */
	argc -= optind;
	argv += optind;

	/* process required arguments */
	if (argc < 1) {
		usage(argv0);
		return 1;
	}

	strncpy((char *)g_ssid, argv[0], sizeof(g_ssid) - 1);
	g_ssid_len = strlen((char *)g_ssid);

	printf("[*] Starting access point with SSID \"%s\" via interface \"%s\"\n",
			g_ssid, g_iface);

	if (!start_pcap(&pch))
		return 1;

	if ((g_sock = open_raw_socket()) == -1)
		return 1;

	/* set the channel for the wireless card */
	if (!set_channel())
		return 1;

	while (1) {
		pcret = pcap_next_ex(pch, &pchdr, &inbuf);
		if (pcret == -1) {
			pcap_perror(pch, "[!] Failed to get a packet");
			continue;
		}

		/* if we got a packet, process it */
		if (pcret == 1) {
			/* check the length against the capture length */
			if (pchdr->len > pchdr->caplen)
				fprintf(stderr, "[-] WARNING: truncated frame! (len: %lu > caplen: %lu)\n",
						(ulong)pchdr->len, (ulong)pchdr->caplen);

			if (!handle_packet(inbuf, pchdr->caplen)) {
				ret = 1;
				break;
			}
		}

		if (!process_periodic_tasks()) {
			ret = 1;
			break;
		}
	}

	pcap_close(pch);
	return ret;
}


/*
 * handle a single packet from the wifi nic
 */
int handle_packet(const u_char *data, u_int32_t left)
{
	dot11_frame_t *d11;

	if (!process_radiotap(&data, &left))
		return 1; /* treat errors as warnings */

	if (!(d11 = get_dot11_frame(&data, &left)))
		return 1; /* treat errors as warnings */

	/* ignore anything from us */
	if (!memcmp(d11->src_mac, g_bssid, ETH_ALEN))
		return 1; /* finished with this packet */

	/* handle retransmissions */
	if (d11->ctrlflags & CF_RETRY) {
		/* if we have a packet that we tried to send, re-send it now */
		if (g_pkt_len > 0) {
			/* don't retransmit too fast */
			struct timespec now, diff;

			if (clock_gettime(CLOCK_REALTIME, &now)) {
				perror("[!] gettimeofday failed");
				return 0;
			}

			/* see how long since the last retransmit. if it's been
			 * long enough, send again */
			timespec_diff(&now, &last_retransmit, &diff);

			if (diff.tv_sec > 0 || diff.tv_nsec > BEACON_INTERVAL * 100000) {
#ifdef DEBUG_RETRANSMIT
				printf("[*] Re-transmitting...\n");
#endif
				if (send(g_sock, g_pkt, g_pkt_len, 0) == -1) {
					perror("[!] Unable to re-send packet!");
					/* just try again later */
				}
				last_retransmit = now;
			}
		}

		/* don't process retransmission packets further */
		return 1;
	}

	/* handle broadcast packets - only probe requests */
	if (d11->type == T_MGMT && d11->subtype == ST_PROBE_REQ) {
		if (!process_probe_request(d11, data, left))
			return 1; /* finished with this packet */
		return 1; /* finished with this packet */
	}

	/* from here on out, we only handle unicast packets */
	if (memcmp(d11->dst_mac, g_bssid, ETH_ALEN)) {
#ifdef DEBUG_IGNORED
		printf("[*] Ingoring 802.11 packet ver:%u type:%s subtype:%s\n",
				d11->version, dot11_types[d11->type],
				dot11_subtypes[d11->type][d11->subtype]);
		hexdump(data, left);
#endif
		return 1; /* finished with this packet */
	}

	if (d11->type == T_MGMT) {
		if (d11->subtype == ST_AUTH)
			return process_auth_request(d11, data, left);

		else if (d11->subtype == ST_ASSOC_REQ)
			return process_assoc_request(d11, data, left);

	} /* type check */

	else if (d11->type == T_DATA) {
		if (g_state != S_ESTABLISHED) {
			g_state = S_ESTABLISHED;
			g_pkt_len = 0;
#ifndef DEBUG_DATA
			printf("[*] Station successfully associated and is sending data...\n");
#endif
		}
#ifdef DEBUG_DATA
		printf("[*] Unhandled 802.11 packet ver:%u type:%s subtype:%s%s\n",
				d11->version, dot11_types[d11->type],
				dot11_subtypes[d11->type][d11->subtype],
				(d11->subtype >> 3) ? " (QoS)" : "");
		hexdump(data, left);
#endif
		return 1;
	}

	/* if we didn't handle this packet somehow, we should display it */
#ifdef DEBUG_DOT11
	printf("[*] Unhandled 802.11 packet ver:%u type:%s subtype:%s\n",
			d11->version, dot11_types[d11->type],
			dot11_subtypes[d11->type][d11->subtype]);
	hexdump(data, left);
#endif

	return 1;
}


/*
 * handle periodic tasks that need to be done
 */
int process_periodic_tasks(void)
{
	if (g_send_beacons) {
		/* we didn't get a pcket yet, do periodic processing */
		struct timespec now, diff;

		if (clock_gettime(CLOCK_REALTIME, &now)) {
			perror("[!] clock_gettime failed");
			return 0;
		}

		/* see how long since the last beacon. if it's been long enough,
		 * send another */
		timespec_diff(&now, &last_beacon, &diff);
		if (diff.tv_sec > 0 || diff.tv_nsec > BEACON_INTERVAL * 1000000) {
#ifdef DEBUG_BEACON_INTERVAL
			printf("%lu.%lu - %lu.%lu = %lu.%lu (vs %lu)\n",
					(ulong)now.tv_sec, now.tv_nsec,
					(ulong)last_beacon.tv_sec, last_beacon.tv_nsec,
					(ulong)diff.tv_sec, diff.tv_nsec,
					(ulong)BEACON_INTERVAL * 1000000);
#endif
			if (!send_beacon())
				return 1; /* treat error as warning */
			last_beacon = now;
		}
	} /* if (g_send_beacons) */

	return 1;
}


/*
 * try to start capturing packets from the specified interface (a wireless card
 * in monitor mode)
 *
 * on succes, we return 1, on failure, 0
 */
int start_pcap(pcap_t **pcap)
{
	char errorstr[PCAP_ERRBUF_SIZE];
	int datalink;

	printf("[*] Starting capture on \"%s\" ...\n", g_iface);

	*pcap = pcap_open_live(g_iface, SNAPLEN, 8, 25, errorstr);
	if (*pcap == (pcap_t *)NULL) {
		fprintf(stderr, "[!] pcap_open_live() failed: %s\n", errorstr);
		return 0;
	}

	datalink = pcap_datalink(*pcap);
	switch (datalink) {
		case DLT_IEEE802_11_RADIO:
			break;

		default:
			fprintf(stderr, "[!] Unknown datalink for interface \"%s\": %d\n",
					g_iface, datalink);
			fprintf(stderr, "    Only RADIOTAP is currently supported.\n");
			return 0;
	}

	return 1;
}


/*
 * open a raw socket that we can use to send raw 802.11 frames
 */
int open_raw_socket(void)
{
	int sock;
	struct sockaddr_ll la;
	struct ifreq ifr;

	sock = socket(PF_PACKET, SOCK_RAW, ETH_P_ALL);
	if (sock == -1) {
		perror("[!] Unable to open raw socket");
		return -1;
	}

	/* build the link-level address struct for binding */
	memset(&la, 0, sizeof(la));
	la.sll_family = AF_PACKET;
	la.sll_halen = ETH_ALEN;

	/* get the interface index */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, g_iface, IFNAMSIZ);
	if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
		perror("[!] Unable to get interface index");
		close(sock);
		return -1;
	}
#ifdef DEBUG_IF_INDEX
	printf("[*] Interface index: %u\n", ifr.ifr_ifindex);
#endif
	la.sll_ifindex = ifr.ifr_ifindex;

	if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
		perror("[!] Unable to get hardware address");
		close(sock);
		return -1;
	}
#ifdef DEBUG_IF_HWADDR
	printf("[*] Interface hardware address: %s\n", mac_string((u_int8_t *)ifr.ifr_hwaddr.sa_data));
#endif
	if (!memcmp(g_bssid, "\x00\x00\x00\x00\x00\x00", ETH_ALEN))
		memcpy(g_bssid, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
	 memcpy(la.sll_addr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	 /* verify the interface uses RADIOTAP */
	 if (ifr.ifr_hwaddr.sa_family != ARPHRD_IEEE80211_RADIOTAP) {
		 fprintf(stderr, "[!] bad address family: %u\n", ifr.ifr_hwaddr.sa_family);
		 close(sock);
		 return -1;
	 }

	 /* bind this socket to the interface */
	 if (bind(sock, (struct sockaddr *)&la, sizeof(la)) == -1) {
		perror("[!] Unable to bind to interface");
		close(sock);
		return -1;
	}
	return sock;
}


/*
 * set the channel of the wireless card
 */
int set_channel(void)
{
	char cmd[256] = { 0 };

	snprintf(cmd, sizeof(cmd) - 1, "iwconfig %s channel %d", g_iface, g_channel);
	if (system(cmd))
		return 0;
	return 1;
}


/*
 * process the radiotap header
 */
int process_radiotap(const u_char **ppkt, u_int32_t *pleft)
{
	const u_char *p = *ppkt;
	radiotap_t *prt = (radiotap_t *)p;
#ifdef DEBUG_RADIOTAP_PRESENT
	int idx = 0;
	u_int32_t *pu = &prt->it_present;
#endif

	if (*pleft < sizeof(radiotap_t)) {
		fprintf(stderr, "[!] Packet doesn't have enough data for a radiotap header?!\n");
		return 0;
	}

#ifdef DEBUG_RADIOTAP
	printf("[*] got RADIOTAP packet - ver:%u pad:%u len:%u\n",
			prt->it_version, prt->it_pad, prt->it_len);
#endif
	if (*pleft <= prt->it_len) {
		fprintf(stderr, "[!] Packet is too small to contain the radiotap header and data\n");
		return 0;
	}

#ifdef DEBUG_RADIOTAP_PRESENT
	printf("    present[%u]: 0x%lx\n", idx, (ulong)prt->it_present);
	while (prt->it_present & 0x1) {
		++idx;
		printf("    present[%u]: 0x%lx\n", idx, (ulong)pu[idx]);
	}
#endif

	*ppkt = p + prt->it_len;
	*pleft -= prt->it_len;

	return 1;
}


/*
 * process the 802.11 frame
 */
dot11_frame_t *get_dot11_frame(const u_char **ppkt, u_int32_t *pleft)
{
	const u_char *p = *ppkt;

	if (*pleft < sizeof(dot11_frame_t)) {
#ifdef DEBUG_DOT11_SHORT_PKTS
		fprintf(stderr, "[-] Not enough data for 802.11 frame header (bytes left: %u)!\n", *pleft);
		hexdump(p, *pleft);
#endif
		return NULL;
	}

	*ppkt = p + sizeof(dot11_frame_t);
	*pleft -= sizeof(dot11_frame_t);

	return (dot11_frame_t *)p;
}


/*
 * process an 802.11 probe request
 */
int process_probe_request(dot11_frame_t *d11, const u_char *data, u_int32_t left)
{
	ie_t *ie;
	char ssid_req[32] = { 0 };

	if (!(ie = get_ssid_ie(data, left))) {
		fprintf(stderr, "[-] Probe request with no SSID encountered!\n");
		return 1; /* just a warning */
	}

	/* the ssid might be empty, or might be too long */
	if (ie->len > 0)
		strcpy(ssid_req, ssid_string(ie));

	/* there are broadcast and unicast probe requests... */
	if (!memcmp(d11->dst_mac, g_bssid, ETH_ALEN)) {
		/* for us!? */
#ifndef DONT_CHECK_SSID_ON_UNICAST
		if (!strcmp(ssid_req, (char *)g_ssid)) {
			printf("[*] (%s) Probe request for our BSSID and SSID, replying...\n", mac_string(d11->src_mac));
			g_state = S_SENT_PROBE_RESP;
			if (!send_probe_response(d11->src_mac))
				return 1; /* treat send errors as a warning */
		}
#else
		printf("[*] (%s) Probe request for our BSSID, replying...\n", mac_string(d11->src_mac));
		g_state = S_SENT_PROBE_RESP;
		if (!send_probe_response(d11->src_mac))
			return 1; /* treat send errors as a warning */
#endif
	} else if (!memcmp(d11->dst_mac, IEEE80211_BROADCAST_ADDR, ETH_ALEN)) {
		/* broadcast probe request - discovery? */
		/* NOTE: this is the active-scan equivalent of a beacon -- no state change here */
		if (ie && ie->len > 0) {
			/* we must check the SSID on broadcast probes */
			if (!strcmp(ssid_req, (char *)g_ssid)) {
				printf("[*] (%s) Broadcast probe request for our SSID \"%s\" received, replying...\n", mac_string(d11->src_mac), ssid_req);
				if (!send_probe_response(d11->src_mac))
					return 1; /* treat send errors as a warning */
			} else {
				printf("[*] (%s) Broadcast probe request for \"%s\" received, NOT replying...\n", mac_string(d11->src_mac), ssid_req);
			}
		} else {
			printf("[*] (%s) Broadcast probe request received, replying...\n", mac_string(d11->src_mac));
			if (!send_probe_response(d11->src_mac))
				return 1; /* treat send errors as a warning */
		}
	} /* mac check */
	else {
		if (ie->len > 0) {
			printf("[*] (%s) Unhandled probe request for SSID (%u bytes): \"%s\"\n", mac_string(d11->src_mac), ie->len, ssid_req);
		} else {
			printf("[*] (%s) Unhandled probe request for empty SSID\n", mac_string(d11->src_mac));
		}
	}
	return 1;
}


/*
 * process an 802.11 authentication request
 */
int process_auth_request(dot11_frame_t *d11, const u_char *data, u_int32_t left)
{
	auth_t *auth;

	if (left < sizeof(auth_t)) {
		fprintf(stderr, "[-] (%s) Auth request without parameters!\n", mac_string(d11->src_mac));
		return 1;
	}

	auth = (auth_t *)data;
	if (auth->seq != 1) {
		fprintf(stderr, "[-] Authentication sequence is not 0x0001 !!\n");
	}

	printf("[*] (%s) Auth request received (alg:0x%x, seq:%u, status:%u), replying...\n",
			mac_string(d11->src_mac),
			auth->algorithm, auth->seq, auth->status);

	g_state = S_SENT_AUTH;
	if (!send_auth_response(d11->src_mac))
		return 1; /* treat send errors as a warning */

	return 1;
}


/*
 * process an 802.11 association request destined for us
 */
int process_assoc_request(dot11_frame_t *d11, const u_char *data, u_int32_t left)
{
	assoc_req_t *assoc;
	ie_t *ie;

	if (left < sizeof(assoc_req_t)) {
		fprintf(stderr, "[-] (%s) Association request without parameters!\n", mac_string(d11->src_mac));
		return 1;
	}
	assoc = (assoc_req_t *)data;

	data += sizeof(assoc_req_t);
	left -= sizeof(assoc_req_t);

	if (!(ie = get_ssid_ie(data, left))) {
		printf("[*] (%s) Association request without SSID received (caps:0x%x, interval: %u), replying...\n",
				mac_string(d11->src_mac),
				assoc->caps, assoc->interval);
	} else {
		printf("[*] (%s) Association request for \"%s\" received (caps:0x%x, interval: %u), replying...\n",
				mac_string(d11->src_mac),
				ssid_string(ie),
				assoc->caps, assoc->interval);
	}

	g_state = S_SENT_ASSOC_RESP;
	if (!send_assoc_response(d11->src_mac))
		return 1; /* treat send errors as a warning */
	return 1;
}


/*
 * send an 802.11 packet with a bunch of re-transmissions for the fuck of it
 */
int send_packet(dot11_frame_t *d11)
{
	if (send(g_sock, g_pkt, g_pkt_len, 0) == -1) {
		perror("[!] Unable to send packet!");
		return 0;
	}

	/* set the retransmit flag on the 802.11 header */
	d11->ctrlflags |= CF_RETRY;

	return 1;
}


/*
 * fill the radio tap header in for a packet
 */
void fill_radiotap(u_int8_t **ppkt)
{
	u_int8_t *p = *ppkt;
	radiotap_t *prt = (radiotap_t *)p;

	/* fill out the radio tap header */
	prt->it_version = 0;
	prt->it_len = sizeof(*prt) + 1;
	prt->it_present = (1 << IEEE80211_RADIOTAP_RATE);

	p += sizeof(radiotap_t);

	/* add the data rate (data of the radiotap header) */
	*p++ = 0x4;  // 2Mb/s

	*ppkt = p;
}


/*
 * fill the 802.11 frame header
 */
void fill_dot11(u_int8_t **ppkt, u_int8_t type, u_int8_t subtype, u_int8_t *dst_mac)
{
	dot11_frame_t *d11 = (dot11_frame_t *)(*ppkt);

	/* add the 802.11 header */
	d11->version = 0;
	d11->type = type;
	d11->subtype = subtype;
	d11->ctrlflags = 0;
	d11->duration = 0;
	memcpy(d11->dst_mac, dst_mac, ETH_ALEN);
	memcpy(d11->src_mac, g_bssid, ETH_ALEN);
	memcpy(d11->bssid, g_bssid, ETH_ALEN);
	d11->seq = get_sequence();
	d11->frag = 0;

	*ppkt += sizeof(dot11_frame_t);
}


/*
 * fill in an information element
 */
void fill_ie(u_int8_t **ppkt, u_int8_t id, u_int8_t *data, u_int8_t len)
{
	u_int8_t *p = *ppkt;
	ie_t *ie = (ie_t *)p;

	/* add the ssid IE */
	ie->id = id;
	ie->len = len;

	p = (u_int8_t *)(ie + 1);
	memcpy(p, data, len);

	p += len;
	*ppkt = p;
}


/*
 * send a beacon frame to announce our network
 */
int send_beacon()
{
	u_int8_t pkt[4096] = { 0 }, *p = pkt;
	beacon_t *bc;

	fill_radiotap(&p);
	fill_dot11(&p, T_MGMT, ST_BEACON, IEEE80211_BROADCAST_ADDR);

	/* add the beacon info */
	bc = (beacon_t *)p;
	bc->timestamp = 0;
	bc->interval = BEACON_INTERVAL;
	bc->caps = 1; // we are an AP ;-)
	p = (u_int8_t *)(bc + 1);

	fill_ie(&p, IEID_SSID, g_ssid, g_ssid_len);
	fill_ie(&p, IEID_RATES, (u_int8_t *)"\x0c\x12\x18\x24\x30\x48\x60\x6c", 8);
	fill_ie(&p, IEID_DSPARAMS, &g_channel, 1);

	/* don't retransmit beacons */
	if (send(g_sock, pkt, p - pkt, 0) == -1) {
		perror("[!] Unable to send beacon!");
		return 0;
	}

	//printf("[*] Sent beacon!\n");
	return 1;
}


/*
 * send a probe response to the specified sender
 */
int send_probe_response(u_int8_t *dst_mac)
{
	u_int8_t *p = g_pkt;
	dot11_frame_t *d11;
	beacon_t *bc;

	fill_radiotap(&p);
	d11 = (dot11_frame_t *)p;
	fill_dot11(&p, T_MGMT, ST_PROBE_RESP, dst_mac);

	/* add the beacon info */
	bc = (beacon_t *)p;
	bc->timestamp = 0;
	bc->interval = BEACON_INTERVAL;
	bc->caps = 1; // we are an AP ;-)
	p = (u_int8_t *)(bc + 1);

	fill_ie(&p, IEID_SSID, g_ssid, g_ssid_len);
	fill_ie(&p, IEID_RATES, (u_int8_t *)"\x0c\x12\x18\x24\x30\x48\x60\x6c", 8);
	fill_ie(&p, IEID_DSPARAMS, &g_channel, 1);

	g_pkt_len = p - g_pkt;
	if (!send_packet(d11))
		return 0;

	//printf("[*] Sent probe response to %s!\n", mac_string(dst_mac));
	return 1;
}


/*
 * send an authentication response
 */
int send_auth_response(u_int8_t *dst_mac)
{
	u_int8_t *p = g_pkt;
	dot11_frame_t *d11;
	auth_t *auth;

	fill_radiotap(&p);
	d11 = (dot11_frame_t *)p;
	fill_dot11(&p, T_MGMT, ST_AUTH, dst_mac);

	/* add the auth info */
	auth = (auth_t *)p;
	auth->algorithm = 0; // AUTH_OPEN;
	auth->seq = 2; // should be responding to auth seq 1
	auth->status = 0; // successful
	p = (u_int8_t *)(auth + 1);

	g_pkt_len = p - g_pkt;
	if (!send_packet(d11))
		return 0;

	//printf("[*] Sent auth response to %s!\n", mac_string(dst_mac));
	return 1;
}


/*
 * send an association response
 */
int send_assoc_response(u_int8_t *dst_mac)
{
	u_int8_t *p = g_pkt;
	dot11_frame_t *d11;
	assoc_resp_t *assoc;

	fill_radiotap(&p);
	d11 = (dot11_frame_t *)p;
	fill_dot11(&p, T_MGMT, ST_ASSOC_RESP, dst_mac);

	/* add the assoc info */
	assoc = (assoc_resp_t *)p;
	assoc->caps = 1;
	assoc->status = 0; // successful
	assoc->id = 1;
	p = (u_int8_t *)(assoc + 1);

	fill_ie(&p, IEID_RATES, (u_int8_t *)"\x0c\x12\x18\x24\x30\x48\x60\x6c", 8);

	g_pkt_len = p - g_pkt;
	if (!send_packet(d11))
		return 0;

	//printf("[*] Sent association response to %s!\n", mac_string(dst_mac));
	return 1;
}


/*
 * process the information elements looking for an SSID
 */
ie_t *get_ssid_ie(const u_int8_t *data, u_int32_t left)
{
	ie_t *ie;
	const u_int8_t *p = data;
	u_int32_t rem = left;

#ifdef DEBUG_GET_SSID_IE
	printf("[*] processing information element data:\n");
	hexdump(data, left);
#endif

	while (rem > 0) {
		/* see if we have enough for the IE header */
		if (rem < sizeof(*ie)) {
			fprintf(stderr, "[-] Not enough data for an IE!\n");
			return NULL;
		}

		ie = (ie_t *)p;

		/* advance... */
		p += sizeof(*ie);
		rem -= sizeof(*ie);

		/* now, is it an SSID ? */
		if (ie->id == IEID_SSID) {
			return ie;
		}

		/* check if we have all the data */
		if (rem < ie->len) {
			fprintf(stderr, "[-] Not enough data for the IE's data!\n");
			return NULL;
		}

		/* advance past the ie->data */
		p += ie->len;
		rem -= ie->len;
	}

#ifdef DEBUG_GET_SSID_IE
	fprintf(stderr, "[-] SSID IE not found!\n");
#endif
	return NULL;
}


/*
 * create the ascii representation of the specified mac address
 */
char *mac_string(u_int8_t *mac)
{
	static char mac_str[32];
	char *p = mac_str;
	int i;

	for (i = 0; i < ETH_ALEN; i++) {
		u_int8_t hi = mac[i] >> 4;
		u_int8_t lo = mac[i] & 0xf;

		if (hi > 9)
			*p++ = hi - 10 + 'a';
		else
			*p++ = hi + '0';
		if (lo > 9)
			*p++ = lo - 10 + 'a';
		else
			*p++ = lo + '0';
		if (i < ETH_ALEN - 1)
			*p++ = ':';
	}
	*p = '\0';

	return mac_str;
}


/*
 * return the ssid string, ensuring truncation and nul termination
 */
char *ssid_string(ie_t *ie)
{
	static char ssid_str[32];

	memset(ssid_str, 0, sizeof(ssid_str));
	if (ie->len > sizeof(ssid_str) - 1)
		strncpy(ssid_str, (char *)ie->data, sizeof(ssid_str) - 1);
	else
		strncpy(ssid_str, (char *)ie->data, ie->len);
	return ssid_str;
}


/*
 * handle sequence number generation
 */
u_int16_t get_sequence(void)
{
	static u_int16_t sequence = 1337;
	uint16_t ret = sequence;

	sequence++;
	if (sequence > 4095)
		sequence = 0;
	return ret;
}


/*
 * diff two timespec values
 */
void timespec_diff(struct timespec *newer, struct timespec *older, struct timespec *diff)
{
	diff->tv_sec = newer->tv_sec - older->tv_sec;
	diff->tv_nsec = newer->tv_nsec - older->tv_nsec;
	if (diff->tv_nsec < 0) {
		--diff->tv_sec;
		diff->tv_nsec += 1000000000;
	}
}
