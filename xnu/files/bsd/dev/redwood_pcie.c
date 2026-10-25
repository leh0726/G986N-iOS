/*
 * The Galaxy S20+'s Wi-Fi, from nothing and step by step, as the POCO X5 Pro's was (redwood_wifi.c). Its
 * radio is not in the SoC: a QCA6390/6490 on the SoC's first PCIe link (dt: qcom,pcie@1c00000, and under it
 * qcom,cnss-qca6490), which ath11k drives through PCIe and MHI where redwood's WCN6750 sat on the SoC's
 * own bus. Its UEFI has no PCIe driver, so nothing has set the link up before us: the controller's power
 * switch and clocks, its PHY (the device tree has the whole sequence, qcom,phy-sequence), the radio's power
 * (wlan_en) and the reset line to the card (PERST) are all ours to do.
 *
 * Boot-arg pcie_step=N performs steps 1 to N and stops, on the y2q only; a step says what it is about to
 * touch before it touches it, and reads come before writes.
 *
 *   1  reads   the global clock controller's side of PCIe0: the clocks' branches and this processor's
 *              votes for them, the power switch, the block resets, the PHY's reference clocks; and the
 *              three pins (wlan_en GPIO20, PERST GPIO79, CLKREQ GPIO80). Nothing of the controller
 *              itself, whose registers have no clock yet.
 *   2  writes  the power switch on (pcie_0_gdsc: its collapse bit cleared) and this processor's votes
 *              for PCIe0's six clocks (GCC 0x152008 bits 0 to 5), then reads them back: the pipe clock
 *              comes from the PHY and stays off until the PHY runs
 *   3  reads   the controller's own registers (PARF, 0x1c00000) and the PHY's status (0x1c06000), now
 *              that their bus clock runs
 *   4  writes  the PHY's clocks on (pcie_phy_aux, the radio's reference clock; refgen runs already), the
 *              PHY through its reset (GCC_PCIE_0_PHY_BCR), then the device tree's qcom,phy-sequence (99
 *              writes, the last two its software reset let go and its start), and waits for the PHY
 *              to say it is ready -- and for the pipe clock, which the PHY makes
 *   5  writes  the controller into a root port (PARF, as Linux's pcie-qcom.c does for this one, 2.7.0),
 *              PERST to the card held, the Broadcom radio powered (WL_REG_ON, GPIO90, high), PERST let go,
 *              then link training on (PARF_LTSSM bit 8), and waits for the link
 *   6  writes  the root port's bus numbers (the card is bus 1) and one window of the address translation
 *              (iATU, dt "iatu" 0x60001000, region 0: configuration requests of type 0 to bus 1 through
 *              dt "conf" 0x60100000), then reads the card's identity, class and first BAR through it
 *   7  writes  the card's two memory BARs their sizes found and addresses given (in dt's memory range,
 *              0x60300000 and up, which the bus sees at the same addresses), a second window of the
 *              translation for memory requests, the root port's window and both sides' memory and bus
 *              master enables; then points BAR0 at the chip's ChipCommon core (config 0x80, as brcmfmac's
 *              BRCMF_PCIE_BAR0_WINDOW) and reads its chip ID
 *   8  reads   the chip's table of its cores (the EROM, from ChipCommon's eromptr), as brcmfmac's
 *              chip.c walks it: each core's id, revision, registers and wrapper on the backplane
 *   9  writes  the firmware into the radio and starts it, in brcmfmac's order (pcie.c,
 *              brcmf_pcie_download_fw_nvram; chip.c for the cores): the ARM CR4 held halted in reset and
 *              the 802.11 core reset, the RAM's size from the CR4's banks, fw/bcm4375_sta.bin at the RAM's
 *              base (0x170000: where 1245 of the image's 1327 strings are pointed at from), the NVRAM
 *              (fw/bcm4375_nvram.bin, tools/brcm-nvram.py) at the RAM's end, the reset vector, the ARM let
 *              go; then waits for the firmware to write the address of its shared area in the RAM's last word.
 *  10  reads   that shared area (brcmfmac's pcie.c: brcmf_pcie_init_share_ram_info) -- its version, the
 *              console's address, the rings' -- and what the firmware has printed in its console
 *  11  reads   the rings the firmware offers (its ring info: brcmfmac's brcmf_pcie_dhi_ringinfo) and the
 *              SMMU its DMA goes through (dt apps-smmu@15000000; PCIe0 is stream 0x1c00): whether
 *              unmatched streams pass or fault, and which entries of its stream table take 0x1c00.
 *              Seen: 40 flow, 43 submission and 5 completion rings offered (their memory and indices in the
 *              radio's RAM); the apps SMMU faults streams nobody takes (cr0 0x2e0406), has 87 entries and
 *              none for PCIe0. So the radio's DMA needs a context of ours there first -- and for the radio
 *              itself the stream is 0x1c01 (dt iommu-map: requester 0x100, bus 1, is 0x1c01), not 0x1c00.
 *              Seen: the fast clock (HT) never comes with the PMU's max_res_mask as the firmware left it
 *              (0xe4fffff); with every resource allowed (0x7fffffff; pcie_maxres=0 leaves it) HT is there.
 *              The firmware still does not answer, nor touch the RAM past its image -- but it runs: it
 *              writes 2484 words of its own image (the cores it found, 0x18002000 and its wrapper, the
 *              RAM's base 0x170000, size 0x190000 and end, a stack near 0x2fdff0) and copies the NVRAM we
 *              gave it into a buffer of its own at 0x285bc0 (0x2660 bytes); seven pages change in all.
 *              Its stack then held 0xfeedc0de and 0x100: brcmfmac's random seed footer, which it was
 *              looking for under the NVRAM. With 256 random bytes and that footer there (pcie_seed=0 leaves
 *              them out) THE FIRMWARE RUNS: the RAM's last word becomes 0x24f8bc, its shared area, and 195
 *              of the RAM's 400 pages change. (pcie_dump=1: the changed words of the stack's pages.)
 *              Step 10: shared area version 7 (flags 0x30040007), 511 rx buffers, ring info 0x2d57a0; the
 *              console says "RTE (SDIO-PCIE-MSGBUF) 18.41.117 (B1 Network/rsdb) on BCM4375 r5 @
 *              37.4/160.0/400.0MHz", both radios attach (wl0 5 GHz, wl1 2.4 GHz), "ThreadX v5.6 initialized".
 *  12  writes  the controller's BDF-to-SID table (PARF 0x2000) as Linux fills it, so that the radio's
 *              requests carry stream 0x1c01; then the apps SMMU: a page of ours, filled with a pattern,
 *              shown to the radio through redwood_wifi_radio_map() (redwood_wifi.c: a free match register
 *              for stream 0x1c01, a free context, page tables of ours, all read back), at the radio's
 *              0xa0000000 and up (dt cnss_pci_iommu_group's pool); a second on, the context's and the
 *              SMMU's global fault status. Put back on the way to the loader.
 *              Seen: the table was full of nonsense (all 256 entries); the hypervisor, which checks
 *              every match register written, took nothing of one for 0x1c00 mask 1 (read back 0).
 *  13  writes  brcmfmac's common rings (pcie.c: brcmf_pcie_init_ringbuffers, alloc_dma_and_ring,
 *              init_scratchbuffers) into 64 KB of ours shown to the radio: control submit, rx post,
 *              control complete, tx complete, rx complete, each one's address, item count and size into
 *              the ring memory the firmware offered; the scratch and ring-update buffers into the shared
 *              area; then "host ready" on the PCIe2 core's second doorbell (this firmware asks for it:
 *              shared flag HOSTRDY_DB1). The indices stay in the radio's RAM (no DMA_INDEX flag).
 *  14  talks   one ioctl through the rings, as brcmfmac's msgbuf.c does: a response buffer posted
 *              (IOCTLRESP_BUF_POST), then WLC_GET_VERSION (IOCTLPTR_REQ) from a request buffer of ours;
 *              whatever the firmware puts on the control complete ring is said, and the answer read from
 *              our response buffer -- the radio reading our memory and writing into it.
 *              Seen: the rings at the radio's 0xa0004000.. read back as written; after "host ready" the
 *              firmware fetches both messages from our control submit ring and writes IOCTLPTR_REQ_ACK
 *              and then IOCTL_CMPLT (status 0, length 4, cmd 1) onto our control complete ring, with the
 *              answer 2 (WLC_IOCTL_VERSION) in our response buffer; the SMMU refuses nothing.
 * Seen: step 2 turns the power switch on (0xf822f000) and five clocks run (the pipe clock waits for
 * the PHY); step 3 reads PARF sys_ctrl 0x2000361c, phy_ctrl 0x900, phy_refclk 0x1019, ltssm 0, and
 * the PHY's status 0x68: not ready, as it has not been set up. Step 4: the PHY says ready (status
 * 0xa6) and the pipe clock runs -- with the reference clock and the PHY's supplies as the firmware left
 * them, nothing asked of RPMh or the PMICs. Step 5: the root port is Qualcomm's (0x010b17cb), and with
 * WL_REG_ON high and PERST let go THE LINK COMES UP: training state 0x11 (L0), PORT_DEBUG1 0x8000010
 * (from a cold radio: a second run without the power taken away first stayed in state 5). Step 6: the
 * card is vendor 0x14e4, device 0x4475 -- Broadcom's BCM4375 -- revision 5, class 0x028000 (a network
 * controller), its BAR0 a 64-bit memory BAR not given an address yet. Step 7: BAR0 wants 64 KB (given
 * 0x60300000), BAR2 4 MB (0x60400000, the chip's RAM); through BAR0 pointed at ChipCommon the chip ID
 * register reads 0x10054375: BCM4375, revision 5, package 0, 16 cores on its backplane. Step 8: the
 * EROM at 0x18111000 lists ChipCommon 0x18000000 (rev 64), two 802.11 cores (0x18001000, 0x18004000,
 * rev 82), ARM CR4 0x18002000 (rev 15; no SOCRAM: the firmware's RAM is the CR4's own TCM), PCIe2
 * 0x18003000 (rev 64), GCI 0x18010000, PMU 0x18012000, and wrappers at 0x181xx000.
 *
 * Not every register of the clock controller may be read from here: the first survey, everything at
 * once, reset the phone without a word (TrustZone's protection of some of them, it seems), and the
 * loader of the next boot wrote over the log. So the reads are in groups, chosen with boot-arg
 * pcie_survey=MASK (default 1: the first alone), and the one that resets the phone is the one to leave out:
 *   1  PCIe0's own block in GCC (0x16b000..0x16b038) and this processor's votes (0x152008, which the
 *      touch screen's driver reads as well)
 *   2  wlan_en (GPIO20); 32 PERST (GPIO79); 64 CLKREQ (GPIO80) -- the three at once reset the phone.
 *      One at a time: GPIO20 resets it (its register is not ours to read, though Linux drives the pin as
 *      the radio's enable), GPIO79 (function 0, pull-down, low) and GPIO80 (pull-up, high) read fine.
 *   4  the PHY's resets (0x16c014, 0x16c01c)
 *   8  the PHY's aux and reference clocks (0x16f004..0x16f02c)
 *  16  the reference clock to the radio (0x18c004)
 * 128  Broadcom's WL_REG_ON (GPIO90, the south tile); 256 its host wake (GPIO150, the west tile).
 *      Both read fine: GPIO90 function 0, pull-down, input, low (the radio off); GPIO150 pull-down, low.
 * Group 1 reads fine: every clock of PCIe0 off and not voted for, its power switch off.
 *
 * Why GPIO20 is not ours, as found since: the hypervisor keeps QUP0's serial engine 7 and the pins
 * GPIO19..23 away from us (hyp.img, near 0x6a1c0: their register pages, 0x0f913000..0x0f917000), for
 * the secure ultrasonic fingerprint sensor on that engine (dt spi@99c000: qcom,qbtspi). And the
 * qca6490 node that named GPIO20 is disabled in this phone's device tree: its Wi-Fi is Broadcom's
 * (dt bcmdhd_wlan: android,bcmdhd_wlan, WL_REG_ON GPIO90, host wake GPIO150), on this same PCIe link.
 *
 * The first guess, ruled out: UEFI's EnvDxe tells TrustZone that the boot loader is done (a
 * "milestone", SIP service 1 command 0x0e, three arguments of 0) from its ExitBootServices handler,
 * and TrustZone takes that as the moment to give the high-level OS its resources. Our loader never
 * exits UEFI, so that has never been said. Boot-arg tz_milestone=1 says it first (and UEFI's own pin
 * driver could not read GPIO20 either: xnuloader's wlan_en=1 reset the phone the same way).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/redwood_input.h>
#include <sys/random.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x90000UL
#define GCC_VOTE                0x52008         /* the application processor's votes: PCIe0's are bits 0 to 5 */
#define GCC_PCIE_0_BCR          0x6b000
#define GCC_PCIE_0_GDSCR        0x6b004
#define GCC_PCIE_0_AUX_CMD_RCGR 0x6b038
#define GCC_PCIE_0_PHY_BCR      0x6c01c
#define GCC_PCIE_0_LINK_DOWN_BCR 0x6c014
#define GCC_PCIE_PHY_AUX_CBCR   0x6f004
#define GCC_PCIE_PHY_REFGEN_CMD_RCGR 0x6f014
#define GCC_PCIE0_PHY_REFGEN_CBCR 0x6f02c
#define GCC_PCIE_WIFI_CLKREF_EN 0x8c004
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SOUTH_PHYS         0x0f500000UL
#define TLMM_WEST_PHYS          0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PIN_WLAN_EN             20
#define PIN_PERST               79
#define PIN_CLKREQ              80
#define PIN_WL_REG_ON           90              /* dt bcmdhd_wlan: wlan-en-gpio */
#define PIN_WL_HOST_WAKE        150             /* dt bcmdhd_wlan: wlan-host-wake-gpio */

