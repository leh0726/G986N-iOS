/*
 * LinuxKPI's net/netlink.h includes FreeBSD's netlink header, which the tree tools/mk-ath11k.sh unpacks does
 * not have and which nothing here uses: brcmfmac's vendor commands (vendor.c) only need the few nla_*
 * functions that net/netlink.h itself defines. An empty header stands in for it.
 */
#ifndef LX_NETLINK_NETLINK_H
#define LX_NETLINK_NETLINK_H
#endif
