/*
 * brcmfmac as the kernel's station (sys/redwood_sta.h) and as what carries en0 (lx_os_net_*()).
 *
 * brcmfmac is a FullMAC driver: the firmware scans and associates, and the driver speaks cfg80211 above
 * it -- its operations (scan, connect, add_key, disconnect) are called here as cfg80211 would call them,
 * and what it reports back to cfg80211 and the network stack (scan results, the connection, received
 * frames) comes here through the names lx_brcmfmac.h gives LinuxKPI's empty inline functions.
 *
 * WPA2: the four-way handshake is ours, with lx_wpa.c as ath11k's station has it: the firmware associates
 * with the RSN element given it here ("wpaie"), key messages arrive as frames of type 0x888e, the answers
 * go out as frames, and the keys agreed are given to the driver (add_key). That runs on the station's
 * thread, never on the thread that polls the interrupt: add_key waits for an answer that thread brings.
 * (This firmware has no supplicant of its own for brcmfmac: "sup_wpa" is not supported. Given the PMK it
 * does not answer the key messages either.) While joined, a thread of its own answers what the access
 * point sends later: the handshake begun again, a new group key.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/cfg80211.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_wpa.h"
#include "lx_brcm.h"

#ifndef PACKET_HOST
#define PACKET_HOST	0			/* (Linux's; LinuxKPI has the others) */
#endif
#define HEARD_MOST	64
#define EAPOL_MOST	512
#define HEADROOM	64			/* in front of a frame sent: what the driver's protocol puts there and more */
#define SCAN_NS		(15 * 1000000000ull)
#define CONNECT_NS	(10 * 1000000000ull)
#define HANDSHAKE_NS	(5 * 1000000000ull)

static struct net_device *ndev;			/* the driver's primary interface (wlan0) */
static volatile bool carrier, stopped;
static enum redwood_sta_phase phase = REDWOOD_STA_OFF;
static struct redwood_sta_bss joined;		/* the network joined or being joined */
static int last_error;

/* the last scan */
static struct redwood_sta_bss *heard;
static volatile unsigned heard_count;
static struct cfg80211_scan_request *scan_request;
static volatile int scan_over;			/* 0 under way, 1 over, -1 aborted */
static struct cfg80211_bss bss_given;		/* (what cfg80211_inform_bss() answers: brcmfmac gives it back unread) */

/* the connection */
static volatile int connected;			/* 0 waiting, 1 connected, -1 refused */
static volatile int lost;			/* the firmware has said the connection is gone (the reason) */
static uint8_t rsn[22];				/* the RSN element we associate with, and message 2 repeats */
static struct lx_wpa wpa;
static uint8_t eapol[EAPOL_MOST];		/* the last key message received, waiting for the station's thread */
static volatile unsigned eapol_length;
static unsigned long long join_began;		/* lx_os_nanoseconds() when the last join began: its lines say how long since */
static volatile unsigned long received;		/* frames received since the join began (key messages and the rest) */

static unsigned long long
join_ms(void)
{
	return ((lx_os_nanoseconds() - join_began) / 1000000ull);
}

/* ---- what brcmfmac reports, instead of to cfg80211 and the network stack ---------------------------- */

int
lx_register_netdev(struct net_device *dev)
{
	char *percent = strchr(dev->name, '%');

	if (percent != NULL && percent[1] == 'd') {
		strlcpy(percent, "0", sizeof(dev->name) - (size_t)(percent - dev->name));	/* ("wlan%d": the first) */
	}
	if (ndev == NULL) {
		ndev = dev;
		lx_tell("lx: brcmfmac: its interface %s (%02x:%02x:%02x:..)\n", dev->name, dev->dev_addr[0], dev->dev_addr[1],
		    dev->dev_addr[2]);
	}
	return (0);
}

void
lx_eth_hw_addr_set(struct net_device *dev, const uint8_t *address)
{
	memcpy(dev->dev_addr, address, ETH_ALEN);
}