static vm_offset_t gcc, tlmm, tlmm_south, tlmm_west;

/*
 * Registers mapped from the kernel's map, not ml_io_map()'s io_submap: that is 8 MB, and with the GPU, the
 * storage and the rest of a whole boot (cpus=8 gpu=1 ...) it has no megabyte left for the steps -- and a
 * mapping that does not fit there is a panic (KMA_NOFAIL), not an error.
 */
static vm_offset_t
map_io(vm_offset_t physical, vm_size_t size)
{
	return ml_io_map_unmappable(physical, size, VM_WIMG_IO);
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static uint32_t survey = 1;

static const struct {
	const char *name;
	uint32_t cbcr;          /* the branch: bit 31 says it is off */
	int vote;               /* its bit in GCC_VOTE, or -1 if it is enabled in the branch itself */
	uint32_t group;         /* of pcie_survey */
} clocks[] = {
	{ "pcie_0_slv_axi", 0x6b014, 0, 1 },
	{ "pcie_0_mstr_axi", 0x6b01c, 1, 1 },
	{ "pcie_0_cfg_ahb", 0x6b024, 2, 1 },
	{ "pcie_0_aux", 0x6b028, 3, 1 },
	{ "pcie_0_pipe", 0x6b02c, 4, 1 },
	{ "pcie_0_slv_q2a_axi", 0x6b010, 5, 1 },
	{ "pcie_phy_aux", GCC_PCIE_PHY_AUX_CBCR, -1, 8 },
	{ "pcie0_phy_refgen", GCC_PCIE0_PHY_REFGEN_CBCR, -1, 8 },
	{ "pcie_wifi_clkref", GCC_PCIE_WIFI_CLKREF_EN, -1, 16 },
};

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_MILESTONE           (SCM_64 | SCM_SIP | 1u << 8 | 0x0e)
#define SCM_THREE_VALUES        3
#define SCM_INTERRUPTED         1

/* One call of TrustZone's, made again for as long as it says it was interrupted (as in redwood_adreno.c). */
static int64_t
scm(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			*answer = x1;
			return (int64_t)x0;
		}
	}
	return -1;
}

static void
say_pin_of(vm_offset_t tile, const char *what, unsigned pin)
{
	uint32_t cfg = rd(tile + 0x1000UL * pin), io = rd(tile + 0x1000UL * pin + 4);

	printf("pcie: GPIO%u (%s): config %#x -- function %u, pull %u, %u mA, %s; in %s, out %s\n", pin, what, cfg,
	    (cfg >> 2) & 0xf, cfg & 3, 2 * (((cfg >> 6) & 7) + 1), (cfg & (1u << 9)) ? "OUTPUT" : "input",
	    (io & 1) ? "high" : "low", (io & 2) ? "high" : "low");
}

static void
say_pin(const char *what, unsigned pin)
{
	say_pin_of(tlmm, what, pin);
}

/* Step 1: what the firmware has left of PCIe0, read only. */
static bool
step_survey(void)
{
	uint32_t votes, gdscr = 0, milestone = 0;
	unsigned on = 0;

	printf("pcie: step 1: read PCIe0's clocks, power switch and resets in the global clock controller, and its "
	    "pins (reads only)\n");
	gcc = map_io(GCC_PHYS, GCC_SIZE);
	tlmm = map_io(TLMM_NORTH_PHYS, TLMM_SIZE);
	PE_parse_boot_argn("pcie_survey", &survey, sizeof(survey));
	if (PE_parse_boot_argn("tz_milestone", &milestone, sizeof(milestone)) && milestone) {
		uint64_t answer = ~0ull;
		int64_t status;

		printf("pcie: about to tell TrustZone that the boot loader is done (smc %#llx, as EnvDxe at ExitBootServices)\n",
		    (unsigned long long)SCM_MILESTONE);
		status = scm(SCM_MILESTONE, SCM_THREE_VALUES, 0, 0, 0, &answer);
		printf("pcie: TrustZone answers %lld (%#llx)\n", (long long)status, (unsigned long long)answer);
	}
	printf("pcie: the groups of reads asked for: %#x\n", survey);
	votes = (survey & 1) ? rd(gcc + GCC_VOTE) : 0;
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		uint32_t value;
		bool running;

		if (!(survey & clocks[i].group)) {
			continue;
		}
		value = rd(gcc + clocks[i].cbcr);
		running = !(value & CBCR_OFF);
		on += running;
		printf("pcie: clock %s: %s (branch %#x)%s\n", clocks[i].name, running ? "on" : "OFF", value,
		    clocks[i].vote < 0 ? "" : (votes & (1u << clocks[i].vote)) ? ", voted for" : ", not voted for");
	}
	if (survey & 1) {
		gdscr = rd(gcc + GCC_PCIE_0_GDSCR);
		printf("pcie: power switch pcie_0_gdsc: %#x (%s); block reset %#x; aux clock source %#x\n", gdscr,
		    (gdscr & GDSC_POWER_ON) ? "ON" : "off", rd(gcc + GCC_PCIE_0_BCR), rd(gcc + GCC_PCIE_0_AUX_CMD_RCGR));
	}
	if (survey & 4) {
		printf("pcie: resets: PHY %#x, link down %#x\n", rd(gcc + GCC_PCIE_0_PHY_BCR), rd(gcc + GCC_PCIE_0_LINK_DOWN_BCR));
	}
	if (survey & 8) {
		printf("pcie: PHY refgen clock source %#x\n", rd(gcc + GCC_PCIE_PHY_REFGEN_CMD_RCGR));
	}
	if (survey & 2) {
		say_pin("wlan_en", PIN_WLAN_EN);
	}
	if (survey & 32) {
		say_pin("PERST", PIN_PERST);
	}
	if (survey & 64) {
		say_pin("CLKREQ", PIN_CLKREQ);
	}
	if (survey & 128) {
		tlmm_south = map_io(TLMM_SOUTH_PHYS, TLMM_SIZE);
		say_pin_of(tlmm_south, "WL_REG_ON", PIN_WL_REG_ON);
	}
	if (survey & 256) {
		tlmm_west = map_io(TLMM_WEST_PHYS, TLMM_SIZE);
		say_pin_of(tlmm_west, "WL_HOST_WAKE", PIN_WL_HOST_WAKE);
	}
	printf("pcie: step 1: %u of the clocks read are running\n", on);
	return true;
}

#define GDSC_COLLAPSE           (1u << 0)
#define PCIE0_VOTES             0x3fu           /* slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a_axi */
#define PARF_PHYS               0x01c00000UL
#define PARF_SIZE               0x3000UL
#define PARF_SYS_CTRL           0x000
#define PARF_PHY_CTRL           0x040
#define PARF_PHY_REFCLK         0x04c
#define PARF_LTSSM              0x1b0
#define PARF_DEVICE_TYPE        0x1000
#define PHY_PHYS                0x01c06000UL
#define PHY_SIZE                0x1000UL
#define PHY_POWER_DOWN          0x840           /* dt qcom,phy-power-down-offset */
#define PHY_STATUS              0x814           /* dt qcom,phy-status-offset; bit 6 (qcom,phy-status-bit): not ready */

static vm_offset_t parf, phy;

static void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* Step 2: PCIe0's power switch and clocks on. */
static bool
step_power(void)
{
	uint32_t gdscr = rd(gcc + GCC_PCIE_0_GDSCR), votes = rd(gcc + GCC_VOTE), running = 0;

	printf("pcie: step 2: about to switch PCIe0's power on: gdscr %#x -> %#x\n", gdscr, gdscr & ~GDSC_COLLAPSE);
	wr(gcc + GCC_PCIE_0_GDSCR, gdscr & ~GDSC_COLLAPSE);
	for (int i = 0; i < 1000 && !(rd(gcc + GCC_PCIE_0_GDSCR) & GDSC_POWER_ON); i++) {
		delay(10);
	}
	gdscr = rd(gcc + GCC_PCIE_0_GDSCR);
	printf("pcie: power switch: %#x (%s)\n", gdscr, (gdscr & GDSC_POWER_ON) ? "ON" : "STILL OFF");
	if (!(gdscr & GDSC_POWER_ON)) {
		return false;
	}
	printf("pcie: about to vote for PCIe0's clocks: votes %#x -> %#x\n", votes, votes | PCIE0_VOTES);
	wr(gcc + GCC_VOTE, votes | PCIE0_VOTES);
	delay(100);
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		uint32_t value;

		if (clocks[i].group != 1) {
			continue;
		}
		value = rd(gcc + clocks[i].cbcr);
		running += !(value & CBCR_OFF);
		printf("pcie: clock %s: %s (branch %#x)\n", clocks[i].name, (value & CBCR_OFF) ? "OFF" : "on", value);
	}
	/* the pipe clock is the PHY's: the other five are what the controller's registers need */
	return (rd(gcc + 0x6b024) & CBCR_OFF) == 0;     /* pcie_0_cfg_ahb: the bus clock of the registers */
}

/* Step 3: the controller's registers and the PHY's status, read only. */
static bool
step_controller(void)
{
	printf("pcie: step 3: read the controller's PARF registers (0x1c00000) and the PHY (0x1c06000): NOT READ BEFORE\n");
	parf = map_io(PARF_PHYS, PARF_SIZE);
	phy = map_io(PHY_PHYS, PHY_SIZE);
	printf("pcie: PARF: sys_ctrl %#x, phy_ctrl %#x, phy_refclk %#x, ltssm %#x, device_type %#x\n",
	    rd(parf + PARF_SYS_CTRL), rd(parf + PARF_PHY_CTRL), rd(parf + PARF_PHY_REFCLK), rd(parf + PARF_LTSSM),
	    rd(parf + PARF_DEVICE_TYPE));
	printf("pcie: PHY: power_down %#x, status %#x (%s)\n", rd(phy + PHY_POWER_DOWN), rd(phy + PHY_STATUS),
	    (rd(phy + PHY_STATUS) & (1u << 6)) ? "not ready" : "ready");
	return true;
}

