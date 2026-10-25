/*
 * What Broadcom's brcmfmac asks of a Linux kernel and lx_base.c does not give: a PCI bus with one device on
 * it (the Galaxy S20+'s BCM4375, which the kernel's redwood_pcie.c has brought up), its interrupt, and as
 * much of cfg80211 and the network devices as the driver's setup reaches. And the four names the kernel
 * enters by (lx_brcm_start() and the rest, sys/redwood_lx.h). tools/mk-brcmfmac.sh builds this with
 * brcmfmac; ath11k's object has nothing of it.
 *
 * The PCI device: LinuxKPI's inline functions reach FreeBSD's bus through its method tables (kobj): a
 * configuration read is PCI_READ_CONFIG(device_get_parent(dev), dev, ...), looked up by its descriptor.
 * Here the parent is a table that finds nothing in its cache, and the lookup answers with our functions
 * on the kernel's lx_os_pci_*().
 *
 * The interrupt: polled. A thread of ours calls the driver's handler every millisecond, which reads the
 * PCIe2 core's mailbox status itself and wakes its thread function when something is there -- as the
 * line would, without one.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kobj.h>

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <net/cfg80211.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include "pci_if.h"

#include <lx_os.h>
#include "lx.h"
#include "lx_brcm.h"

int brcmf_core_init(void);			/* (core.c: what the module's start comes to on PCIe) */
extern int brcmf_msg_level;			/* (common.c: which of the driver's messages are said) */
extern int linuxkpi_debug;			/* (lx_base.c: whether they are, through pr_debug()) */

#define BRCM_DEBUG_DEFAULT	0		/* errors only; boot-arg wifi_brcm_debug=MASK for more (debug.h's BRCMF_*_VAL:
						 * 0x100000 the firmware's console, 0x8000 the connection, 0x80000 every ring write) */

/* ---- the PCI device ------------------------------------------------------------------------------ */

static struct {
	unsigned id, class_revision;
	unsigned long long bar[2], bar_size[2];	/* BAR0 and BAR2, as the kernel set them */
	struct pci_dev *pdev;
	struct pci_bus bus;
	struct pci_devinfo info;		/* what device_get_ivars() finds (pci_pme_capable() looks) */
	char bsddev;				/* the FreeBSD device: only its address is used */
} card;

static uint32_t
config_read(device_t dev, device_t child, int reg, int width)
{
	return (lx_os_pci_config_read((unsigned)reg, (unsigned)width));
}

static void
config_write(device_t dev, device_t child, int reg, uint32_t value, int width)
{
	lx_os_pci_config_write((unsigned)reg, value, (unsigned)width);
}

static void
command_bits(uint32_t on, uint32_t off)
{
	uint32_t command = lx_os_pci_config_read(PCIR_COMMAND, 2);

	lx_os_pci_config_write(PCIR_COMMAND, (command | on) & ~off, 2);
}

static int
enable_busmaster(device_t dev, device_t child)
{
	command_bits(PCIM_CMD_BUSMASTEREN, 0);
	return (0);
}

static int
disable_busmaster(device_t dev, device_t child)
{
	command_bits(0, PCIM_CMD_BUSMASTEREN);
	return (0);
}

static int
enable_io(device_t dev, device_t child, int space)
{
	if (space == SYS_RES_MEMORY) {
		command_bits(PCIM_CMD_MEMEN, 0);
	}
	return (0);
}

static int
release_msi(device_t dev, device_t child)
{
	return (0);
}

/* The methods' descriptors (generated in FreeBSD's build from pci_if.m) and what each finds here. */
struct kobjop_desc pci_read_config_desc = { 1, NULL };
struct kobjop_desc pci_write_config_desc = { 2, NULL };
struct kobjop_desc pci_enable_busmaster_desc = { 3, NULL };
struct kobjop_desc pci_disable_busmaster_desc = { 4, NULL };
struct kobjop_desc pci_enable_io_desc = { 5, NULL };
struct kobjop_desc pci_release_msi_desc = { 6, NULL };

