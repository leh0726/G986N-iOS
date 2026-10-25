/*
 * Included before every file of brcmfmac, after lx_prelude.h (tools/mk-brcmfmac.sh): what its files get
 * in Linux through other headers. linux/delay.h also takes away FreeBSD's msleep() macro (sys/systm.h),
 * which the driver's msleep(ms) calls would otherwise run into; LinuxKPI names LIST_HEAD_INIT
 * LINUX_LIST_HEAD_INIT, because sys/queue.h has a LIST_HEAD of its own. LinuxKPI's memcpy_toio() and its
 * kin are memcpy() and memset(), whose wide unaligned stores device memory refuses (the radio's RAM
 * behind BAR2): ours go in aligned words (lx_base.c).
 */
#include <linux/delay.h>
#include <linux/list.h>
#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name) LINUX_LIST_HEAD_INIT(name)
#endif
#include <linux/io.h>
#undef memset_io
#undef memcpy_fromio
#undef memcpy_toio
void	lx_memset_io(volatile void *to, int value, size_t length);
void	lx_memcpy_fromio(void *to, const volatile void *from, size_t length);
void	lx_memcpy_toio(volatile void *to, const void *from, size_t length);
#define memset_io(a, b, c)	lx_memset_io((a), (b), (c))
#define memcpy_fromio(a, b, c)	lx_memcpy_fromio((a), (b), (c))
#define memcpy_toio(a, b, c)	lx_memcpy_toio((a), (b), (c))

/*
 * What brcmfmac reports to cfg80211 and the network stack, which LinuxKPI has as inline functions that
 * do nothing ("TODO"): the calls go to wifi/lx/brcm/lx_brcm_sta.c instead, where the kernel's station
 * and en0 hear of them. (The headers are read first, here, so that their own definitions keep their
 * names.)
 */
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <net/cfg80211.h>
struct sk_buff;
struct net_device;
int	lx_register_netdev(struct net_device *ndev);
void	lx_netif_rx(struct sk_buff *skb);
uint16_t lx_eth_type_trans(struct sk_buff *skb, struct net_device *ndev);
void	lx_eth_hw_addr_set(struct net_device *ndev, const uint8_t *address);
void	lx_netif_carrier(struct net_device *ndev, bool on);
bool	lx_netif_carrier_ok(struct net_device *ndev);
void	lx_netif_queue(struct net_device *ndev, bool stop);
bool	lx_netif_queue_stopped(struct net_device *ndev);
struct cfg80211_bss *lx_cfg80211_inform_bss_data(struct wiphy *wiphy, struct cfg80211_inform_bss *data,
    enum cfg80211_bss_frame_type type, const uint8_t *bssid, uint64_t tsf, uint16_t capability, uint16_t interval,
    const uint8_t *ie, size_t ie_len, gfp_t gfp);
struct cfg80211_bss *lx_cfg80211_inform_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel,
    enum cfg80211_bss_frame_type type, const uint8_t *bssid, uint64_t tsf, uint16_t capability, uint16_t interval,
    const uint8_t *ie, size_t ie_len, int signal, gfp_t gfp);
void	lx_cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss);
void	lx_cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info);
void	lx_cfg80211_connect_done(struct net_device *ndev, struct cfg80211_connect_resp_params *params, gfp_t gfp);
void	lx_cfg80211_disconnected(struct net_device *ndev, uint16_t reason, void *ie, int ie_len, bool locally, gfp_t gfp);
void	lx_cfg80211_port_authorized(struct net_device *ndev, const uint8_t *bssid, const uint8_t *bitmap, uint8_t len,
    gfp_t gfp);
#define register_netdev(n)			lx_register_netdev(n)
#define register_netdevice(n)			lx_register_netdev(n)
#define cfg80211_register_netdevice(n)		lx_register_netdev(n)
#define netif_rx(s)				lx_netif_rx(s)
#define eth_type_trans(s, d)			lx_eth_type_trans((s), (d))
#define eth_hw_addr_set(d, a)			lx_eth_hw_addr_set((d), (a))
#define netif_carrier_on(d)			lx_netif_carrier((d), true)
#define netif_carrier_off(d)			lx_netif_carrier((d), false)
#define netif_carrier_ok(d)			lx_netif_carrier_ok(d)
#define netif_stop_queue(d)			lx_netif_queue((d), true)
#define netif_wake_queue(d)			lx_netif_queue((d), false)
#define netif_queue_stopped(d)			lx_netif_queue_stopped(d)
#define cfg80211_inform_bss_data(...) lx_cfg80211_inform_bss_data(__VA_ARGS__)
#define cfg80211_inform_bss(...) lx_cfg80211_inform_bss(__VA_ARGS__)
#define cfg80211_put_bss(...) lx_cfg80211_put_bss(__VA_ARGS__)
#define cfg80211_scan_done(...) lx_cfg80211_scan_done(__VA_ARGS__)
#define cfg80211_connect_done(...) lx_cfg80211_connect_done(__VA_ARGS__)
#define cfg80211_disconnected(...) lx_cfg80211_disconnected(__VA_ARGS__)
#define cfg80211_port_authorized(...) lx_cfg80211_port_authorized(__VA_ARGS__)
/*
 * LinuxKPI's skb_header_cloned() answers "cloned" always, and its pskb_expand_head() fails always: every
 * frame sent was dropped as "unmodifiable headroom". Nothing clones a buffer here (lx_base.c makes them
 * whole, lx_brcm_sta.c gives frames to send 64 bytes in front).
 */
#include <linux/skbuff.h>
#define skb_header_cloned(s)			((void)(s), false)