/* dt qcom,phy-sequence: offset in the PHY, value (every delay in it is 0) */
static const struct {
	uint16_t offset;
	uint8_t value;
} phy_sequence[] = {
	{ 0x840, 0x03 },
	{ 0x94, 0x08 },
	{ 0x154, 0x34 },
	{ 0x16c, 0x08 },
	{ 0x58, 0x0f },
	{ 0xa4, 0x42 },
	{ 0x110, 0x24 },
	{ 0x11c, 0x03 },
	{ 0x118, 0xb4 },
	{ 0x10c, 0x02 },
	{ 0x1bc, 0x11 },
	{ 0xbc, 0x82 },
	{ 0xd4, 0x03 },
	{ 0xd0, 0x55 },
	{ 0xcc, 0x55 },
	{ 0xb0, 0x1a },
	{ 0xac, 0x0a },
	{ 0xc4, 0x68 },
	{ 0xe0, 0x02 },
	{ 0xdc, 0xaa },
	{ 0xd8, 0xab },
	{ 0xb8, 0x34 },
	{ 0xb4, 0x14 },
	{ 0x158, 0x01 },
	{ 0x74, 0x06 },
	{ 0x7c, 0x16 },
	{ 0x84, 0x36 },
	{ 0x78, 0x06 },
	{ 0x80, 0x16 },
	{ 0x88, 0x36 },
	{ 0x1b0, 0x1e },
	{ 0x1ac, 0xca },
	{ 0x1b8, 0x18 },
	{ 0x1b4, 0xa2 },
	{ 0x50, 0x07 },
	{ 0x10, 0x01 },
	{ 0x1c, 0x31 },
	{ 0x20, 0x01 },
	{ 0x24, 0xde },
	{ 0x28, 0x07 },
	{ 0x30, 0x4c },
	{ 0x34, 0x06 },
	{ 0x29c, 0x12 },
	{ 0x284, 0x35 },
	{ 0x23c, 0x11 },
	{ 0x51c, 0x03 },
	{ 0x518, 0x1c },
	{ 0x524, 0x1e },
	{ 0x4e8, 0x00 },
	{ 0x4ec, 0x0e },
	{ 0x4f0, 0x4a },
	{ 0x4f4, 0x0f },
	{ 0x5b4, 0x04 },
	{ 0x434, 0x7f },
	{ 0x444, 0x70 },
	{ 0x510, 0x17 },
	{ 0x4d4, 0x04 },
	{ 0x4d8, 0x07 },
	{ 0x598, 0xd4 },
	{ 0x59c, 0x54 },
	{ 0x5a0, 0xdb },
	{ 0x5a4, 0x3b },
	{ 0x5a8, 0x31 },
	{ 0x584, 0x24 },
	{ 0x588, 0xe4 },
	{ 0x58c, 0xec },
	{ 0x590, 0x3b },
	{ 0x594, 0x36 },
	{ 0x570, 0x3f },
	{ 0x574, 0x3f },
	{ 0x578, 0xff },
	{ 0x57c, 0x7f },
	{ 0x580, 0x14 },
	{ 0x4fc, 0x00 },
	{ 0x4f8, 0xc0 },
	{ 0x460, 0x30 },
	{ 0x464, 0x00 },
	{ 0x5bc, 0x0c },
	{ 0x4dc, 0x1b },
	{ 0x408, 0x0c },
	{ 0x414, 0x03 },
	{ 0x5b8, 0x30 },
	{ 0x9a4, 0x01 },
	{ 0xc90, 0x00 },
	{ 0xc40, 0x01 },
	{ 0xc48, 0x01 },
	{ 0xc50, 0x00 },
	{ 0xcb4, 0x33 },
	{ 0xcbc, 0x00 },
	{ 0xce0, 0x58 },
	{ 0xca4, 0x0f },
	{ 0x48, 0x90 },
	{ 0xc1c, 0xc1 },
	{ 0x988, 0x77 },
	{ 0x998, 0x0b },
	{ 0x8dc, 0x0d },
	{ 0x9ec, 0x12 },
	{ 0x800, 0x00 },
	{ 0x844, 0x03 },
};
#define BCR_ASSERT              (1u << 0)
#define PHY_NOT_READY           (1u << 6)       /* dt qcom,phy-status-bit */

/* Step 4: the PHY set up and started. */
static bool
step_phy(void)
{
	uint32_t status = 0;

	printf("pcie: step 4: about to turn the PHY's aux clock and the radio's reference clock on\n");
	wr(gcc + GCC_PCIE_PHY_AUX_CBCR, rd(gcc + GCC_PCIE_PHY_AUX_CBCR) | 1);
	wr(gcc + GCC_PCIE_WIFI_CLKREF_EN, rd(gcc + GCC_PCIE_WIFI_CLKREF_EN) | 1);
	delay(100);
	printf("pcie: pcie_phy_aux %#x, pcie_wifi_clkref %#x, pcie0_phy_refgen %#x\n", rd(gcc + GCC_PCIE_PHY_AUX_CBCR),
	    rd(gcc + GCC_PCIE_WIFI_CLKREF_EN), rd(gcc + GCC_PCIE0_PHY_REFGEN_CBCR));
	/* a run before this one may have left the controller training a link: start it afresh too */
	printf("pcie: about to put the controller through its reset (GCC_PCIE_0_BCR)\n");
	wr(gcc + GCC_PCIE_0_BCR, BCR_ASSERT);
	delay(1000);
	wr(gcc + GCC_PCIE_0_BCR, 0);
	delay(1000);
	printf("pcie: about to put the PHY through its reset (GCC_PCIE_0_PHY_BCR)\n");
	wr(gcc + GCC_PCIE_0_PHY_BCR, BCR_ASSERT);
	delay(1000);
	wr(gcc + GCC_PCIE_0_PHY_BCR, 0);
	delay(1000);
	printf("pcie: about to write the PHY's %zu-step sequence\n", sizeof(phy_sequence) / sizeof(phy_sequence[0]));
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(phy + phy_sequence[i].offset, phy_sequence[i].value);
	}
	for (int i = 0; i < 100; i++) {
		status = rd(phy + PHY_STATUS);
		if (!(status & PHY_NOT_READY)) {
			break;
		}
		delay(1000);
	}
	printf("pcie: PHY status %#x: %s; pcie_0_pipe %#x (%s)\n", status, (status & PHY_NOT_READY) ? "NOT READY" : "READY",
	    rd(gcc + 0x6b02c), (rd(gcc + 0x6b02c) & CBCR_OFF) ? "off" : "running");
	return !(status & PHY_NOT_READY);
}

#define PARF_PM_CTRL            0x020
#define PARF_DBI_BASE_ADDR      0x168
#define PARF_MHI_CLOCK_RESET_CTRL 0x174
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2 0x1a8
#define DEVICE_TYPE_RC          0x4
#define PHY_TEST_PWR_DOWN       (1u << 0)
#define MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN (1u << 29)
#define BYPASS                  (1u << 4)
#define REQ_NOT_ENTR_L1         (1u << 5)
#define EN                      (1u << 31)
#define LTSSM_EN                (1u << 8)
#define DBI_PHYS                0x60000000UL    /* dt dm_core: the controller's own configuration space */
#define DBI_SIZE                0x1000UL
#define PCIE_PORT_DEBUG0        0x728           /* [5:0] the training state machine's state */
#define PCIE_PORT_DEBUG1        0x72c
#define DEBUG1_LINK_UP          (1u << 4)
#define DEBUG1_IN_TRAINING      (1u << 29)
#define CFG_FUNCTION            (0xfu << 2)
#define CFG_OUTPUT              (1u << 9)
#define IO_OUT                  (1u << 1)

static vm_offset_t dbi;

/* A pin as an output of this level (function 0, the rest of its configuration kept). */
static void
pin_drive(vm_offset_t tile, unsigned pin, bool high)
{
	vm_offset_t cfg = tile + 0x1000UL * pin, io = cfg + 4;

	wr(io, high ? IO_OUT : 0);
	wr(cfg, (rd(cfg) & ~CFG_FUNCTION) | CFG_OUTPUT);
}

/* Step 5: a root port, the radio powered and out of reset, and the link trained. */
static bool
step_link(void)
{
	uint32_t debug0 = 0, debug1 = 0;

	/*
	 * The radio from off: a run before this one leaves it powered and trained, and a link to it then
	 * did not come up again (training state 5 over and over). Linked the first time, from cold.
	 */
	if (tlmm_south == 0) {
		tlmm_south = map_io(TLMM_SOUTH_PHYS, TLMM_SIZE);
	}
	printf("pcie: step 5: about to stop link training and take the radio's power away (PERST and WL_REG_ON low)\n");
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) & ~LTSSM_EN);
	pin_drive(tlmm, PIN_PERST, false);
	pin_drive(tlmm_south, PIN_WL_REG_ON, false);
	delay(100 * 1000);

	printf("pcie: about to make the controller a root port (PARF writes, as pcie-qcom.c's 2.7.0)\n");
	wr(parf + PARF_DEVICE_TYPE, DEVICE_TYPE_RC);
	wr(parf + PARF_PHY_CTRL, rd(parf + PARF_PHY_CTRL) & ~PHY_TEST_PWR_DOWN);
	wr(parf + PARF_DBI_BASE_ADDR, 0);
	wr(parf + PARF_SYS_CTRL, rd(parf + PARF_SYS_CTRL) & ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN);
	wr(parf + PARF_MHI_CLOCK_RESET_CTRL, rd(parf + PARF_MHI_CLOCK_RESET_CTRL) | BYPASS);
	wr(parf + PARF_PM_CTRL, rd(parf + PARF_PM_CTRL) & ~REQ_NOT_ENTR_L1);
	wr(parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2, rd(parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2) | EN);
	printf("pcie: PARF now: device_type %#x, phy_ctrl %#x, sys_ctrl %#x\n", rd(parf + PARF_DEVICE_TYPE),
	    rd(parf + PARF_PHY_CTRL), rd(parf + PARF_SYS_CTRL));

	printf("pcie: about to hold the card in reset (PERST, GPIO%u, low) and power the radio (WL_REG_ON, GPIO%u, high)\n",
	    PIN_PERST, PIN_WL_REG_ON);
	pin_drive(tlmm, PIN_PERST, false);
	pin_drive(tlmm_south, PIN_WL_REG_ON, true);
	delay(200 * 1000);                      /* Broadcom's radios want some 150 ms after WL_REG_ON */
	say_pin_of(tlmm_south, "WL_REG_ON", PIN_WL_REG_ON);
	printf("pcie: about to let the card out of reset (PERST high)\n");
	pin_drive(tlmm, PIN_PERST, true);
	delay(100 * 1000);                      /* PCIe: 100 ms from PERST to the first configuration request */
	say_pin("PERST", PIN_PERST);

	dbi = map_io(DBI_PHYS, DBI_SIZE);
	printf("pcie: the root port's own identity: %#x (Qualcomm is 0x17cb); about to start link training\n", rd(dbi));
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) | LTSSM_EN);
	for (int i = 0; i < 100; i++) {
		debug0 = rd(dbi + PCIE_PORT_DEBUG0);
		debug1 = rd(dbi + PCIE_PORT_DEBUG1);
		if ((debug1 & DEBUG1_LINK_UP) && !(debug1 & DEBUG1_IN_TRAINING)) {
			break;
		}
		delay(10 * 1000);
	}
	printf("pcie: link: debug0 %#x (training state %u), debug1 %#x, parf ltssm %#x: %s\n", debug0, debug0 & 0x3f,
	    debug1, rd(parf + PARF_LTSSM), (debug1 & DEBUG1_LINK_UP) && !(debug1 & DEBUG1_IN_TRAINING) ? "THE LINK IS UP" :
	    "no link");
	return (debug1 & DEBUG1_LINK_UP) && !(debug1 & DEBUG1_IN_TRAINING);
}

#define IATU_PHYS               0x60001000UL    /* dt iatu: the address translation's regions, unrolled */
#define IATU_SIZE               0x1000UL
#define IATU_CTRL1              0x00            /* of region 0 (outbound): the kind of request */
#define IATU_CTRL2              0x04            /* bit 31: on */
#define IATU_LOWER_BASE         0x08
#define IATU_UPPER_BASE         0x0c
#define IATU_LIMIT              0x10
#define IATU_LOWER_TARGET       0x14
#define IATU_UPPER_TARGET       0x18
#define IATU_TYPE_CFG0          0x4
#define IATU_ENABLE             (1u << 31)
#define CONF_PHYS               0x60100000UL    /* dt conf: where configuration requests go out */
#define CONF_SIZE               0x100000UL
#define PCI_PRIMARY_BUS         0x18            /* of the root port: primary, secondary, subordinate */
#define BROADCOM                0x14e4

static vm_offset_t iatu, conf;