static struct kobj_method methods[] = {
	{ &pci_read_config_desc, (kobjop_t)config_read },
	{ &pci_write_config_desc, (kobjop_t)config_write },
	{ &pci_enable_busmaster_desc, (kobjop_t)enable_busmaster },
	{ &pci_disable_busmaster_desc, (kobjop_t)disable_busmaster },
	{ &pci_enable_io_desc, (kobjop_t)enable_io },
	{ &pci_release_msi_desc, (kobjop_t)release_msi },
};
static struct kobj_method nothing;		/* what every cache entry holds: never the method looked for */
static struct kobj_ops parent_ops;
static struct kobj parent = { &parent_ops };

static long
no_method(void)
{
	printf("lx: a PCI method that is not here was asked for: answers ENXIO\n");
	return (ENXIO);
}

static struct kobj_method missing = { NULL, (kobjop_t)no_method };

kobj_method_t *
kobj_lookup_method(kobj_class_t cls, kobj_method_t **cep, kobjop_desc_t desc)
{
	for (unsigned i = 0; i < nitems(methods); i++) {
		if (methods[i].desc == desc) {
			return (&methods[i]);
		}
	}
	return (&missing);
}

device_t
device_get_parent(device_t dev)
{
	return ((device_t)&parent);
}

void *
device_get_ivars(device_t dev)
{
	return (&card.info);
}

unsigned long
pci_resource_start(struct pci_dev *pdev, int bar)
{
	return (bar == 0 ? card.bar[0] : bar == 2 ? card.bar[1] : 0);
}

unsigned long
pci_resource_len(struct pci_dev *pdev, int bar)
{
	return (bar == 0 ? card.bar_size[0] : bar == 2 ? card.bar_size[1] : 0);
}

/* (MSI later: the driver goes on with its "legacy" interrupt, which is polled.) */
int
_lkpi_pci_enable_msi_range(struct pci_dev *pdev, int minvec, int maxvec)
{
	return (-ENOTSUP);
}

/*
 * The driver's registration is its probe: the kernel brings the link up (from a cold radio), and if the
 * card is one the driver's table names, the device is made and handed to it. One driver, one device.
 */
int
linux_pci_register_driver(struct pci_driver *pdrv)
{
	const struct pci_device_id *id;
	struct pci_dev *pdev;
	int error;

	for (unsigned i = 0; i < KOBJ_CACHE_SIZE; i++) {
		parent_ops.cache[i] = &nothing;
	}
	error = lx_os_pci_up(&card.id, &card.class_revision, card.bar, card.bar_size);
	if (error != 0) {
		lx_tell("lx: brcmfmac: no radio on PCIe (%d)\n", error);
		return (-ENODEV);
	}
	for (id = pdrv->id_table; id->vendor != 0; id++) {
		if ((id->vendor == PCI_ANY_ID || id->vendor == (card.id & 0xffff)) &&
		    (id->device == PCI_ANY_ID || id->device == card.id >> 16) &&
		    ((card.class_revision >> 8) & id->class_mask) == (id->class & id->class_mask)) {
			break;
		}
	}
	if (id->vendor == 0) {
		lx_tell("lx: brcmfmac: the card (%04x:%04x) is not one the driver knows\n", card.id & 0xffff, card.id >> 16);
		return (-ENODEV);
	}
	pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
	if (pdev == NULL) {
		return (-ENOMEM);
	}
	card.bus.number = 1;
	card.bus.self = pdev;
	pdev->bus = &card.bus;
	pdev->devfn = 0;
	pdev->vendor = card.id & 0xffff;
	pdev->device = card.id >> 16;
	pdev->revision = card.class_revision & 0xff;
	pdev->class = card.class_revision >> 8;
	pdev->subsystem_vendor = lx_os_pci_config_read(PCIR_SUBVEND_0, 2);
	pdev->subsystem_device = lx_os_pci_config_read(PCIR_SUBDEV_0, 2);
	pdev->irq = pdev->dev.irq = 1;
	pdev->pdrv = pdrv;
	pdev->dev.bsddev = (device_t)&card.bsddev;
	pdev->dev.kobj.name = "0000:01:00.0";
	INIT_LIST_HEAD(&pdev->links);
	card.pdev = pdev;
	lx_tell("lx: brcmfmac: the card %04x:%04x revision %u, BAR0 %#llx, BAR2 %#llx: the driver's probe\n", pdev->vendor,
	    pdev->device, pdev->revision, card.bar[0], card.bar[1]);
	error = pdrv->probe(pdev, id);
	lx_tell("lx: brcmfmac: the probe returns %d\n", error);
	return (error);
}