/* Linux's: the Ethernet header taken off (the frame's start kept as its "MAC header"), its type returned. */
uint16_t
lx_eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	struct ethhdr *eth;

	skb->dev = dev;
	skb_reset_mac_header(skb);
	eth = (struct ethhdr *)skb->data;
	skb_pull(skb, ETH_HLEN);
	if (is_multicast_ether_addr(eth->h_dest)) {
		skb->pkt_type = is_broadcast_ether_addr(eth->h_dest) ? PACKET_BROADCAST : PACKET_MULTICAST;
	} else if (!ether_addr_equal(eth->h_dest, dev->dev_addr)) {
		skb->pkt_type = PACKET_OTHERHOST;
	} else {
		skb->pkt_type = PACKET_HOST;
	}
	return (ntohs(eth->h_proto) >= ETH_P_802_3_MIN ? eth->h_proto : htons(ETH_P_802_2));
}

/* A frame received: key messages for the handshake, the rest to en0. */
void
lx_netif_rx(struct sk_buff *skb)
{
	const uint8_t *frame = skb_mac_header(skb);
	unsigned long length = skb->len + (unsigned long)(skb->data - frame);

	received++;
	if (length >= ETH_HLEN && frame[12] == 0x88 && frame[13] == 0x8e) {
		lx_tell("lx: brcmfmac: +%llu ms: a key message arrives (%lu bytes)\n", join_ms(), length - ETH_HLEN);
		lx_os_lock();
		if (length - ETH_HLEN <= EAPOL_MOST) {
			memcpy(eapol, frame + ETH_HLEN, length - ETH_HLEN);
			eapol_length = (unsigned)(length - ETH_HLEN);
			lx_os_wakeup(&eapol_length);
		}
		lx_os_unlock();
	} else {
		lx_os_net_input(frame, length);
	}
	kfree_skb(skb);
}

void
lx_netif_carrier(struct net_device *dev, bool on)
{
	carrier = on;
}

bool
lx_netif_carrier_ok(struct net_device *dev)
{
	return (carrier);
}

void
lx_netif_queue(struct net_device *dev, bool stop)
{
	stopped = stop;
}

bool
lx_netif_queue_stopped(struct net_device *dev)
{
	return (stopped);
}

static void
heard_one(const uint8_t *bssid, struct linuxkpi_ieee80211_channel *channel, uint16_t capability, uint16_t interval,
    const uint8_t *ie, size_t ie_len, int dbm)
{
	struct redwood_sta_bss *bss;
	unsigned i;

	if (heard == NULL) {
		return;
	}
	lx_os_lock();
	for (i = 0; i < heard_count && memcmp(heard[i].bssid, bssid, ETH_ALEN) != 0; i++) {
	}
	if (i == heard_count && heard_count < HEARD_MOST) {
		heard_count++;
	}
	if (i < heard_count) {
		bss = &heard[i];
		bzero(bss, sizeof(*bss));
		memcpy(bss->bssid, bssid, ETH_ALEN);
		bss->frequency = channel != NULL ? channel->center_freq : 0;
		bss->rssi = (int16_t)dbm;
		bss->capability = capability;
		bss->beacon_interval = interval;
		for (size_t at = 0; at + 2 <= ie_len && at + 2 + ie[at + 1] <= ie_len; at += 2 + ie[at + 1]) {
			if (ie[at] == 0 && ie[at + 1] <= sizeof(bss->ssid)) {
				memcpy(bss->ssid, ie + at + 2, ie[at + 1]);
				bss->ssid_length = ie[at + 1];
				break;
			}
		}
		if (ie_len <= sizeof(bss->elements)) {
			memcpy(bss->elements, ie, ie_len);
			bss->elements_length = (uint16_t)ie_len;
		}
	}
	lx_os_unlock();
}

struct cfg80211_bss *
lx_cfg80211_inform_bss_data(struct wiphy *wiphy, struct cfg80211_inform_bss *data, enum cfg80211_bss_frame_type type,
    const uint8_t *bssid, uint64_t tsf, uint16_t capability, uint16_t interval, const uint8_t *ie, size_t ie_len, gfp_t gfp)
{
	heard_one(bssid, data->chan, capability, interval, ie, ie_len, data->signal / 100);	/* (mBm) */
	return (&bss_given);
}

struct cfg80211_bss *
lx_cfg80211_inform_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel, enum cfg80211_bss_frame_type type,
    const uint8_t *bssid, uint64_t tsf, uint16_t capability, uint16_t interval, const uint8_t *ie, size_t ie_len, int signal,
    gfp_t gfp)
{
	heard_one(bssid, channel, capability, interval, ie, ie_len, signal / 100);
	return (&bss_given);
}