/* Step 6: the card's configuration space, through the address translation. */
static bool
step_card(void)
{
	uint32_t id, class, bar0;

	printf("pcie: step 6: about to give the root port its bus numbers (0, 1, 1) and open a window onto bus 1\n");
	wr(dbi + PCI_PRIMARY_BUS, (rd(dbi + PCI_PRIMARY_BUS) & 0xff000000u) | 0x010100u);
	iatu = map_io(IATU_PHYS, IATU_SIZE);
	conf = map_io(CONF_PHYS, 0x1000);       /* (the card's configuration space: the window is bigger, the card 4 KB) */
	wr(iatu + IATU_LOWER_BASE, (uint32_t)CONF_PHYS);
	wr(iatu + IATU_UPPER_BASE, 0);
	wr(iatu + IATU_LIMIT, (uint32_t)(CONF_PHYS + CONF_SIZE - 1));
	wr(iatu + IATU_LOWER_TARGET, 1u << 24);     /* bus 1, device 0, function 0 */
	wr(iatu + IATU_UPPER_TARGET, 0);
	wr(iatu + IATU_CTRL1, IATU_TYPE_CFG0);
	wr(iatu + IATU_CTRL2, IATU_ENABLE);
	for (int i = 0; i < 10 && !(rd(iatu + IATU_CTRL2) & IATU_ENABLE); i++) {
		delay(1000);
	}
	printf("pcie: bus numbers %#x; window: ctrl1 %#x ctrl2 %#x; about to read the card's identity\n",
	    rd(dbi + PCI_PRIMARY_BUS), rd(iatu + IATU_CTRL1), rd(iatu + IATU_CTRL2));
	id = rd(conf + 0x00);
	class = rd(conf + 0x08);
	bar0 = rd(conf + 0x10);
	printf("pcie: the card: vendor %#x, device %#x, class %#x (revision %#x), BAR0 %#x: %s\n", id & 0xffff, id >> 16,
	    class >> 8, class & 0xff, bar0, (id & 0xffff) == BROADCOM ? "A BROADCOM CHIP ANSWERS" : "not Broadcom's");
	return (id & 0xffff) == BROADCOM;
}

#define IATU_REGION(n)          (0x200UL * (n))  /* outbound regions, unrolled */
#define IATU_TYPE_MEM           0x0
#define MEM_PHYS                0x60300000UL    /* dt ranges: memory, 0x60300000..0x63ffffff, the same on the bus */
#define MEM_END                 0x64000000UL
#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      (1u << 1)
#define PCI_COMMAND_MASTER      (1u << 2)
#define PCI_BAR0                0x10
#define PCI_BAR2                0x18
#define PCI_MEMORY_WINDOW       0x20            /* of the root port: base [15:4], limit [31:20] */
#define BCM_BAR0_WINDOW         0x80            /* brcmfmac's BRCMF_PCIE_BAR0_WINDOW: which 4K of the backplane BAR0 shows */
#define BCM_CHIPCOMMON          0x18000000u     /* the backplane's first core: its first word is the chip ID */

static uint64_t bar0_phys, bar2_phys, bar0_size, bar2_size;
static vm_offset_t bar0;

/* The size a 64-bit memory BAR asks for (all ones written, what comes back), and its old value put back. */
static uint64_t
bar_size(unsigned bar)
{
	uint32_t low = rd(conf + bar), high = rd(conf + bar + 4), size_low, size_high;

	wr(conf + bar, 0xffffffffu);
	wr(conf + bar + 4, 0xffffffffu);
	size_low = rd(conf + bar) & ~0xfu;
	size_high = rd(conf + bar + 4);
	wr(conf + bar, low);
	wr(conf + bar + 4, high);
	return ~(((uint64_t)size_high << 32) | size_low) + 1;
}

