/* What the files of wifi/lx/brcm know of one another. */
#ifndef _LX_BRCM_H_
#define _LX_BRCM_H_

struct wiphy;
struct cfg80211_ops;

extern struct wiphy *lx_brcm_wiphy;		/* lx_brcm.c: the driver's one wiphy, and ... */
extern const struct cfg80211_ops *lx_brcm_ops;	/* ... what cfg80211 would call the driver by (scan, connect, add_key) */

int	lx_brcm_station_offer(void);		/* lx_brcm_sta.c: en0 and the kernel's station on the driver; 0, or an errno */
void	lx_brcm_station_withdraw(void);		/* ... taken back, the network left */

#endif