void
lx_cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss)
{
}

void
lx_cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info)
{
	lx_os_lock();
	scan_over = info != NULL && info->aborted ? -1 : 1;
	lx_os_wakeup(&scan_over);
	lx_os_unlock();
}

void
lx_cfg80211_connect_done(struct net_device *dev, struct cfg80211_connect_resp_params *params, gfp_t gfp)
{
	lx_tell("lx: brcmfmac: +%llu ms: the firmware says the connection %s (status %d)\n", join_ms(),
	    params->status == 0 ? "IS MADE" : "WAS REFUSED", params->status);
	lx_os_lock();
	connected = params->status == 0 ? 1 : -1;
	lx_os_wakeup(&connected);
	lx_os_unlock();
}

void
lx_cfg80211_disconnected(struct net_device *dev, uint16_t reason, void *ie, int ie_len, bool locally, gfp_t gfp)
{
	bool was_joined = phase == REDWOOD_STA_JOINED;

	lx_tell("lx: brcmfmac: +%llu ms: the connection is gone (reason %u%s); %lu frames received since the join began\n",
	    join_ms(), reason, locally ? ", ours" : "", received);
	lx_os_lock();
	lost = reason != 0 ? reason : 1;
	if (connected == 0) {
		connected = -1;
	}
	lx_os_wakeup(&connected);
	lx_os_wakeup(&eapol_length);
	lx_os_unlock();
	if (was_joined && !locally) {
		phase = REDWOOD_STA_IDLE;
		explicit_bzero(&wpa, sizeof(wpa));
		last_error = ECONNRESET;
		lx_os_net_link(0);
		lx_os_station_lost(ECONNRESET);
	}
}

void
lx_cfg80211_port_authorized(struct net_device *dev, const uint8_t *bssid, const uint8_t *bitmap, uint8_t len, gfp_t gfp)
{
	lx_tell("lx: brcmfmac: the firmware says the port is authorized\n");
}

/* ---- frames to the network ------------------------------------------------------------------------- */

static int
send_frame(const unsigned char *frame, unsigned long length)
{
	struct sk_buff *skb;

	if (ndev == NULL || length < ETH_HLEN) {
		return (ENXIO);
	}
	if (stopped) {
		return (EAGAIN);
	}
	skb = dev_alloc_skb(length + HEADROOM);
	if (skb == NULL) {
		return (ENOMEM);
	}
	skb_reserve(skb, HEADROOM);
	memcpy(skb_put(skb, length), frame, length);
	skb->dev = ndev;
	return (ndev->netdev_ops->ndo_start_xmit(skb, ndev) == NETDEV_TX_OK ? 0 : EIO);
}

int
lx_brcm_transmit(const unsigned char *frame, unsigned long length)
{
	if (phase != REDWOOD_STA_JOINED) {
		return (ENETDOWN);
	}
	return (send_frame(frame, length));
}

/* ---- the station ----------------------------------------------------------------------------------- */

/* Sleeps until *what is no longer `while_is`, or the time runs out: what it is then. */
static int
wait_while(volatile int *what, int while_is, unsigned long long ns)
{
	unsigned long long until = lx_os_nanoseconds() + ns;
	int value;

	lx_os_lock();
	while (*what == while_is && lx_os_nanoseconds() < until) {
		lx_os_sleep((const void *)what, 50 * 1000000ull);
	}
	value = *what;
	lx_os_unlock();
	return (value);
}

static int
sta_power(bool on)
{
	if (on) {
		return (0);			/* (up since the station was offered) */
	}
	if (phase == REDWOOD_STA_JOINED || phase == REDWOOD_STA_JOINING) {
		lx_brcm_ops->disconnect(lx_brcm_wiphy, ndev, WLAN_REASON_DEAUTH_LEAVING);
		lx_os_net_link(0);
	}
	phase = REDWOOD_STA_IDLE;
	explicit_bzero(&wpa, sizeof(wpa));
	return (0);
}