/* Step 7: the card's memory where we can reach it, and its first register. */
static bool
step_bars(void)
{
	uint64_t next = MEM_PHYS;
	uint32_t chip;

	printf("pcie: step 7: about to size the card's BARs (all ones written and read back)\n");
	bar0_size = bar_size(PCI_BAR0);
	bar2_size = bar_size(PCI_BAR2);
	printf("pcie: BAR0 wants %#llx bytes, BAR2 %#llx\n", bar0_size, bar2_size);
	if (bar0_size == 0 || bar0_size > 0x1000000 || bar2_size == 0 || bar2_size > 0x2000000) {
		printf("pcie: sizes that do not look like a Broadcom radio's\n");
		return false;
	}
	next = (next + bar0_size - 1) & ~(bar0_size - 1);
	bar0_phys = next;
	next += bar0_size;
	next = (next + bar2_size - 1) & ~(bar2_size - 1);
	bar2_phys = next;
	next += bar2_size;
	if (next > MEM_END) {
		printf("pcie: the BARs do not fit the memory range\n");
		return false;
	}
	printf("pcie: about to give BAR0 %#llx and BAR2 %#llx, and open a window for memory requests\n", bar0_phys, bar2_phys);
	wr(conf + PCI_BAR0, (uint32_t)bar0_phys);
	wr(conf + PCI_BAR0 + 4, (uint32_t)(bar0_phys >> 32));
	wr(conf + PCI_BAR2, (uint32_t)bar2_phys);
	wr(conf + PCI_BAR2 + 4, (uint32_t)(bar2_phys >> 32));
	wr(iatu + IATU_REGION(1) + IATU_LOWER_BASE, (uint32_t)MEM_PHYS);
	wr(iatu + IATU_REGION(1) + IATU_UPPER_BASE, 0);
	wr(iatu + IATU_REGION(1) + IATU_LIMIT, (uint32_t)(MEM_END - 1));
	wr(iatu + IATU_REGION(1) + IATU_LOWER_TARGET, (uint32_t)MEM_PHYS);
	wr(iatu + IATU_REGION(1) + IATU_UPPER_TARGET, 0);
	wr(iatu + IATU_REGION(1) + IATU_CTRL1, IATU_TYPE_MEM);
	wr(iatu + IATU_REGION(1) + IATU_CTRL2, IATU_ENABLE);
	wr(dbi + PCI_MEMORY_WINDOW, ((uint32_t)(MEM_END - 1) & 0xfff00000u) | (uint32_t)(MEM_PHYS >> 16));
	wr(dbi + PCI_COMMAND, rd(dbi + PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	wr(conf + PCI_COMMAND, rd(conf + PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	printf("pcie: card BAR0 %#x %#x, BAR2 %#x %#x, command %#x; root port window %#x, command %#x\n",
	    rd(conf + PCI_BAR0), rd(conf + PCI_BAR0 + 4), rd(conf + PCI_BAR2), rd(conf + PCI_BAR2 + 4), rd(conf + PCI_COMMAND),
	    rd(dbi + PCI_MEMORY_WINDOW), rd(dbi + PCI_COMMAND));

	printf("pcie: about to point BAR0 at ChipCommon (%#x) and read the chip ID through it\n", BCM_CHIPCOMMON);
	wr(conf + BCM_BAR0_WINDOW, BCM_CHIPCOMMON);
	bar0 = map_io((vm_offset_t)bar0_phys, (vm_size_t)bar0_size);
	chip = rd(bar0);
	printf("pcie: BAR0 window %#x; chip ID register %#x: chip %#x (BCM%x), revision %u, package %u, %u cores: %s\n",
	    rd(conf + BCM_BAR0_WINDOW), chip, chip & 0xffff, chip & 0xffff, (chip >> 16) & 0xf, (chip >> 20) & 0xf,
	    (chip >> 24) & 0x1f, (chip & 0xffff) == 0x4375 ? "THE BCM4375 ANSWERS FROM ITS BACKPLANE" : "not what was expected");
	return (chip & 0xffff) == 0x4375;
}

/* brcmfmac's chip.c: the EROM's descriptors */
#define CC_EROMPTR              0xfc
#define DMP_DESC_TYPE_MSK       0xfu
#define DMP_DESC_VALID          0x1u
#define DMP_DESC_COMPONENT      0x1u
#define DMP_DESC_MASTER_PORT    0x3u
#define DMP_DESC_ADDRESS        0x5u
#define DMP_DESC_ADDRSIZE_GT32  0x8u
#define DMP_DESC_EOT            0xfu
#define DMP_COMP_PARTNUM(d)     (((d) >> 8) & 0xfffu)
#define DMP_COMP_REVISION(d)    ((d) >> 24)
#define DMP_COMP_NUM_SWRAP(d)   (((d) >> 19) & 0x1fu)
#define DMP_COMP_NUM_MWRAP(d)   (((d) >> 14) & 0x1fu)
#define DMP_SLAVE_TYPE(d)       (((d) >> 6) & 3u)       /* 0 slave, 1 bridge, 2 slave wrapper, 3 master wrapper */
#define DMP_SLAVE_SIZE_TYPE(d)  (((d) >> 4) & 3u)       /* 3: a size descriptor follows */
#define DMP_SLAVE_PORT(d)       (((d) >> 8) & 0xfu)
#define BCM_WINDOW_SIZE         0x1000u

static uint32_t window_at = ~0u;
static uint32_t arm_base, id_arm_wrapper, d11_wrapper;     /* step 8's: the ARM CR4's registers and wrapper, an 802.11's wrapper */

/* A word of the backplane, through BAR0's 4K window (moved when it has to be). */
static uint32_t
backplane_rd(uint32_t address)
{
	if ((address & ~(BCM_WINDOW_SIZE - 1)) != window_at) {
		window_at = address & ~(BCM_WINDOW_SIZE - 1);
		wr(conf + BCM_BAR0_WINDOW, window_at);
	}
	return rd(bar0 + (address & (BCM_WINDOW_SIZE - 1)));
}

static const char *
core_name(uint32_t id)
{
	switch (id) {
	case 0x800: return "ChipCommon";
	case 0x80e: return "SOCRAM";
	case 0x812: return "802.11";
	case 0x827: return "PMU";
	case 0x83c: return "PCIe2";
	case 0x83e: return "ARM CR4";
	case 0x840: return "GCI";
	case 0x135: return "(a bus component)";
	case 0x240: return "default slave";
	default: return "?";
	}
}

/* Step 8: the chip's cores, from its EROM. */
static bool
step_cores(void)
{
	uint32_t at, end, desc, cores = 0, arm = 0, ram = 0, pcie2 = 0;

	window_at = ~0u;
	at = backplane_rd(BCM_CHIPCOMMON + CC_EROMPTR);
	printf("pcie: step 8: the EROM is at %#x on the backplane; its cores:\n", at);
	end = at + 0x1000 * 4;
	while (at < end && cores < 64) {
		uint32_t cia, cib, id, base = 0, wrapper = 0;

		desc = backplane_rd(at);
		at += 4;
		if ((desc & DMP_DESC_TYPE_MSK) == DMP_DESC_EOT) {
			break;
		}
		if (!(desc & DMP_DESC_VALID) || (desc & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT) {
			continue;
		}
		cia = desc;
		cib = backplane_rd(at);
		at += 4;
		id = DMP_COMP_PARTNUM(cia);
		/* its master ports and addresses, up to the next component */
		for (;;) {
			uint32_t d = backplane_rd(at), type = d & DMP_DESC_TYPE_MSK;

			if (type == DMP_DESC_EOT || (d & DMP_DESC_VALID && type == DMP_DESC_COMPONENT)) {
				break;
			}
			at += 4;
			if (type == DMP_DESC_MASTER_PORT) {
				continue;
			}
			if ((type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS) {
				uint32_t size_type = DMP_SLAVE_SIZE_TYPE(d), kind = DMP_SLAVE_TYPE(d);

				if (d & DMP_DESC_ADDRSIZE_GT32) {
					at += 4;                /* the upper half of the address */
				}
				if (size_type == 3) {
					uint32_t size = backplane_rd(at);

					at += 4;
					if (size & DMP_DESC_ADDRSIZE_GT32) {
						at += 4;
					}
				}
				if (kind == 0 && base == 0 && DMP_SLAVE_PORT(d) == 0) {
					base = d & 0xfffff000u;
				} else if ((kind == 2 || kind == 3) && wrapper == 0) {
					wrapper = d & 0xfffff000u;
				}
			}
		}
		cores++;
		printf("pcie:   core %#x %s, revision %u, registers %#x, wrapper %#x (%u master, %u slave wrappers)\n", id,
		    core_name(id), DMP_COMP_REVISION(cib), base, wrapper, DMP_COMP_NUM_MWRAP(cib), DMP_COMP_NUM_SWRAP(cib));
		if (id == 0x812 && d11_wrapper == 0) {
			d11_wrapper = wrapper;
		}
		if (id == 0x83e) {
			arm = base;
			arm_base = base;
			id_arm_wrapper = wrapper;
		} else if (id == 0x80e) {
			ram = base;
		} else if (id == 0x83c) {
			pcie2 = base;
		}
	}
	printf("pcie: %u cores; the ones a firmware download needs: ARM CR4 %#x, SOCRAM %#x, PCIe2 %#x\n", cores, arm,
	    ram, pcie2);
	if (id_arm_wrapper == 0) {
		return false;
	}
	return cores > 0 && arm != 0;
}

/* BCMA's wrapper registers, and the bits brcmfmac's chip.c sets in them */
#define BCMA_IOCTL              0x408
#define BCMA_RESET_CTL          0x800
#define BCMA_IOCTL_CLK          0x1u
#define BCMA_IOCTL_FGC          0x2u
#define BCMA_RESET_CTL_RESET    0x1u
#define ARMCR4_IOCTL_CPUHALT    0x20u
#define D11_IOCTL_PHYCLOCKEN    0x4u
#define D11_IOCTL_PHYRESET      0x8u
#define ARMCR4_CAP              0x04
#define ARMCR4_BANKIDX          0x40
#define ARMCR4_BANKINFO         0x44
#define BCM4375_RAM_BASE        0x170000u
#define BCM_RANDOM_SEED_MAGIC   0xfeedc0deu
#define BCM_RANDOM_SEED_LENGTH  0x100u
#define BCMA_IOST               0x500
#define SAMPLE_WORDS            4096            /* of the RAM between the image and the NVRAM, compared before and after */

/* ChipCommon's and the PMU core's registers (brcmfmac's chipcommon.h; this chip's PMU is a core of its own) */
#define CC_CAPABILITIES         0x004
#define CC_CHIPSTATUS           0x02c
#define CLK_CTL_ST              0x1e0           /* in every core: [17] HT available, [16] ALP available, [4] HT asked for */
#define PMU_CONTROL             0x600
#define PMU_CAPABILITIES        0x604
#define PMU_STATUS              0x608
#define PMU_RES_STATE           0x60c
#define PMU_RES_PENDING         0x610
#define PMU_MIN_RES_MASK        0x618
#define PMU_MAX_RES_MASK        0x61c
#define BCM_PMU                 0x18012000u     /* step 8's PMU core */
#define CCS_ALPAVAIL            (1u << 16)
#define CCS_HTAVAIL             (1u << 17)
#define CCS_FORCEHT             (1u << 1)       /* the fast clock asked for, and kept (bcmdhd's si_clkctl_cc, CLK_FAST) */

/* The chip's clocks and power resources, as they are now. */
static void
bcm_clocks(const char *when)
{
	uint32_t cc = backplane_rd(BCM_CHIPCOMMON + CLK_CTL_ST), arm = backplane_rd(arm_base + CLK_CTL_ST);

	printf("pcie: clocks %s: ChipCommon clk_ctl_st %#x (ALP %s, HT %s), capabilities %#x, chipstatus %#x; ARM CR4 "
	    "clk_ctl_st %#x (HT %s)\n", when, cc, (cc & CCS_ALPAVAIL) ? "yes" : "no", (cc & CCS_HTAVAIL) ? "yes" : "no",
	    backplane_rd(BCM_CHIPCOMMON + CC_CAPABILITIES), backplane_rd(BCM_CHIPCOMMON + CC_CHIPSTATUS), arm,
	    (arm & CCS_HTAVAIL) ? "yes" : "no");
	printf("pcie: PMU %s: control %#x, capabilities %#x, status %#x, resources on %#x, pending %#x, min %#x, max %#x\n",
	    when, backplane_rd(BCM_PMU + PMU_CONTROL), backplane_rd(BCM_PMU + PMU_CAPABILITIES),
	    backplane_rd(BCM_PMU + PMU_STATUS), backplane_rd(BCM_PMU + PMU_RES_STATE), backplane_rd(BCM_PMU + PMU_RES_PENDING),
	    backplane_rd(BCM_PMU + PMU_MIN_RES_MASK), backplane_rd(BCM_PMU + PMU_MAX_RES_MASK));
}

static vm_offset_t bar2;
static uint32_t shared_at;              /* step 9's: where the firmware's shared area is */
static uint32_t ram_sample[SAMPLE_WORDS];
#define RAM_PAGES               (0x190000 / 0x1000)
/* the pages seen to change (step 9's comparison), kept from before the start and dumped after it */
static const uint32_t watched_pages[] = { 0x24c000, 0x24f000, 0x2fd000 };
static uint32_t watched_before[3][0x400];

static void
watched_keep(void)
{
	for (unsigned p = 0; p < 3; p++) {
		for (unsigned i = 0; i < 0x400; i++) {
			watched_before[p][i] = rd(bar2 + watched_pages[p] + 4 * i);
		}
	}
}

/* Every word of those pages that is not what it was: "pcie: w ADDRESS BEFORE NOW" (tools read these lines) */
static void
watched_dump(void)
{
	for (unsigned p = 0; p < 3; p++) {
		unsigned changed = 0;

		for (unsigned i = 0; i < 0x400; i++) {
			uint32_t now = rd(bar2 + watched_pages[p] + 4 * i);

			if (now != watched_before[p][i]) {
				changed++;
				printf("pcie: w %#x %#x %#x\n", watched_pages[p] + 4 * i, watched_before[p][i], now);
			}
		}
		printf("pcie: page %#x: %u words changed\n", watched_pages[p], changed);
	}
}
static uint32_t page_sum[RAM_PAGES];

/* A sum of every 4K page of the RAM from `base` on (FNV-1a over its words). */
static void
page_sums(uint32_t base, uint32_t pages, uint32_t *sums)
{
	for (uint32_t p = 0; p < pages; p++) {
		uint32_t h = 2166136261u;

		for (uint32_t i = 0; i < 0x1000; i += 4) {
			h = (h ^ rd(bar2 + base + 0x1000 * p + i)) * 16777619u;
		}
		sums[p] = h;
	}
}

/* Every run of printable text of 16 or more characters in the RAM between `from` and `to`, up to a few. */
static void
ram_text(uint32_t from, uint32_t to)
{
	char text[200];
	unsigned n = 0, said = 0;
	uint32_t started = from;

	for (uint32_t at = from; at < to && said < 12; at++) {
		uint32_t c = rd(bar2 + (at & ~3u)) >> (8 * (at & 3)) & 0xff;

		if ((c >= 0x20 && c < 0x7f) || c == '\n') {
			if (n == 0) {
				started = at;
			}
			if (n < sizeof(text) - 1) {
				text[n++] = c == '\n' ? '|' : (char)c;
			}
			continue;
		}
		if (n >= 16) {
			text[n] = 0;
			printf("pcie:   text at %#x: \"%s\"\n", started, text);
			said++;
		}
		n = 0;
	}
	if (said == 0) {
		printf("pcie: no text in %#x..%#x\n", from, to);
	}
}

/* What the firmware has changed of its own image: its words against the file's. */
static void
image_changes(const uint8_t *fw, uint64_t fw_size)
{
	unsigned changed = 0, said = 0;

	for (uint64_t i = 0; i + 4 <= fw_size; i += 4) {
		uint32_t file = (uint32_t)fw[i] | (uint32_t)fw[i + 1] << 8 | (uint32_t)fw[i + 2] << 16 | (uint32_t)fw[i + 3] << 24;
		uint32_t now = rd(bar2 + BCM4375_RAM_BASE + i);

		if (now != file) {
			changed++;
			if (said++ < 8) {
				printf("pcie:   the image's word at %#llx (RAM %#llx): %#x, the file's %#x\n", i,
				    (uint64_t)BCM4375_RAM_BASE + i, now, file);
			}
		}
	}
	printf("pcie: %u of %llu words of the image differ from the file\n", changed, fw_size / 4);
}

/* How many words of the sample have changed since it was taken. */
static unsigned
ram_changed(uint32_t from)
{
	unsigned changed = 0;

	for (unsigned i = 0; i < SAMPLE_WORDS; i++) {
		changed += rd(bar2 + from + 4 * i) != ram_sample[i];
	}
	return changed;
}

/*
 * Text the firmware has printed into its console, if it runs: the ring is in its RAM past the image, and
 * its banner begins "RTE (" (the format string itself is in the image, so only past it counts).
 */
static void
ram_console(uint32_t from, uint32_t to)
{
	for (uint32_t at = from; at + 64 < to; at += 4) {
		uint32_t w = rd(bar2 + at);

		if ((w & 0xffffffu) == 0x455452u /* "RTE" */ || w == 0x3a306c77u /* "wl0:" */) {
			char text[161];
			unsigned n = 0;

			for (uint32_t i = 0; i < 160; i++) {
				uint32_t c = rd(bar2 + ((at + i) & ~3u)) >> (8 * ((at + i) & 3)) & 0xff;

				text[n++] = c >= 0x20 && c < 0x7f ? (char)c : c == '\n' ? '|' : c == 0 ? ' ' : '.';
			}
			text[n] = 0;
			printf("pcie: the RAM at %#x: \"%s\"\n", at, text);
			return;
		}
	}
	printf("pcie: no console text found in %#x..%#x\n", from, to);
}


static void
backplane_wr(uint32_t address, uint32_t value)
{
	(void)backplane_rd(address);            /* (the window moved if it has to be) */
	wr(bar0 + (address & (BCM_WINDOW_SIZE - 1)), value);
}

/* brcmf_chip_ai_coredisable and brcmf_chip_ai_resetcore, for the core whose wrapper is at `wrapper` */
static void
core_reset(uint32_t wrapper, uint32_t prereset, uint32_t reset, uint32_t postreset)
{
	if (!(backplane_rd(wrapper + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET)) {
		backplane_wr(wrapper + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
		(void)backplane_rd(wrapper + BCMA_IOCTL);
		backplane_wr(wrapper + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);
		delay(10);
		for (int i = 0; i < 30 && backplane_rd(wrapper + BCMA_RESET_CTL) != BCMA_RESET_CTL_RESET; i++) {
			delay(10);
		}
	}
	backplane_wr(wrapper + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
	(void)backplane_rd(wrapper + BCMA_IOCTL);
	for (int i = 0; i < 50 && (backplane_rd(wrapper + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET); i++) {
		backplane_wr(wrapper + BCMA_RESET_CTL, 0);
		delay(50);
	}
	backplane_wr(wrapper + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK);
	(void)backplane_rd(wrapper + BCMA_IOCTL);
}

/* brcmf_chip_tcm_ramsize: the CR4's banks of tightly coupled memory, added up */
static uint32_t
cr4_ram_size(void)
{
	uint32_t cap = backplane_rd(arm_base + ARMCR4_CAP), banks = (cap & 0xf) + ((cap >> 4) & 0xf), size = 0;

	for (uint32_t i = 0; i < banks; i++) {
		uint32_t info;

		backplane_wr(arm_base + ARMCR4_BANKIDX, i);
		info = backplane_rd(arm_base + ARMCR4_BANKINFO);
		size += ((info & 0x7f) + 1) * ((info & 0x200) ? 1024 : 8192);
	}
	printf("pcie: the CR4's capabilities %#x: %u banks, %#x bytes of RAM\n", cap, banks, size);
	return size;
}

static void
tcm_copy(uint32_t address, const uint8_t *data, uint64_t length)
{
	for (uint64_t i = 0; i + 4 <= length; i += 4) {
		uint32_t word = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 | (uint32_t)data[i + 2] << 16 |
		    (uint32_t)data[i + 3] << 24;

		wr(bar2 + address + i, word);
	}
}

extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* Step 9: the radio's firmware into its RAM, and the radio's processor started on it. */
static bool
step_firmware(void)
{
	const uint8_t *fw, *nvram;
	uint64_t fw_size = 0, nvram_size = 0;
	uint32_t ram_size, last, written, now = 0, reset_vector, heap;

	fw = redwood_image_file("fw", "bcm4375_sta.bin", &fw_size, NULL);
	nvram = redwood_image_file("fw", "bcm4375_nvram.bin", &nvram_size, NULL);
	if (fw == NULL || nvram == NULL || fw_size < 16 || nvram_size < 8) {
		printf("pcie: step 9: no fw/bcm4375_sta.bin or fw/bcm4375_nvram.bin in the small image (../fw-wifi-y2q)\n");
		return false;
	}
	printf("pcie: step 9: firmware %llu bytes (FWID %s), NVRAM %llu bytes; about to halt the ARM CR4 and reset the "
	    "802.11 core\n", fw_size, "01-1b4772fd", nvram_size);
	core_reset(id_arm_wrapper, backplane_rd(id_arm_wrapper + BCMA_IOCTL) & ARMCR4_IOCTL_CPUHALT, ARMCR4_IOCTL_CPUHALT,
	    ARMCR4_IOCTL_CPUHALT);
	core_reset(d11_wrapper, D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN);
	printf("pcie: ARM CR4 ioctl %#x reset %#x; 802.11 ioctl %#x reset %#x\n", backplane_rd(id_arm_wrapper + BCMA_IOCTL),
	    backplane_rd(id_arm_wrapper + BCMA_RESET_CTL), backplane_rd(d11_wrapper + BCMA_IOCTL),
	    backplane_rd(d11_wrapper + BCMA_RESET_CTL));
	ram_size = cr4_ram_size();
	if (ram_size < fw_size + nvram_size || BCM4375_RAM_BASE + ram_size > bar2_size) {
		printf("pcie: the RAM (%#x from %#x) does not take the firmware, or is beyond BAR2 (%#llx)\n", ram_size,
		    BCM4375_RAM_BASE, bar2_size);
		return false;
	}
	bar2 = map_io((vm_offset_t)bar2_phys, (vm_size_t)bar2_size);
	last = BCM4375_RAM_BASE + ram_size - 4;
	printf("pcie: about to write the firmware at %#x and the NVRAM at %#x\n", BCM4375_RAM_BASE,
	    (uint32_t)(BCM4375_RAM_BASE + ram_size - nvram_size));
	tcm_copy(BCM4375_RAM_BASE, fw, fw_size);
	wr(bar2 + last, 0);
	tcm_copy((uint32_t)(BCM4375_RAM_BASE + ram_size - nvram_size), nvram, nvram_size);
	/*
	 * Under the NVRAM, random bytes and a footer saying how many (brcmfmac's random seed, pcie.c: its
	 * length then the magic 0xfeedc0de): the firmware looks for them -- the magic and the length 0x100
	 * are on its stack when it stops without them (pcie_seed=0 leaves them out).
	 */
	{
		uint32_t seed = 1, footer = (uint32_t)(BCM4375_RAM_BASE + ram_size - nvram_size) - 8;
		uint8_t bytes[BCM_RANDOM_SEED_LENGTH];

		PE_parse_boot_argn("pcie_seed", &seed, sizeof(seed));
		if (seed) {
			read_random(bytes, sizeof(bytes));
			wr(bar2 + footer, BCM_RANDOM_SEED_LENGTH);
			wr(bar2 + footer + 4, BCM_RANDOM_SEED_MAGIC);
			tcm_copy(footer - BCM_RANDOM_SEED_LENGTH, bytes, sizeof(bytes));
			printf("pcie: a random seed of %u bytes at %#x, its footer at %#x\n", BCM_RANDOM_SEED_LENGTH,
			    footer - BCM_RANDOM_SEED_LENGTH, footer);
		}
	}
	written = rd(bar2 + last);
	printf("pcie: read back: the firmware's first word %#x (the file's %#x), the RAM's last word %#x (the NVRAM's "
	    "length token)\n", rd(bar2 + BCM4375_RAM_BASE), (uint32_t)fw[0] | (uint32_t)fw[1] << 8 | (uint32_t)fw[2] << 16 |
	    (uint32_t)fw[3] << 24, written);

	reset_vector = (uint32_t)fw[0] | (uint32_t)fw[1] << 8 | (uint32_t)fw[2] << 16 | (uint32_t)fw[3] << 24;
	/* the RAM past the image, as it is before the ARM runs: a running firmware clears and fills it */
	heap = (BCM4375_RAM_BASE + (uint32_t)fw_size + 0xfffu) & ~0xfffu;
	for (unsigned i = 0; i < SAMPLE_WORDS; i++) {
		ram_sample[i] = rd(bar2 + heap + 4 * i);
	}
	bcm_clocks("before the start");
	/*
	 * Every resource the PMU has allowed (its capabilities' [12:8] say how many), so that nothing the
	 * fast clock depends on is kept off by the limit (pcie_maxres=0: left as it is)
	 */
	{
		uint32_t widen = 1, count = (backplane_rd(BCM_PMU + PMU_CAPABILITIES) >> 8) & 0x1f;
		uint32_t mask = count >= 31 ? 0x7fffffffu : (1u << count) - 1;

		PE_parse_boot_argn("pcie_maxres", &widen, sizeof(widen));
		if (widen) {
			printf("pcie: about to allow all %u of the PMU's resources: max_res_mask %#x -> %#x\n", count,
			    backplane_rd(BCM_PMU + PMU_MAX_RES_MASK), mask);
			backplane_wr(BCM_PMU + PMU_MAX_RES_MASK, mask);
			delay(10 * 1000);
			bcm_clocks("with every resource allowed");
		}
	}
	/* the fast clock first, as bcmdhd asks for it before it starts the radio's processor */
	printf("pcie: about to ask ChipCommon for the fast clock (clk_ctl_st |= FORCEHT) and wait for it\n");
	backplane_wr(BCM_CHIPCOMMON + CLK_CTL_ST, backplane_rd(BCM_CHIPCOMMON + CLK_CTL_ST) | CCS_FORCEHT);
	for (int i = 0; i < 100 && !(backplane_rd(BCM_CHIPCOMMON + CLK_CTL_ST) & CCS_HTAVAIL); i++) {
		delay(1000);
	}
	bcm_clocks("with the fast clock asked for");
	page_sums(BCM4375_RAM_BASE, ram_size / 0x1000, page_sum);
	watched_keep();
	printf("pcie: about to write the reset vector and let the ARM CR4 go\n");
	wr(bar2 + 0, reset_vector);             /* brcmf_pcie_buscore_activate */
	core_reset(id_arm_wrapper, ARMCR4_IOCTL_CPUHALT, 0, 0);
	printf("pcie: the ARM CR4 now: ioctl %#x (halt %s), reset %#x, status %#x; TCM word 0 %#x\n",
	    backplane_rd(id_arm_wrapper + BCMA_IOCTL), (backplane_rd(id_arm_wrapper + BCMA_IOCTL) & ARMCR4_IOCTL_CPUHALT) ?
	    "ON" : "off", backplane_rd(id_arm_wrapper + BCMA_RESET_CTL), backplane_rd(id_arm_wrapper + BCMA_IOST),
	    rd(bar2 + 0));
	delay(1000 * 1000);
	printf("pcie: after 1 s, %u of %u words past the image (%#x) have changed\n", ram_changed(heap), SAMPLE_WORDS,
	    heap);
	bcm_clocks("1 s after the start");
	for (int i = 0; i < 500; i++) {
		now = rd(bar2 + last);
		if (now != written) {
			break;
		}
		delay(10 * 1000);
	}
	printf("pcie: the RAM's last word %#x after %s: %s\n", now, now != written ? "the start" : "5 s",
	    now != written ? "THE FIRMWARE RUNS: its shared area is there" : "the firmware has not answered");
	printf("pcie: by now %u of %u words past the image have changed\n", ram_changed(heap), SAMPLE_WORDS);
	{
		static uint32_t after[RAM_PAGES];
		unsigned pages = 0;

		page_sums(BCM4375_RAM_BASE, ram_size / 0x1000, after);
		for (uint32_t p = 0; p < ram_size / 0x1000; p++) {
			if (after[p] != page_sum[p]) {
				if (pages < 12) {
					printf("pcie:   RAM page %#x has changed\n", BCM4375_RAM_BASE + 0x1000 * p);
				}
				pages++;
			}
		}
		printf("pcie: %u of the RAM's %u pages have changed since the start\n", pages, ram_size / 0x1000);
	}
	image_changes(fw, fw_size);
	/* the pages the firmware wrote in its image (seen: 0x285000..0x288fff, a buffer at 0x285bc0 of 0x2660) */
	{
		uint32_t dump = 0;

		if (PE_parse_boot_argn("pcie_dump", &dump, sizeof(dump)) && dump) {
			watched_dump();         /* (the changed words of three pages: the stack that found the seed) */
		}
	}
	ram_console(heap, (uint32_t)(BCM4375_RAM_BASE + ram_size - nvram_size));
	shared_at = now != written ? now : 0;
	return now != written;
}

/* brcmfmac's offsets into the shared area and the console's description */
#define SHARED_FLAGS            0
#define SHARED_CONSOLE_ADDR     20
#define SHARED_MAX_RXBUFPOST    34              /* 16 bits: the upper half of the word at 32 (device memory takes no unaligned reads) */
#define SHARED_RX_DATAOFFSET    36
#define SHARED_HTOD_MB_DATA     40
#define SHARED_DTOH_MB_DATA     44
#define SHARED_RING_INFO        48
#define CONSOLE_BUFADDR         8
#define CONSOLE_BUFSIZE         12
#define CONSOLE_WRITEIDX        16

/* Step 10: the firmware's shared area, and its console. */
static bool
step_shared(void)
{
	uint32_t flags, console, buf, size, writeidx, start;
	char line[161];
	unsigned n = 0, lines = 0;

	if (shared_at == 0 || shared_at < BCM4375_RAM_BASE || shared_at >= BCM4375_RAM_BASE + 0x190000) {
		printf("pcie: step 10: no shared area to read (%#x)\n", shared_at);
		return false;
	}
	flags = rd(bar2 + shared_at + SHARED_FLAGS);
	console = rd(bar2 + shared_at + SHARED_CONSOLE_ADDR);
	printf("pcie: step 10: the shared area at %#x: flags %#x (version %u; brcmfmac takes 5 to 7), console %#x, "
	    "max rx buffers %u, rx data offset %u, host-to-dongle mailbox %#x, dongle-to-host %#x, ring info %#x\n",
	    shared_at, flags, flags & 0xff, console, rd(bar2 + shared_at + (SHARED_MAX_RXBUFPOST & ~3)) >> 16,
	    rd(bar2 + shared_at + SHARED_RX_DATAOFFSET), rd(bar2 + shared_at + SHARED_HTOD_MB_DATA),
	    rd(bar2 + shared_at + SHARED_DTOH_MB_DATA), rd(bar2 + shared_at + SHARED_RING_INFO));
	if (console < BCM4375_RAM_BASE || console >= BCM4375_RAM_BASE + 0x190000) {
		printf("pcie: no console\n");
		return true;
	}
	buf = rd(bar2 + console + CONSOLE_BUFADDR);
	size = rd(bar2 + console + CONSOLE_BUFSIZE);
	writeidx = rd(bar2 + console + CONSOLE_WRITEIDX);
	printf("pcie: the console: buffer %#x of %u bytes, written up to %u\n", buf, size, writeidx);
	if (buf < BCM4375_RAM_BASE || size == 0 || size > 0x10000 || writeidx >= size) {
		return true;
	}
	/* from the oldest byte still there: past the write index if the ring has gone round (a zero says not) */
	start = (rd(bar2 + ((buf + writeidx) & ~3u)) >> (8 * ((buf + writeidx) & 3)) & 0xff) ? writeidx : 0;
	for (uint32_t i = 0; i < size && lines < 60; i++) {
		uint32_t at = buf + (start + i) % size, c;

		if ((start + i) % size == writeidx && i != 0) {
			break;
		}
		c = rd(bar2 + (at & ~3u)) >> (8 * (at & 3)) & 0xff;
		if (c == '\n' || n == sizeof(line) - 1) {
			line[n] = 0;
			if (n) {
				printf("pcie: console: %s\n", line);
				lines++;
			}
			n = 0;
		} else if (c >= 0x20 && c < 0x7f) {
			line[n++] = (char)c;
		}
	}
	if (n) {
		line[n] = 0;
		printf("pcie: console: %s\n", line);
	}
	return true;
}

#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x10000UL       /* the global pages only */
#define SMMU_CR0                0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define CR0_USFCFG              (1u << 10)      /* 1: a stream no entry takes is faulted; 0: it passes */
#define PCIE0_STREAM            0x1c00          /* dt pcie@1c00000: iommu-map, qcom,smmu-sid-base */
#define RINGINFO_MAX_RINGS      52              /* 16 bits each: flow rings, submission rings; then completion */

/* Step 11: what the rings and the DMA path look like, read only. */
static bool
step_dma_survey(void)
{
	uint32_t info = rd(bar2 + shared_at + SHARED_RING_INFO), cr0, idr0, smrs, takers = 0;
	vm_offset_t smmu;

	printf("pcie: step 11: the ring info at %#x: ring memory %#x, write/read indices host-to-dongle %#x %#x, "
	    "dongle-to-host %#x %#x; rings: %u flow, %u submission, %u completion\n", info, rd(bar2 + info),
	    rd(bar2 + info + 4), rd(bar2 + info + 8), rd(bar2 + info + 12), rd(bar2 + info + 16),
	    rd(bar2 + info + RINGINFO_MAX_RINGS) & 0xffff, rd(bar2 + info + RINGINFO_MAX_RINGS) >> 16,
	    rd(bar2 + info + RINGINFO_MAX_RINGS + 4) & 0xffff);

	printf("pcie: about to read the apps SMMU's global registers (0x15000000: NOT READ BEFORE)\n");
	smmu = map_io(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	cr0 = rd(smmu + SMMU_CR0);
	idr0 = rd(smmu + SMMU_IDR0);
	smrs = idr0 & 0xff;
	printf("pcie: apps SMMU: cr0 %#x (a stream nobody takes %s), idr0 %#x (%u stream table entries), idr1 %#x\n", cr0,
	    (cr0 & CR0_USFCFG) ? "FAULTS" : "passes", idr0, smrs, rd(smmu + SMMU_IDR1));
	for (uint32_t i = 0; i < smrs; i++) {
		uint32_t smr = rd(smmu + SMMU_SMR(i)), id = smr & 0xffff, mask = (smr >> 16) & 0x7fff;

		if ((smr & (1u << 31)) && ((id ^ PCIE0_STREAM) & ~mask & 0x7fff) == 0) {
			takers++;
			printf("pcie: entry %u takes stream %#x: smr %#x, s2cr %#x\n", i, PCIE0_STREAM, smr, rd(smmu + SMMU_S2CR(i)));
		}
	}
	printf("pcie: %u entries of %u take PCIe0's stream\n", takers, smrs);
	return true;
}

#define SMMU_SGFSR              0x048           /* what the SMMU as a whole has refused ... */
#define SMMU_SGFSYNR1           0x054           /* ... and of which stream ([15:0]) */
#define PARF_BDF_TO_SID         0x2000          /* 256 words: BDF [31:16], SID offset [15:8], next [7:0] (pcie-qcom.c 1.9.0) */
#define DMA_PATTERN(i)          (0x5a000000u | (uint32_t)(i))

static vm_offset_t dma_page;                    /* step 12's: a page of ours the radio is shown ... */
static unsigned long long dma_address;          /* ... at this address of its own */

/* What the SMMU as a whole has refused since the boot, and of which stream (0x1c00/0x1c01: PCIe0's). */
static void
smmu_global_faults(const char *when)
{
	vm_offset_t smmu = map_io(APPS_SMMU_PHYS, 0x1000);
	uint32_t status = rd(smmu + SMMU_SGFSR), stream = rd(smmu + SMMU_SGFSYNR1) & 0xffff;

	printf("pcie: apps SMMU %s: global fault status %#x, stream %#x%s\n", when, status, stream,
	    status != 0 && (stream & ~1u) == PCIE0_STREAM ? ": PCIe0's" : "");
}

/* The CRC-8 (x^8 + x^2 + x + 1, most significant bit first) of a BDF's two bytes, high one first: its slot in the table. */
static uint8_t
bdf_hash(uint16_t bdf)
{
	uint8_t crc = 0, bytes[2] = { (uint8_t)(bdf >> 8), (uint8_t)bdf };

	for (int b = 0; b < 2; b++) {
		crc ^= bytes[b];
		for (int i = 0; i < 8; i++) {
			crc = (crc & 0x80) ? (uint8_t)(crc << 1 ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

/* dt iommu-map of pcie@1c00000 into the controller's table, as pcie-qcom.c's qcom_pcie_config_sid_1_9_0() does. */
static void
bdf_to_sid_fill(void)
{
	static const uint16_t map[][2] = { { 0x000, 0 }, { 0x100, 1 } };       /* BDF, stream - 0x1c00 */

	for (uint32_t i = 0; i < 256; i++) {
		wr(parf + PARF_BDF_TO_SID + 4 * i, 0);
	}
	for (unsigned m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
		uint8_t hash = bdf_hash(map[m][0]);
		uint32_t value = rd(parf + PARF_BDF_TO_SID + 4 * hash);

		/* a slot taken: chained on to the next free one through the taken one's "next" */
		while (value != 0) {
			uint8_t current = hash++;

			if ((value & 0xff) == 0) {
				wr(parf + PARF_BDF_TO_SID + 4 * current, value | hash);
			}
			value = rd(parf + PARF_BDF_TO_SID + 4 * hash);
		}
		wr(parf + PARF_BDF_TO_SID + 4 * hash, (uint32_t)map[m][0] << 16 | (uint32_t)map[m][1] << 8);
		printf("pcie: BDF %#x -> stream %#x in slot %u: reads %#x\n", map[m][0], PCIE0_STREAM + map[m][1], hash,
		    rd(parf + PARF_BDF_TO_SID + 4 * hash));
	}
}

/*
 * Step 12: a page of ours where the radio can reach it, through a context of the apps SMMU with page
 * tables of ours (redwood_wifi.c's radio_map(), the same as redwood's radio has: match register and
 * context found free, written, read back). First the controller is made to give the radio's requests
 * its stream, 0x1c01: its BDF-to-SID table, found full of nonsense, is filled as Linux fills it.
 * Seen: the hypervisor took nothing of a match register for 0x1c00 mask 1 (read back 0); Android has
 * the radio's 0x1c01 alone, and so does redwood_wifi.c now on this phone.
 * This SMMU does not translate on request (idr0 ATOSNS), so whether the radio really reaches the
 * page is for the radio to show (step 13); here, what the SMMU has refused, before and after.
 */
static bool
step_dma(void)
{
	vm_offset_t cached = 0;
	uint64_t physical;
	unsigned entries = 0;

	printf("pcie: step 12: the controller's BDF-to-SID table (PARF %#x, 256 words), its entries in use:\n", PARF_BDF_TO_SID);
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t entry = rd(parf + PARF_BDF_TO_SID + 4 * i);

		if (entry != 0 && entries++ < 4) {
			printf("pcie:   entry %u: %#x (BDF %#x, stream %#x, next %u)\n", i, entry, entry >> 16,
			    PCIE0_STREAM + (entry >> 8 & 0xff), entry & 0xff);
		}
	}
	/* seen: all 256 in use, with nonsense -- the table's memory as it powers up; nobody has filled it */
	printf("pcie: %u entries in use; about to fill the table as Linux does (pcie-qcom.c, config_sid_1_9_0): emptied, "
	    "then the root port (BDF 0) stream %#x and the radio (BDF 0x100) stream %#x\n", entries, PCIE0_STREAM,
	    PCIE0_STREAM + 1);
	bdf_to_sid_fill();
	smmu_global_faults("before");

	if (kmem_alloc_contig(kernel_map, &cached, PAGE_SIZE, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("pcie: no page\n");
		return false;
	}
	physical = kvtophys(cached);
	for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) {
		((volatile uint32_t *)cached)[i] = DMA_PATTERN(i);
	}
	for (vm_offset_t line = cached; line < cached + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	printf("pcie: a page of ours at %#llx, filled with %#x, %#x, ...; about to show it to the radio "
	    "(redwood_wifi_radio_map: THE SMMU IS WRITTEN)\n", physical, DMA_PATTERN(0), DMA_PATTERN(1));
	if (!redwood_wifi_radio_map(physical, PAGE_SIZE, &dma_address)) {
		printf("pcie: THE RADIO IS NOT SHOWN THE PAGE (the lines of wifi: above say why)\n");
		return false;
	}
	dma_page = ml_io_map_unmappable(physical, PAGE_SIZE, VM_WIMG_WCOMB);
	printf("pcie: THE RADIO IS SHOWN OUR PAGE at its address %#llx; the page still reads %#x %#x\n", dma_address,
	    rd(dma_page), rd(dma_page + 4));
	delay(1000 * 1000);
	printf("pcie: one second on:\n");
	redwood_wifi_radio_faults();
	smmu_global_faults("after");
	return true;
}

/* ---- steps 13 and 14: brcmfmac's message rings -------------------------------------------------- */

#define PCIE2_CORE              0x18003000u     /* step 8's */
#define PCIE2_64_H2D_MAILBOX_0  0xa20           /* the doorbell (PCIe2 core revision 64 and later) */
#define PCIE2_64_H2D_MAILBOX_1  0xa24           /* "host ready" */
#define SHARED_HOSTRDY_DB1      (1u << 28)
#define SHARED_DMA_INDEX        (1u << 16)
#define SHARED_DMA_SCRATCH_LEN  52
#define SHARED_DMA_SCRATCH_ADDR 56
#define SHARED_DMA_RINGUPD_LEN  64
#define SHARED_DMA_RINGUPD_ADDR 68
#define RING_MAX_ITEM           4               /* in a ring's 16 bytes of the firmware's ring memory */
#define RING_LEN_ITEMS          6
#define RING_BASE_ADDR          8
#define RING_MEM_SIZE           16
#define RINGS                   5               /* the common ones: 2 host-to-dongle, then 3 dongle-to-host */
#define MSGBUF_TYPE_IOCTLPTR_REQ        0x9
#define MSGBUF_TYPE_IOCTLPTR_REQ_ACK    0xa
#define MSGBUF_TYPE_IOCTLRESP_BUF_POST  0xb
#define MSGBUF_TYPE_IOCTL_CMPLT         0xc
#define BRCMF_IOCTL_REQ_PKTID   0xfffe
#define WLC_GET_VERSION         1
#define IOCTL_BUF_SIZE          0x2000          /* brcmfmac's BRCMF_MSGBUF_MAX_CTL_PKT_SIZE */

/* where each ring and buffer is in our 64 KB, and its items: brcmfmac's sizes for shared version 7 */
static const struct {
	const char *name;
	uint32_t offset, items, size;
} rings[RINGS] = {
	{ "control submit", 0x0000, 64, 40 },
	{ "rx post", 0x1000, 256, 32 },
	{ "control complete", 0x3000, 64, 24 },
	{ "tx complete", 0x4000, 256, 24 },
	{ "rx complete", 0x6000, 256, 40 },
};
#define SCRATCH_OFFSET          0x9000
#define SCRATCH_SIZE            8
#define RINGUPD_OFFSET          0x9100
#define RINGUPD_SIZE            1024
#define IOCTL_REQ_OFFSET        0xa000
#define IOCTL_RESP_OFFSET       0xc000
#define RINGS_SIZE              0x10000

static vm_offset_t ring_memory;         /* our 64 KB, without the cache */
static unsigned long long ring_address; /* the radio's address of it */
static uint32_t idx_h2d_w, idx_h2d_r, idx_d2h_w, idx_d2h_r;     /* in the radio's RAM: the rings' indices, 4 bytes apart */

static void
mailbox(uint32_t which)
{
	backplane_wr(PCIE2_CORE + which, 1);
}

static uint16_t
tcm16(uint32_t address)
{
	return *(volatile uint16_t *)(bar2 + address);
}

static void
tcm16_wr(uint32_t address, uint16_t value)
{
	*(volatile uint16_t *)(bar2 + address) = value;
}

/* Step 13: the common rings in memory of ours, and the firmware told where they are. */
static bool
step_rings(void)
{
	uint32_t info = rd(bar2 + shared_at + SHARED_RING_INFO), ringmem = rd(bar2 + info), flags;
	vm_offset_t cached = 0;
	uint64_t physical;

	flags = rd(bar2 + shared_at + SHARED_FLAGS);
	idx_h2d_w = rd(bar2 + info + 4);
	idx_h2d_r = rd(bar2 + info + 8);
	idx_d2h_w = rd(bar2 + info + 12);
	idx_d2h_r = rd(bar2 + info + 16);
	printf("pcie: step 13: shared flags %#x (indices %s, host ready on doorbell 1 %s); ring memory %#x\n", flags,
	    (flags & SHARED_DMA_INDEX) ? "BY DMA, which this does not do" : "in the radio's RAM",
	    (flags & SHARED_HOSTRDY_DB1) ? "asked for" : "not asked for", ringmem);
	if (flags & SHARED_DMA_INDEX) {
		return false;
	}
	if (kmem_alloc_contig(kernel_map, &cached, RINGS_SIZE, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("pcie: no 64 KB in one piece\n");
		return false;
	}
	physical = kvtophys(cached);
	for (vm_offset_t line = cached; line < cached + RINGS_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (!redwood_wifi_radio_map(physical, RINGS_SIZE, &ring_address)) {
		printf("pcie: the radio is not shown the rings' memory\n");
		return false;
	}
	ring_memory = ml_io_map_unmappable(physical, RINGS_SIZE, VM_WIMG_WCOMB);
	for (uint32_t i = 0; i < RINGS_SIZE; i += 4) {
		wr(ring_memory + i, 0);
	}
	printf("pcie: 64 KB of ours at %#llx, the radio's %#llx; about to write the rings into its ring memory\n", physical,
	    ring_address);
	for (unsigned i = 0; i < RINGS; i++) {
		uint32_t at = ringmem + RING_MEM_SIZE * i;
		uint64_t address = ring_address + rings[i].offset;

		wr(bar2 + at + RING_BASE_ADDR, (uint32_t)address);
		wr(bar2 + at + RING_BASE_ADDR + 4, (uint32_t)(address >> 32));
		tcm16_wr(at + RING_MAX_ITEM, (uint16_t)rings[i].items);
		tcm16_wr(at + RING_LEN_ITEMS, (uint16_t)rings[i].size);
		printf("pcie:   ring %u (%s): %u items of %u bytes at %#llx; reads back %#x %#x, %u, %u\n", i, rings[i].name,
		    rings[i].items, rings[i].size, address, rd(bar2 + at + RING_BASE_ADDR), rd(bar2 + at + RING_BASE_ADDR + 4),
		    tcm16(at + RING_MAX_ITEM), tcm16(at + RING_LEN_ITEMS));
	}
	wr(bar2 + shared_at + SHARED_DMA_SCRATCH_LEN, SCRATCH_SIZE);
	wr(bar2 + shared_at + SHARED_DMA_SCRATCH_ADDR, (uint32_t)(ring_address + SCRATCH_OFFSET));
	wr(bar2 + shared_at + SHARED_DMA_SCRATCH_ADDR + 4, (uint32_t)((ring_address + SCRATCH_OFFSET) >> 32));
	wr(bar2 + shared_at + SHARED_DMA_RINGUPD_LEN, RINGUPD_SIZE);
	wr(bar2 + shared_at + SHARED_DMA_RINGUPD_ADDR, (uint32_t)(ring_address + RINGUPD_OFFSET));
	wr(bar2 + shared_at + SHARED_DMA_RINGUPD_ADDR + 4, (uint32_t)((ring_address + RINGUPD_OFFSET) >> 32));
	printf("pcie: indices in the radio's RAM: host-to-dongle write %#x read %#x, dongle-to-host write %#x read %#x\n",
	    idx_h2d_w, idx_h2d_r, idx_d2h_w, idx_d2h_r);
	if (flags & SHARED_HOSTRDY_DB1) {
		printf("pcie: about to say \"host ready\" (PCIe2 core %#x)\n", PCIE2_64_H2D_MAILBOX_1);
		mailbox(PCIE2_64_H2D_MAILBOX_1);
	}
	return true;
}

/* The next item of a host-to-dongle ring (0 control submit, 1 rx post): its place in our memory. */
static vm_offset_t
h2d_item(unsigned ring, uint16_t *index)
{
	*index = tcm16(idx_h2d_w + 4 * ring);
	return ring_memory + rings[ring].offset + rings[ring].size * (*index % rings[ring].items);
}

/* ... written: the write index moved on, and the doorbell. */
static void
h2d_send(unsigned ring, uint16_t index)
{
	tcm16_wr(idx_h2d_w + 4 * ring, (uint16_t)((index + 1) % rings[ring].items));
	mailbox(PCIE2_64_H2D_MAILBOX_0);
}

/* Step 14: one ioctl, WLC_GET_VERSION, through the rings. */
static bool
step_ioctl(void)
{
	vm_offset_t item;
	uint16_t index, seen = 0;
	bool answered = false;

	printf("pcie: step 14: about to post a response buffer (IOCTLRESP_BUF_POST, %u bytes at %#llx)\n", IOCTL_BUF_SIZE,
	    ring_address + IOCTL_RESP_OFFSET);
	item = h2d_item(0, &index);
	wr(item + 0, MSGBUF_TYPE_IOCTLRESP_BUF_POST);          /* msgtype, ifidx 0, flags 0 */
	wr(item + 4, 1);                                        /* request_id: the buffer's id */
	wr(item + 8, IOCTL_BUF_SIZE);                           /* host_buf_len */
	wr(item + 12, 0);
	wr(item + 16, (uint32_t)(ring_address + IOCTL_RESP_OFFSET));
	wr(item + 20, (uint32_t)((ring_address + IOCTL_RESP_OFFSET) >> 32));
	h2d_send(0, index);

	printf("pcie: about to send WLC_GET_VERSION (IOCTLPTR_REQ, request buffer at %#llx)\n", ring_address + IOCTL_REQ_OFFSET);
	wr(ring_memory + IOCTL_REQ_OFFSET, 0);
	item = h2d_item(0, &index);
	wr(item + 0, MSGBUF_TYPE_IOCTLPTR_REQ);
	wr(item + 4, BRCMF_IOCTL_REQ_PKTID);
	wr(item + 8, WLC_GET_VERSION);                          /* cmd */
	wr(item + 12, 1 | 4u << 16);                            /* trans_id 1, input_buf_len 4 */
	wr(item + 16, 4);                                       /* output_buf_len 4 */
	wr(item + 20, 0);
	wr(item + 24, (uint32_t)(ring_address + IOCTL_REQ_OFFSET));
	wr(item + 28, (uint32_t)((ring_address + IOCTL_REQ_OFFSET) >> 32));
	wr(item + 32, 0);
	wr(item + 36, 0);
	h2d_send(0, index);

	for (int tries = 0; tries < 200 && !answered; tries++) {
		uint16_t written = tcm16(idx_d2h_w);            /* the control complete ring, the first dongle-to-host one */

		while (seen != written) {
			vm_offset_t c = ring_memory + rings[2].offset + rings[2].size * (seen % rings[2].items);
			uint32_t w0 = rd(c), w2 = rd(c + 8), w3 = rd(c + 12), w4 = rd(c + 16);

			printf("pcie: control complete %u: type %#x, request %#x, status %u, words %#x %#x %#x\n", seen,
			    w0 & 0xff, rd(c + 4), w2 & 0xffff, w2, w3, w4);
			if ((w0 & 0xff) == MSGBUF_TYPE_IOCTL_CMPLT) {
				answered = true;
				printf("pcie: THE FIRMWARE ANSWERS THROUGH OUR MEMORY: response length %u, cmd %u, and our buffer "
				    "says %#x (the ioctl version)\n", w3 & 0xffff, w4, rd(ring_memory + IOCTL_RESP_OFFSET));
			}
			seen = (uint16_t)((seen + 1) % rings[2].items);
			tcm16_wr(idx_d2h_r, seen);
		}
		delay(10 * 1000);
	}
	redwood_wifi_radio_faults();
	if (!answered) {
		printf("pcie: no answer in 2 s; control submit read up to %u, control complete written up to %u\n",
		    tcm16(idx_h2d_r), tcm16(idx_d2h_w));
	}
	return answered;
}

/* ---- for the driver (redwood_lx.c: Linux's brcmfmac, tools/mk-brcmfmac.sh) ------------------------------ */

/*
 * What the driver finds in place before it probes: steps 1 to 8 (the link from a cold radio, the card's
 * BARs and bus mastering, its cores read) and the controller's BDF-to-SID table of step 12, so that the
 * radio's requests carry the stream redwood_wifi_radio_map() sets up. Firmware, rings and everything
 * after are the driver's. Once; false if a step failed (the log says which).
 */
bool
redwood_pcie_radio_up(uint32_t *id, uint32_t *class_revision, uint64_t bar[2], uint64_t bar_size[2])
{
	static bool (*const steps[])(void) = { step_survey, step_power, step_controller, step_phy, step_link, step_card,
		                               step_bars, step_cores };
	static int up = -1;

	if (up < 0) {
		up = 1;
		for (uint32_t step = 1; step <= sizeof(steps) / sizeof(steps[0]); step++) {
			if (!steps[step - 1]()) {
				printf("pcie: step %u: FAILED: no radio for the driver\n", step);
				up = 0;
				break;
			}
			printf("pcie: step %u: done\n", step);
		}
		if (up) {
			bdf_to_sid_fill();
			printf("pcie: the radio is the driver's: BAR0 %#llx (%#llx bytes), BAR2 %#llx (%#llx bytes)\n", bar0_phys,
			    bar0_size, bar2_phys, bar2_size);
		}
	}
	if (!up) {
		return false;
	}
	*id = rd(conf + 0x00);
	*class_revision = rd(conf + 0x08);
	bar[0] = bar0_phys;
	bar_size[0] = bar0_size;
	bar[1] = bar2_phys;
	bar_size[1] = bar2_size;
	return true;
}

uint32_t
redwood_pcie_config_read(uint32_t offset, unsigned width)
{
	if (conf == 0 || offset + width > 0x1000) {
		return ~0u;
	}
	switch (width) {
	case 1:
		return *(volatile uint8_t *)(conf + offset);
	case 2:
		return *(volatile uint16_t *)(conf + offset);
	default:
		return rd(conf + offset);
	}
}

void
redwood_pcie_config_write(uint32_t offset, uint32_t value, unsigned width)
{
	if (conf == 0 || offset + width > 0x1000) {
		return;
	}
	if (offset == BCM_BAR0_WINDOW) {
		window_at = ~0u;                /* (the steps' idea of where BAR0 looks is no longer true) */
	}
	switch (width) {
	case 1:
		*(volatile uint8_t *)(conf + offset) = (uint8_t)value;
		break;
	case 2:
		*(volatile uint16_t *)(conf + offset) = (uint16_t)value;
		break;
	default:
		wr(conf + offset, value);
		break;
	}
}

/*
 * On the way back to the loader (redwood_wifi_stop(), redwood_wifi_quiesce(): one processor, nothing
 * taken or slept on): the link's training off, the card held in reset and the radio's power taken away,
 * as step 5 does first -- a radio left powered does not train its link again.
 */
void
redwood_pcie_radio_down(void)
{
	if (parf == 0 || tlmm == 0 || tlmm_south == 0) {
		return;
	}
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) & ~LTSSM_EN);
	pin_drive(tlmm, PIN_PERST, false);
	pin_drive(tlmm_south, PIN_WL_REG_ON, false);
}

void
redwood_pcie_init(void)
{
	static bool (*const steps[])(void) = { step_survey, step_power, step_controller, step_phy, step_link, step_card,
		                               step_bars, step_cores, step_firmware, step_shared, step_dma_survey, step_dma,
		                               step_rings, step_ioctl };
	uint32_t last = 0;

	if (!redwood_platform_is("y2q") || !PE_parse_boot_argn("pcie_step", &last, sizeof(last)) || last == 0) {
		return;
	}
	for (uint32_t step = 1; step <= last && step <= sizeof(steps) / sizeof(steps[0]); step++) {
		if (!steps[step - 1]()) {
			printf("pcie: step %u: FAILED; the steps after it are not attempted\n", step);
			return;
		}
		printf("pcie: step %u: done\n", step);
	}
}