void
linux_pci_unregister_driver(struct pci_driver *pdrv)
{
	if (card.pdev != NULL && pdrv->remove != NULL) {
		pdrv->remove(card.pdev);
	}
}

/* ---- the interrupt, polled --------------------------------------------------------------------------- */

static struct {
	irq_handler_t handler, thread_fn;
	void *arg;
	volatile bool attached, running;
} poll;

static void
poller(void *argument)
{
	static char tick;

	for (;;) {
		lx_os_lock();
		while (poll.attached && poll.running) {
			lx_os_sleep(&tick, 1000000ull);
			break;
		}
		if (!poll.attached) {
			poll.running = false;
			lx_os_wakeup(&poll.running);
			lx_os_unlock();
			return;
		}
		lx_os_unlock();
		if (poll.handler(1, poll.arg) == IRQ_WAKE_THREAD && poll.thread_fn != NULL) {
			poll.thread_fn(1, poll.arg);
		}
	}
}

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
    const char *name, void *arg)
{
	if (poll.attached) {
		return (-EBUSY);
	}
	poll.handler = handler;
	poll.thread_fn = thread_fn;
	poll.arg = arg;
	poll.attached = poll.running = true;
	if (lx_os_thread("wifi-poll", poller, NULL) != 0) {
		poll.attached = poll.running = false;
		return (-ENOMEM);
	}
	printf("lx: brcmfmac: its interrupt (\"%s\") is looked at every millisecond\n", name);
	return (0);
}

/* Returns when the handlers are not running and will not again (unless from the poller itself). */
void
lkpi_free_irq(unsigned int irq, void *arg)
{
	lx_os_lock();
	poll.attached = false;
	while (poll.running) {
		lx_os_sleep(&poll.running, 5000000ull);
	}
	lx_os_unlock();
}

/* ---- cfg80211 and network devices, as far as the driver's setup goes ----------------------------------- */

int linuxkpi_debug_80211;
struct wiphy *lx_brcm_wiphy;			/* the driver's one, and ... */
const struct cfg80211_ops *lx_brcm_ops;		/* ... what cfg80211 would call it by (lx_brcm_sta.c): LinuxKPI's wiphy has no room for it */
static volatile int registered;			/* the driver has registered its wiphy: it is up (1), or gave up (-1) */

struct wiphy *
linuxkpi_wiphy_new(const struct cfg80211_ops *ops, size_t priv_len)
{
	struct wiphy *wiphy = kzalloc(sizeof(*wiphy) + priv_len, GFP_KERNEL);

	if (wiphy != NULL) {
		lx_brcm_wiphy = wiphy;
		lx_brcm_ops = ops;
	}
	return (wiphy);
}

void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
	kfree(wiphy);
}

int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
	unsigned channels = 0;

	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		if (wiphy->bands[band] != NULL) {
			channels += wiphy->bands[band]->n_channels;
		}
	}
	lx_tell("lx: brcmfmac: THE DRIVER IS UP: its wiphy is registered, %u channels\n", channels);
	registered = 1;
	lx_os_lock();
	lx_os_wakeup(&registered);
	lx_os_unlock();
	return (0);
}

struct net_device *
linuxkpi_alloc_netdev(size_t length, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *ndev = kzalloc(sizeof(*ndev) + length, GFP_KERNEL);

	if (ndev == NULL) {
		return (NULL);
	}
	strlcpy(ndev->name, name, sizeof(ndev->name));
	ndev->name_assign_type = flags;
	if (setup != NULL) {
		setup(ndev);
	}
	return (ndev);
}

void
linuxkpi_free_netdev(struct net_device *ndev)
{
	kfree(ndev);
}

int
linuxkpi_net_ratelimit(void)
{
	return (1);
}

uint32_t
linuxkpi_ieee80211_channel_to_frequency(uint32_t channel, enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return (channel == 14 ? 2484 : channel < 14 ? 2407 + channel * 5 : 0);
	case NL80211_BAND_5GHZ:
		return (channel >= 182 && channel <= 196 ? 4000 + channel * 5 : 5000 + channel * 5);
	case NL80211_BAND_6GHZ:
		return (channel == 2 ? 5935 : 5950 + channel * 5);
	default:
		return (0);
	}
}