static int
sta_scan(const struct redwood_sta_scan *what)
{
	struct cfg80211_scan_request *request;
	struct cfg80211_ssid *ssid = NULL;
	int error;

	request = kzalloc(sizeof(*request), GFP_KERNEL);
	if (request == NULL) {
		return (ENOMEM);
	}
	if (what->ssid_length != 0) {
		ssid = kzalloc(sizeof(*ssid), GFP_KERNEL);
		if (ssid == NULL) {
			kfree(request);
			return (ENOMEM);
		}
		memcpy(ssid->ssid, what->ssid, what->ssid_length);
		ssid->ssid_len = what->ssid_length;
		request->ssids = ssid;
		request->n_ssids = 1;
	}
	request->wdev = ndev->ieee80211_ptr;
	request->wiphy = lx_brcm_wiphy;
	request->n_channels = 0;		/* (every channel, the firmware's own way) */
	lx_os_lock();
	heard_count = 0;
	scan_over = 0;
	lx_os_unlock();
	if (phase == REDWOOD_STA_IDLE) {
		phase = REDWOOD_STA_SCANNING;
	}
	scan_request = request;
	error = lx_brcm_ops->scan(lx_brcm_wiphy, request);
	if (error == 0) {
		error = wait_while(&scan_over, 0, SCAN_NS);
		error = error == 1 ? 0 : error == 0 ? ETIMEDOUT : EINTR;
	} else {
		error = -error;
	}
	scan_request = NULL;
	if (phase == REDWOOD_STA_SCANNING) {
		phase = REDWOOD_STA_IDLE;
	}
	lx_tell("lx: brcmfmac: the scan is over (%d): %u access points heard\n", error, heard_count);
	for (unsigned i = 0; i < heard_count && i < 12; i++) {
		lx_tell("lx:   %.*s  %02x:%02x:%02x:%02x:%02x:%02x  %u MHz  %d dBm\n", heard[i].ssid_length, heard[i].ssid,
		    heard[i].bssid[0], heard[i].bssid[1], heard[i].bssid[2], heard[i].bssid[3], heard[i].bssid[4], heard[i].bssid[5],
		    heard[i].frequency, heard[i].rssi);
	}
	kfree(ssid);
	kfree(request);
	return (error);
}

static void
sta_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	for (unsigned i = 0; i < heard_count; i++) {
		each(&heard[i], context);
	}
}

/* The access point of that name heard best in the last scan, or NULL. */
static const struct redwood_sta_bss *
best_of(const struct redwood_sta_join *what)
{
	const struct redwood_sta_bss *best = NULL;
	static const uint8_t none[ETH_ALEN];

	for (unsigned i = 0; i < heard_count; i++) {
		const struct redwood_sta_bss *bss = &heard[i];

		if (bss->ssid_length != what->ssid_length || memcmp(bss->ssid, what->ssid, what->ssid_length) != 0) {
			continue;
		}
		if (memcmp(what->bssid, none, ETH_ALEN) != 0 && memcmp(what->bssid, bss->bssid, ETH_ALEN) != 0) {
			continue;
		}
		if (best == NULL || bss->rssi > best->rssi) {
			best = bss;
		}
	}
	return (best);
}

/* The group cipher the access point's RSN element names (CCMP if it names none we know). */
static uint8_t
group_cipher_of(const struct redwood_sta_bss *bss)
{
	for (unsigned at = 0; at + 2 <= bss->elements_length; at += 2 + bss->elements[at + 1]) {
		const uint8_t *e = &bss->elements[at];

		if (e[0] == 0x30 && e[1] >= 6 && e[4] == 0x00 && e[5] == 0x0f && e[6] == 0xac) {
			return (e[7]);
		}
	}
	return (4);
}

static int
key_set(const uint8_t *key, int key_length, uint8_t index, bool pairwise, const uint8_t *seq, uint32_t cipher)
{
	struct key_params params = { .key = key, .key_len = key_length, .seq = seq, .seq_len = seq != NULL ? 6 : 0,
				     .cipher = cipher };

	return (lx_brcm_ops->add_key(lx_brcm_wiphy, ndev, -1, index, pairwise, pairwise ? joined.bssid : NULL, &params));
}

/*
 * One key message answered, and the keys it brings given to the driver (the answer first: message 4 and
 * the group's answer go out under the keys in use). LX_WPA_* of lx_wpa_message(); for LX_WPA_KEYS and
 * LX_WPA_GROUP *error says whether the driver took the keys.
 */