uint32_t
linuxkpi_ieee80211_frequency_to_channel(uint32_t frequency, uint32_t flags)
{
	if (frequency == 2484) {
		return (14);
	}
	if (frequency < 2484) {
		return ((frequency - 2407) / 5);
	}
	if (frequency >= 4910 && frequency <= 4980) {
		return ((frequency - 4000) / 5);
	}
	if (frequency < 5950) {
		return (frequency == 5935 ? 2 : (frequency - 5000) / 5);
	}
	return ((frequency - 5950) / 5);
}

struct linuxkpi_ieee80211_channel *
linuxkpi_ieee80211_get_channel(struct wiphy *wiphy, uint32_t frequency)
{
	for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *has = wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels; i++) {
			if (has->channels[i].center_freq == frequency) {
				return (&has->channels[i]);
			}
		}
	}
	return (NULL);
}

/* ---- debug.c's, which is not built: it is debugfs, and there is none ------------------------------------- */

struct brcmf_pub;
struct brcmf_bus;
struct seq_file;

struct dentry *
brcmf_debugfs_get_devdir(struct brcmf_pub *drvr)
{
	return (ERR_PTR(-ENOENT));
}

void
brcmf_debugfs_add_entry(struct brcmf_pub *drvr, const char *fn, int (*read_fn)(struct seq_file *seq, void *data))
{
}

int
brcmf_debug_create_memdump(struct brcmf_bus *bus, const void *data, size_t len)
{
	return (0);
}

/* (what the driver's debugfs files would have been read through: none is ever opened) */
void
seq_puts(struct seq_file *m, const char *str)
{
}

void
lkpi_seq_printf(struct seq_file *m, const char *fmt, ...)
{
}

int
simple_attr_open(struct inode *inode, struct file *filp, int (*get)(void *, uint64_t *), int (*set)(void *, uint64_t),
    const char *fmt)
{
	return (-ENODEV);
}

int
simple_attr_release(struct inode *inode, struct file *filp)
{
	return (0);
}

ssize_t
simple_attr_read(struct file *filp, char *buf, size_t read_size, loff_t *ppos)
{
	return (-ENODEV);
}

ssize_t
simple_attr_write(struct file *filp, const char *buf, size_t write_size, loff_t *ppos)
{
	return (-ENODEV);
}

/* ---- where the kernel enters (sys/redwood_lx.h) ------------------------------------------------------ */

int
lx_brcm_prepare(void)
{
	return (lx_pool_init());
}

/*
 * The driver's module start: its probe brings the radio up, and the rest (firmware, rings, the firmware's
 * first answers, the wiphy) goes on in the background on the thread of its firmware request. This waits
 * for that, twenty seconds at the most.
 */
int
lx_brcm_start(void)
{
	unsigned long long until;
	int error;

	/* boot-arg wifi_brcm_debug=MASK: brcmfmac's debug.h, BRCMF_*_VAL, and LinuxKPI's pr_debug() with them */
	brcmf_msg_level = lx_os_option("wifi_brcm_debug") != 0 ? (int)lx_os_option("wifi_brcm_debug") : BRCM_DEBUG_DEFAULT;
	linuxkpi_debug = lx_os_option("wifi_brcm_debug") != 0;
	if ((error = lx_base_init()) != 0 || (error = lx_pool_init()) != 0) {
		lx_tell("lx: brcmfmac: no thread or no memory for the radio (%d)\n", error);
		return (error);
	}
	error = brcmf_core_init();
	if (error != 0) {
		lx_tell("lx: brcmfmac: the driver's start returns %d\n", error);
		return (-error);
	}
	until = lx_os_nanoseconds() + 20 * 1000000000ull;
	lx_os_lock();
	while (registered == 0 && lx_os_nanoseconds() < until) {
		lx_os_sleep(&registered, 100 * 1000000ull);
	}
	lx_os_unlock();
	lx_memory_say();
	if (registered != 1) {
		lx_tell("lx: brcmfmac: THE DRIVER IS NOT UP after 20 seconds (the lines of lx: above say how far it got)\n");
		return (ETIMEDOUT);
	}
	return (lx_brcm_station_offer());
}

void
lx_brcm_stop(void)
{
	lx_brcm_station_withdraw();
}