static uint32_t group_cipher_now;		/* of the network joined: 0x000fac00 | its suite type */

static int
key_message(const uint8_t *message, unsigned length, int *error)
{
	uint8_t frame[ETH_HLEN + LX_WPA_REPLY_MOST];
	unsigned reply_length = 0;
	const char *why = NULL;
	int said;

	*error = 0;
	said = lx_wpa_message(&wpa, message, length, frame + ETH_HLEN, &reply_length, &why);
	if (said == LX_WPA_BAD) {
		lx_tell("lx: brcmfmac: a key message that cannot be answered (a wrong passphrase?): %s\n", why != NULL ? why : "?");
		return (said);
	}
	lx_tell("lx: brcmfmac: +%llu ms: the key message is answered (%s)\n", join_ms(), said == LX_WPA_ANSWER ?
	    "message 2" : said == LX_WPA_KEYS ? "message 4" : said == LX_WPA_GROUP ? "the group's" : "nothing to send");
	if (said == LX_WPA_NOTHING) {
		return (said);
	}
	memcpy(frame, joined.bssid, ETH_ALEN);
	memcpy(frame + ETH_ALEN, wpa.own, ETH_ALEN);
	frame[12] = 0x88;
	frame[13] = 0x8e;
	if (send_frame(frame, ETH_HLEN + reply_length) != 0) {
		lx_tell("lx: brcmfmac: the answer to a key message could not be sent\n");
		*error = EIO;
		return (said);
	}
	if (said == LX_WPA_KEYS) {
		int pairwise = key_set(wpa.pairwise, 16, 0, true, NULL, WLAN_CIPHER_SUITE_CCMP);
		int group = key_set(wpa.group, (int)wpa.group_length, (uint8_t)wpa.group_index, false, wpa.group_counter,
		    group_cipher_now);

		lx_tell("lx: brcmfmac: THE HANDSHAKE IS DONE: the pairwise key given to the driver (%d), the group's "
		    "(number %u, %u bytes) %d\n", pairwise, wpa.group_index, wpa.group_length, group);
		*error = pairwise != 0 ? -pairwise : group != 0 ? -group : 0;
	} else if (said == LX_WPA_GROUP) {
		int group = key_set(wpa.group, (int)wpa.group_length, (uint8_t)wpa.group_index, false, wpa.group_counter,
		    group_cipher_now);

		lx_tell("lx: brcmfmac: the group's new key (number %u) is given to the driver: %d\n", wpa.group_index, group);
		*error = group != 0 ? -group : 0;
	}
	return (said);
}

/* A message 1 (pairwise, acknowledge, not signed): a new handshake, which a nonce of ours of its own begins. */
static bool
is_message_1(const uint8_t *message, unsigned length)
{
	unsigned info = length >= 7 ? (unsigned)message[5] << 8 | message[6] : 0;

	return ((info & 0x0188) == 0x0088);
}

/*
 * Key messages while joined: the access point begins the handshake again now and then (a new pairwise
 * key, seen 45 s in) or sends a new group key, and sends the network's members away if nobody answers.
 */
static void
keys_thread(void *argument)
{
	uint8_t message[EAPOL_MOST];

	for (;;) {
		unsigned length = 0;
		int error;

		lx_os_lock();
		while (eapol_length == 0 || phase != REDWOOD_STA_JOINED) {
			lx_os_sleep(&eapol_length, 100 * 1000000ull);
		}
		length = eapol_length;
		memcpy(message, eapol, length);
		eapol_length = 0;
		lx_os_unlock();
		if (is_message_1(message, length)) {
			get_random_bytes(wpa.own_nonce, sizeof(wpa.own_nonce));
		}
		key_message(message, length, &error);
	}
}

/* The handshake: key messages answered until both keys are there. 0, or an errno. */
static int
handshake(uint32_t group_cipher)
{
	unsigned long long until = lx_os_nanoseconds() + HANDSHAKE_NS;
	uint8_t message[EAPOL_MOST];

	group_cipher_now = 0x000fac00u | group_cipher;

	for (;;) {
		unsigned length = 0;
		int said, error;

		lx_os_lock();
		while (eapol_length == 0 && lost == 0 && lx_os_nanoseconds() < until) {
			lx_os_sleep(&eapol_length, 50 * 1000000ull);
		}
		if (eapol_length != 0) {
			length = eapol_length;
			memcpy(message, eapol, length);
			eapol_length = 0;
		}
		lx_os_unlock();
		if (lost != 0) {
			return (ECONNRESET);
		}
		if (length == 0) {
			lx_tell("lx: brcmfmac: THE ACCESS POINT SENT NO KEY MESSAGE in time\n");
			return (ETIMEDOUT);
		}
		said = key_message(message, length, &error);
		if (said == LX_WPA_KEYS || error != 0) {
			return (error);
		}
	}
}

static int
sta_join(const struct redwood_sta_join *what)
{
	struct cfg80211_connect_params sme;
	const struct redwood_sta_bss *target;
	uint32_t akm = WLAN_AKM_SUITE_PSK, pairwise = WLAN_CIPHER_SUITE_CCMP;
	uint8_t group = 4;
	unsigned long long began = lx_os_nanoseconds();
	int error;

	if (phase == REDWOOD_STA_JOINED) {
		sta_power(false);
	}
	target = best_of(what);
	if (target == NULL) {
		struct redwood_sta_scan scan;

		bzero(&scan, sizeof(scan));
		memcpy(scan.ssid, what->ssid, what->ssid_length);
		scan.ssid_length = what->ssid_length;
		sta_scan(&scan);
		target = best_of(what);
	}
	if (target == NULL) {
		lx_tell("lx: brcmfmac: the network to join was not heard\n");
		last_error = ENOENT;
		return (ENOENT);
	}
	joined = *target;
	phase = REDWOOD_STA_JOINING;
	bzero(&sme, sizeof(sme));
	sme.ssid = what->ssid;
	sme.ssid_len = what->ssid_length;
	sme.bssid = joined.bssid;
	sme.channel = linuxkpi_ieee80211_get_channel(lx_brcm_wiphy, joined.frequency);
	sme.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	if (what->security == REDWOOD_STA_WPA2_PSK) {
		static const uint8_t rsn_psk_ccmp[22] = { 0x30, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0,
							  0x00, 0x0f, 0xac, 2, 0, 0 };

		group = group_cipher_of(&joined);
		memcpy(rsn, rsn_psk_ccmp, sizeof(rsn));
		rsn[7] = group;
		sme.ie = rsn;
		sme.ie_len = sizeof(rsn);
		sme.privacy = 1;
		sme.crypto.wpa_versions = NL80211_WPA_VERSION_2;
		sme.crypto.cipher_group = 0x000fac00u | group;
		sme.crypto.n_ciphers_pairwise = 1;
		sme.crypto.ciphers_pairwise = &pairwise;
		sme.crypto.n_akm_suites = 1;
		sme.crypto.akm_suites = &akm;
		bzero(&wpa, sizeof(wpa));
		if (what->passphrase_length != 0) {
			lx_wpa_pmk(what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, wpa.pmk);
		} else {
			memcpy(wpa.pmk, what->pmk, sizeof(wpa.pmk));
		}
		memcpy(wpa.own, ndev->dev_addr, ETH_ALEN);
		memcpy(wpa.peer, joined.bssid, ETH_ALEN);
		get_random_bytes(wpa.own_nonce, sizeof(wpa.own_nonce));
		wpa.rsn = rsn;
		wpa.rsn_length = sizeof(rsn);
	}
	lx_tell("lx: brcmfmac: joining %.*s at %02x:%02x:%02x:%02x:%02x:%02x, %u MHz, %d dBm (%s)\n", what->ssid_length,
	    what->ssid, joined.bssid[0], joined.bssid[1], joined.bssid[2], joined.bssid[3], joined.bssid[4], joined.bssid[5],
	    joined.frequency, joined.rssi, what->security == REDWOOD_STA_WPA2_PSK ? "WPA2-PSK" : "open");
	lx_os_lock();
	connected = lost = 0;
	eapol_length = 0;
	received = 0;
	join_began = lx_os_nanoseconds();
	lx_os_unlock();
	error = -lx_brcm_ops->connect(lx_brcm_wiphy, ndev, &sme);
	if (error == 0) {
		int answer = wait_while(&connected, 0, CONNECT_NS);

		error = answer == 1 ? 0 : answer == 0 ? ETIMEDOUT : ECONNREFUSED;
	}
	if (error == 0 && what->security == REDWOOD_STA_WPA2_PSK) {
		error = handshake(group);
	}
	if (error != 0) {
		explicit_bzero(&wpa, sizeof(wpa));
		lx_tell("lx: brcmfmac: +%llu ms: THE NETWORK IS NOT JOINED (%d); %lu frames received\n", join_ms(), error,
		    received);
		lx_brcm_ops->disconnect(lx_brcm_wiphy, ndev, WLAN_REASON_DEAUTH_LEAVING);
		phase = REDWOOD_STA_IDLE;
		last_error = error;
		return (error);
	}
	phase = REDWOOD_STA_JOINED;
	last_error = 0;
	lx_os_net_link(1);
	lx_tell("lx: brcmfmac: THE NETWORK IS JOINED, %llu ms after the join began: frames pass\n",
	    (lx_os_nanoseconds() - began) / 1000000ull);
	return (0);
}

static int
sta_leave(void)
{
	return (sta_power(false));
}

static void
sta_state(struct redwood_sta_state *state)
{
	state->phase = phase;
	state->bss = joined;
	state->last_error = last_error;
}

static unsigned
sta_channels(struct redwood_sta_channel *channels, unsigned most)
{
	unsigned count = 0;

	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = lx_brcm_wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels && count < most; i++) {
			if (has->channels[i].flags & IEEE80211_CHAN_DISABLED) {
				continue;
			}
			channels[count].frequency = has->channels[i].center_freq;
			channels[count].flags = ((has->channels[i].flags & IEEE80211_CHAN_NO_IR) ? REDWOOD_STA_CHANNEL_PASSIVE : 0) |
			    ((has->channels[i].flags & IEEE80211_CHAN_RADAR) ? REDWOOD_STA_CHANNEL_RADAR : 0);
			count++;
		}
	}
	return (count);
}

static struct redwood_sta_ops station = {
	.power = sta_power,
	.scan = sta_scan,
	.heard = sta_heard,
	.join = sta_join,
	.leave = sta_leave,
	.state = sta_state,
	.channels = sta_channels,
};

/*
 * When the driver is up: its interface brought up (ndo_open: the firmware's "up"), en0 given the
 * radio's address, and the station offered to the kernel. Waits a few seconds for the interface,
 * which the driver registers after its wiphy.
 */
int
lx_brcm_station_offer(void)
{
	unsigned long long until = lx_os_nanoseconds() + 5 * 1000000000ull;
	int error;

	while (ndev == NULL && lx_os_nanoseconds() < until) {
		lx_pause_ms(50);
	}
	if (ndev == NULL) {
		lx_tell("lx: brcmfmac: the driver has registered no interface\n");
		return (ENXIO);
	}
	if (heard == NULL && (heard = kzalloc(HEARD_MOST * sizeof(*heard), GFP_KERNEL)) == NULL) {
		return (ENOMEM);
	}
	if ((error = lx_os_thread("wifi-keys", keys_thread, NULL)) != 0) {
		return (error);
	}
	error = -ndev->netdev_ops->ndo_open(ndev);
	if (error != 0) {
		lx_tell("lx: brcmfmac: its interface does not come up (%d)\n", error);
		return (error);
	}
	ndev->flags |= IFF_UP;
	phase = REDWOOD_STA_IDLE;
	memcpy(station.address, ndev->dev_addr, ETH_ALEN);
	lx_tell("lx: brcmfmac: %s is up, address %02x:%02x:%02x:%02x:%02x:%02x; en0 and the station are the radio's\n",
	    ndev->name, ndev->dev_addr[0], ndev->dev_addr[1], ndev->dev_addr[2], ndev->dev_addr[3], ndev->dev_addr[4],
	    ndev->dev_addr[5]);
	lx_os_net_attach(ndev->dev_addr);
	lx_os_station(&station);
	return (0);
}

void
lx_brcm_station_withdraw(void)
{
	if (phase == REDWOOD_STA_OFF) {
		return;
	}
	lx_os_station(NULL);
	sta_power(false);
	phase = REDWOOD_STA_OFF;
}
