# Galaxy S20+ (y2q) Wi-Fi: status and handoff

The y2q's Wi-Fi is a **Broadcom BCM4375 (B1, PCIe ID 14e4:4475, chip rev 5)** on the SoC's first PCIe link
(PCIe0, `qcom,pcie@1c00000`). It is *not* the QCA6390/6490 that a disabled node in the device tree describes.
Bring-up lives in `xnu-src/.../bsd/dev/redwood_pcie.c` (exported to `xnu/files/bsd/dev/redwood_pcie.c`), as
numbered steps run with the boot-arg `pcie_step=N`. Every step is described at the top of that file, with
what was seen on the phone.

## How to run a step

From the loader's USB mode (volume key during the 2 s autoboot wait):

```
XL_DEVICE=y2q XL_SECS=40 XL_RAMDISK=userland/root.apfs \
  XL_CMDLINE="cpus=1 bootcpu=7 pcie_step=14 pcie_survey=1" ./xl run
grep -a "pcie:" logs/last.log
```

The images must be built with `common/build.sh y2q FLASHBOOT=1 CRYPTEXES=flash` (never `userland/build.sh`
directly: `REST=1` and `GL=1` come from `common/build.sh` and the profile). The firmware comes from
`fw-wifi-y2q/` (pulled from the phone's `/vendor/firmware`; not in the repository) and goes into the small
image as `fw/bcm4375_sta.bin` and `fw/bcm4375_nvram.bin` (`tools/brcm-nvram.py`, choice by `Y2Q_NVRAM`,
default `CS01_semco_b1`). The `launchd` panic at the end of these test runs is the test setup (no iOS root),
not Wi-Fi.

## What works (steps 1 to 14, all on the phone)

| Step | What | Result |
|---|---|---|
| 1 | survey (reads) | PCIe0 fully off; pins: PERST GPIO79, CLKREQ GPIO80, WL_REG_ON GPIO90, host wake GPIO150 |
| 2 | power switch, clock votes | `pcie_0_gdsc` on, 5 of 6 clocks (pipe waits for the PHY) |
| 3 | controller registers (reads) | PARF readable |
| 4 | PHY: clocks, resets, dt `qcom,phy-sequence` | PHY ready, pipe clock runs |
| 5 | root port, radio power, PERST, link training | link up, L0 (from a cold radio: step 5 powers it off first) |
| 6 | iATU config window, read the card | vendor 0x14e4, device 0x4475 |
| 7 | BARs (BAR0 64 KB at 0x60300000, BAR2 4 MB at 0x60400000) | chip ID 0x10054375 |
| 8 | EROM core list (reads) | ARM CR4 0x18002000 (wrapper 0x18102000), PCIe2 0x18003000, PMU 0x18012000 |
| 9 | firmware download and start | firmware runs; shared area address appears in the RAM's last word |
| 10 | shared area and console (reads) | shared version 7; console: `RTE (SDIO-PCIE-MSGBUF) 18.41.117 ... BCM4375 r5`, both radios attached |
| 11 | ring info and apps SMMU (reads) | 40 flow / 43 submission / 5 completion rings; apps SMMU faults unmatched streams, no entry for PCIe0 |
| 12 | BDF-to-SID table, apps SMMU stream 0x1c01 to a context of ours | a page of ours at the radio's 0xa0000000; everything reads back, no faults |
| 13 | brcmfmac's 5 common rings, scratch and ring-update buffers in 64 KB of ours; "host ready" (doorbell 1) | rings at the radio's 0xa0004000.. read back as written |
| 14 | one ioctl through the rings: IOCTLRESP_BUF_POST, then WLC_GET_VERSION | firmware answers IOCTLPTR_REQ_ACK and IOCTL_CMPLT (status 0), version 2 in our buffer: **DMA works both ways** |

Things that were needed and are not obvious (all in step 9):
- RAM base 0x170000 (1245 of the image's 1327 string pointers line up there), size 0x190000 from the CR4 banks.
- PMU `max_res_mask` widened from 0xe4fffff to 0x7fffffff, or HT never becomes available.
- brcmfmac's random seed under the NVRAM (256 random bytes, then length 0x100 and magic 0xfeedc0de): without
  it the firmware stops early (the magic was found on its stack).

Never touch GPIO19..23 or QUP0 serial engine 7 (0x99c000): the hypervisor gives them to the secure
fingerprint sensor, and any access resets the phone (`hyp.img` table near 0x6a1c0).

## The radio's DMA path (step 12)

Everything after step 11 needs the chip to read and write host memory by DMA, through the apps SMMU
(`apps-smmu@15000000`, an Arm MMU-500), which faults every stream without an entry. Step 12 sets that up
with redwood's code: `redwood_wifi_radio_map()` in `bsd/dev/redwood_wifi.c` (declared in
`bsd/sys/redwood_input.h`) now works on the y2q as well. It finds a free match register and context,
fills our own page tables, writes them and reads them back, and gives out the radio's addresses from
0xa0000000 up (dt `cnss_pci_iommu_group`: 256 MB from 0xa0000000, the same pool as redwood's). The driver's
DMA pool (`redwood_lx.c: lx_os_radio_memory()`) can use it unchanged. `redwood_wifi_radio_faults()` writes
what the context has refused to the log. The stream entry and context are put back when the kernel returns
to the loader (`redwood_wifi_stop()` / `redwood_wifi_quiesce()`, for a radio without a DSP).

Seen on the phone (step 12):
- **The controller's BDF-to-SID table (PARF 0x2000) is uninitialised**: all 256 entries hold garbage, so
  which stream the radio's requests carried was unknown. Step 12 fills it as Linux's
  `qcom_pcie_config_sid_1_9_0()` does: it zeroes the table, then writes BDF 0 to stream 0x1c00 and BDF 0x100
  (the radio) to 0x1c01, in CRC-8 slots 0 and 21.
- **The hypervisor checks every stream entry written** (`SMMU Virt SMR: Invalid SID` in `hyp.img`). It
  silently dropped 0x1c00 with mask 1 (read back 0) and accepted **0x1c01 with mask 0** (Android's form).
  So the y2q uses that; boot-args `wifi_sid=` / `wifi_sid_mask=` try others.
- Result: match register 7 holds `0x80001c01`, s2cr 7 = context 6, and context 6 holds our tables
  (sctlr 0x127, ttbr0, tcr, cbar 0x1f300, cba2r 1), all as written. A 16 KB page of ours with a pattern is
  at the radio's 0xa0000000. A second later, context fsr 0x400 (only the format field, no fault) and global
  sgfsr 0.
- The phone is not reset between runs, so the context can still hold the last run's tables when it is
  found. Only its sctlr is put back, which is harmless.

**Shown by step 14: the radio reaches our memory both ways.** It fetched two messages from our ring and
wrote an ioctl answer into our buffer, with no SMMU fault (`logs/012416-run.log`).

## Steps 13 and 14 in the code (for reference)

In `redwood_pcie.c`: `step_rings()` (around line 1588) and `step_ioctl()` (around line 1670).
- 64 KB of ours (`ring_memory`, uncached) is shown to the radio at 0xa0004000 and up. The ioctl request
  buffer is at the radio's 0xa000e000 and the response buffer (8 KB) at 0xa0010000.
- Shared flags are `0x30040007`: version 7, and `0x10000000` = `BRCMF_PCIE_SHARED_HOSTRDY_DB1` (host ready on
  doorbell 1). There is no `DMA_INDEX` (0x10000), so the ring indices stay in the radio's RAM: 16-bit values,
  4 bytes apart. brcmfmac does not interpret `0x40000` or `0x20000000`; they caused no trouble.
- The PCIe2 core is revision 64, so brcmfmac's `brcmf_reginfo_64` applies: H2D mailbox 0 (ring doorbell)
  `0xa20`, mailbox 1 (host ready) `0xa24`, interrupt mask `0xc14`, mailbox interrupt status `0xc30`, mailbox
  mask `0xc34`. D2H doorbell bits in the status are `0x1`..`0x8000`, and `int_fn0` is 0. They are reached
  through BAR0's window on the PCIe2 core at 0x18003000 (`backplane_rd()` / `backplane_wr()`).
- Ring sizes are brcmfmac's (`msgbuf.h`): control submit 64 x 40, rx post 1024 x 32, control complete 64 x 24,
  tx complete 1024 x 24, rx complete 1024 x 40 (the version-7 sizes).

# Remaining work: the driver (handoff)

Everything up to the message protocol is proven on the phone by hand-written steps. What remains is to put
**Linux's brcmfmac** on top of it, running in our kernel through the LinuxKPI layer the way redwood's ath11k
runs, until the phone scans, joins a network and passes traffic. The tasks are below in order, each with
a test that shows it is done.

## Progress (2026-10-25)

- **Task 1 is done:** `BRCMFMAC_STRICT=1 tools/mk-brcmfmac.sh` gives `out/brcmfmac.o`, and `tools/mk-ath11k.sh`
  still gives `out/ath11k.o`. The C and kernel basics were added to `wifi/lx/src/lx_base.c`, which both
  scripts already built. brcmfmac's own parts are in `wifi/lx/brcm/lx_brcm.c`, which ath11k does not build:
  PCI through FreeBSD's kobj lookup, the polled interrupt, wiphy and netdev, debug.c's debugfs functions,
  and the entry points. The vendor tables come from building `wcc/`, `cyw/` and `bca/core.c`. The object
  exports `lx_brcm_start/stop/transmit/prepare`, so both objects link into the one kernel (`xnu-build.sh`),
  and `redwood_lx.c` chooses brcmfmac on the y2q.
- **Task 2 is done on the phone** (`logs/092053-run.log`). brcmfmac downloads the firmware itself; the log shows
  "PCIe protocol version 7" and the console's `RTE (SDIO-PCIE-MSGBUF) 18.41.117 ... on BCM4375 r5` banner.
  The patches are in `wifi/lx/brcm/patches/`, not `wifi/lx/patches/` (`mk-ath11k.sh` applies everything there
  to the FreeBSD tree); `mk-brcmfmac.sh` applies them to `src/` and unpacks again when they change. They add
  chip 0x4375, PCIe 0x4475 (WCC_SEED), RAM base 0x170000, firmware `brcmfmac4375b1-pcie`, and before the ARM
  starts, max_res_mask from the PMU's capabilities plus FORCEHT, as step 9 does. The kernel maps
  `brcmfmac4375b1-pcie.{bin,txt,clm_blob}` to `fw/bcm4375_sta.bin`, `bcm4375_nvram.txt` and `bcm4375_clm.blob`
  (`userland/build.sh` ships the last two; there is no txcap blob, which the driver does without).
  The driver is built with `DEBUG` and `CONFIG_BRCMDBG`. Boot-arg `wifi_brcm_debug=MASK` sets brcmfmac's
  message mask (debug.h `BRCMF_*_VAL`; LinuxKPI's `pr_debug()` too). The default is errors only;
  0x100004 (console plus INFO) is the readable one for a run, 0x80000 logs every ring write.
- **Task 3 is done on the phone** (`logs/092120-run.log`). The probe brings the link up (steps 1 to 8 and the
  BDF table, `redwood_pcie_radio_up()`). The driver downloads the firmware, sets up its rings (TCM indices,
  40 flow rings) and finishes its first ioctls: firmware `18.41.117 (B1 Network/rsdb)`, CLM `9.9.5_Y2QDCM`,
  capabilities, features (P2P, GSCAN, PNO, RSDB, MFP, SCAN_V2 ...). Then it registers its wiphy, 43 channels,
  3.4 s after the start. After that the SMMU has refused nothing (fsr 0x400, sgfsr 0). The interrupt is
  polled every millisecond. `redwood_pcie_radio_down()` (PERST and WL_REG_ON low) runs from
  `redwood_wifi_stop()` / `redwood_wifi_quiesce()` before the stream is put back. `lx_base.c`'s pool got 64
  slots of 16 KB, because brcmfmac's ioctl and event buffers are 8 KB.
- Fixed in `lx_base.c` on the way, for both drivers: a missing firmware file now sets `*fw = NULL`, as Linux
  does (brcmfmac released the CLM blob twice); new socket buffers have `next = NULL`; `%pV` in messages.
  For brcmfmac only (`lx_brcmfmac.h`): `memcpy_toio`/`memcpy_fromio`/`memset_io` go in aligned words,
  because LinuxKPI's are `memcpy`, and its wide unaligned stores fault on the radio's RAM.
- **A full boot (`AUTOBOOT_ARGS`: 8 cores, GPU, iOS root) panicked in step 6** and the autoboot restarted it over
  and over: `ml_io_map()` takes from `io_submap` (8 MB, `KMA_NOFAIL`), which the GPU and the rest have used
  up by then, and step 6 mapped the card's 1 MB configuration window. `redwood_pcie.c` now maps everything
  through `ml_io_map_unmappable()` (the kernel's map), and the window 4 KB. With that (`logs/095041-run.log`)
  iOS's wifid scans and Settings lists the networks.
- **Joining from Settings: associated, then sent away.** ASSOCIATE reaches `sta_join()`; the firmware says the
  connection is made (status 0), then the access point deauthenticates (reason 2) and the handshake never
  starts: nothing arrived on the rx complete ring (ring 4) in between, so message 1 never reached
  `lx_netif_rx()`. Not known yet whether the firmware does not hand over data frames at all (the receive path
  is untested: nothing was ever received before) or drops the key messages itself. The join's lines say ms
  since the join began and how many frames were received.
- **Why the join fails, as far as known (2026-10-25, five attempts from Settings, 2.4 and 5 GHz alike):** the
  firmware associates (+0.5 s), receives the access point's message 1 four times (its counters: `rxframe` +4,
  `rxbyte` +452, 113 bytes each), hands none of them to the host, and the access point gives up at +4.5 s
  (deauthentication, reason 2). Host side checked: all 511 receive buffers posted and taken by the firmware
  (rx post read index 511), rx complete write index stays 0, the SMMU refuses nothing; ioctl answers come back
  through the same buffer path. Tried without effect: the firmware's own handshake with the PMK (no PSK_SUP
  event, no frame sent; that experiment has been taken out again), every firmware event enabled (no
  EAPOL_MSG), telling the firmware the host is in D0 (`bcm4375-d0.patch`, kept: it is what bcmdhd does). Next: compare what Android's
  bcmdhd does at start that brcmfmac does not (host capabilities in the shared area, D2H sync mode for the
  shared flag 0x40000, rx-related iovars), from Samsung's kernel source for this phone.
- **Fixed (2026-10-25): the firmware dropped every data frame** because brcmfmac writes its legacy "ring
  update buffer" at shared +64/+68, where this firmware's shared area (bcmdhd's `pciedev_shared_t`, version 6
  and later) has `host_scb_addr` (+64), `host_scb_size` (+72), `flags2` (+80, here 0x40801001: HSCB, EDL,
  extended trap data, D11 tx status) and `host_cap` (+84), which brcmfmac never writes.
  `bcm4375-hscb.patch`: on the BCM4375 the ring update fields are left alone, host SCB memory is given if the
  firmware asks for it (it asks for 0 bytes), and `host_cap` is written as bcmdhd writes it (0x801407:
  version 7, host ready, no OOB device wake, no trap on UR). With that the key messages arrive.
- **Fixed: nothing was ever sent.** LinuxKPI's `skb_header_cloned()` answers "cloned" always and its
  `pskb_expand_head()` fails always, so brcmfmac dropped every frame as "unmodifiable headroom";
  `lx_brcmfmac.h` makes it false (nothing clones a buffer here).
- **Fixed: dropped after 45 s.** The access point begins the 4-way handshake again about 45 s in; nobody
  answered key messages after the join. `lx_brcm_sta.c` keeps the handshake's state for the connection and a
  thread ("wifi-keys") answers key messages while joined (new pairwise key, group key).
- **Seen working (logs/104320-run.log.live, joined from Settings):** handshake in ~0.6 s, DHCP (192.168.86.61),
  DNS 39/39 answered, every TCP connection answered, HTTP and TLS data both ways (example.com from Safari over
  HTTP; 42 KB from an Apple server over TLS).
- **Open (2026-10-25): Safari stays blank and the status bar's Wi-Fi bars are empty**, although the network
  carries Safari's request (example.com's ~1.8 KB page came back over HTTP). wifid asks `CURRENT_NETWORK`
  hundreds of times and is answered only while joined; `RSSI` is polled rarely (4 times a run), so the bars
  likely come from events: `redwood_apple80211.c` posts LINK_CHANGED (with the join's RSSI) only when the link
  changes, and never RSSI/link-quality updates. Next: compare with redwood (where Safari and the bars work),
  find which request or event wifid needs to count the network as usable (refused today: RATE, NOISE,
  PHY_MODE, MCS, 383, 422, ...), and check that iOS makes en0 the primary interface (NECP/nw_path).
  First suspect, being tried: en0 is made only when brcmfmac is up (~4 s in), after iOS has started, while
  iOS looks for the interface once as it starts (see `redwood_lx_net_early()`). `xl` now gives the kernel
  `wifi_mac=` from `fw-wifi-$XL_DEVICE/mac` (not in the repository; the y2q's is the address its firmware
  reports) for runs and for the autoboot image, so en0 is made as the kernel starts.
- **Apps do have network (logs/141830-run.log, the kernel's per-connection summaries):** MobileSafari (7 KB
  back), Weather (7.9 KB), Safari's services and WebKit's Networking process (example.com: request sent,
  579 bytes back) all connect and get answers, although `CaptiveNetwork` stays at `Stage = Evaluate`. So the
  blank Safari page is after the network (drawing: WebContent/GPU on the Adreno 650?), Weather may need
  Apple's authenticated service, and the empty bars come from link-quality events that are not sent.
  `redwood_apple80211.c` now answers NOISE, RATE, PHY_MODE, MCS, POWERSAVE, INT_MIT, PROTMODE while joined
  and keeps COUNTRY_CODE (wifid retried "KR" every 30 s); still refused: set 383 (4104 bytes), get 423, 96,
  97, set 422.
- **Why apps get nothing (logs/145354-run.log.live*, `net_trace` with times and TCP details):** the Weather app's
  connections to 23.220.68.x:443 each send the TLS ClientHello (517 bytes), receive the server's whole
  handshake with its certificates (5351 bytes, in order, acknowledged within a millisecond), and then send
  nothing more until the server closes 20 s later; the app tries the next address, and so on. So the client's
  TLS stalls after the certificates arrive: the trust evaluation in the app's process does not complete. The
  network has delivered everything, and the same fetches from `net_test` (root: the system's trustd) succeed.
  Not Wi-Fi: the mobile user's trustd (user/501), on the flash's iOS volume with `/var` kept (`var_store=1`),
  where redwood's working Safari ran on the image's own root. With `ALLOW="keybagd"` (mkjobs.py's DENY) the
  keybag daemon's job is in the image but is never started: nothing asks for it, so it is not what apps wait
  on. Next: the same run on the image's own root (`root.apfs`, without `ufs=2 root_apfs=ufs_darwin
  var_store=1 ufs_write=1`), to tell the flash volume's state from the y2q itself.
- **Taken out again after the investigation (2026-10-25):** the diagnostics used to find all this -- the
  rings' indices and SMMU refusals said around a join (`lx_os_radio_say()`, `redwood_pcie_radio_say()`), the
  firmware's `counters`, every firmware event asked for, the TCP details in `net_trace`, `redwood_net.c`'s
  and `redwood_apple80211.c`'s lines through `redwood_wifi_say` -- and the firmware-supplicant experiment
  (`wifi_fw_sup`, `bcm4375-fwsup.patch`). In a whole boot the kernel's `printf()` no longer reaches the log,
  so a diagnostic for one has to say its lines with `redwood_wifi_tell()`; the test job that showed configd's
  store and the fetches from an application's side was `rc.jobs` plus `net_test wifi waitaddr en0 300 store
  resolve example.com tcp example.com 80 reach example.com fetch https://example.com/`.
- **Started by `wifi=1` on the y2q** (`brcm_init()` in `redwood_wifi.c`, on a thread of its own). With
  `pcie_step=N` the steps run instead. Test runs, which end in the `launchd` panic, need `wifi_wait=1`: with
  `cpus=1` the panic stops the driver otherwise. Run with
  `XL_SECS=90 XL_CMDLINE="cpus=1 bootcpu=7 wifi=1 wifi_wait=1 wifi_brcm_debug=0x100004"`, then
  `grep -a "pcie:\|wifi:\|lx:\|brcmf\|CONSOLE" logs/last.log`.
- **Task 4: scanning works on the phone, joining is written but not tried** (`logs/093354-run.log`). After the
  driver comes up, `lx_brcm_station_offer()` (`wifi/lx/brcm/lx_brcm_sta.c`) brings its interface up
  (`ndo_open`), attaches en0 with the radio's address and offers the kernel's station (`redwood_sta_ops`).
  `brcm_thread()` then asks for one scan (40 access points heard in 3.1 s) and joins `fw/network` with
  `join_given()`. `userland/build.sh` takes that file from `../fw-wifi-y2q/network` (name, then passphrase).
  - What brcmfmac reports, which LinuxKPI has as empty "TODO" inlines (`register_netdev`, `netif_rx`,
    `eth_type_trans`, carrier and queue state, `eth_hw_addr_set`, `cfg80211_inform_bss*`, `scan_done`,
    `connect_done`, `disconnected`, `port_authorized`), is renamed in `lx_brcmfmac.h` to functions in
    `lx_brcm_sta.c`. `eth_type_trans` matters beyond the data path: brcmfmac recognises firmware events by
    `skb->protocol`.
  - cfg80211's calls into the driver (scan, connect, add_key, disconnect) go through `lx_brcm_ops`, because
    LinuxKPI's `struct wiphy` has no room for the ops.
  - WPA2: the firmware offers no supplicant to brcmfmac (no `sup_wpa`, so no FWSUP). `connect` is given our
    RSN element (PSK, CCMP pairwise, the AP's group cipher) as `sme->ie`, which brcmfmac sets as the
    firmware's "wpaie". The 4-way handshake is `lx_wpa.c`'s, run on the station's thread: key messages are
    taken out of the receive path, answers go out through `ndo_start_xmit`, and the keys go to the driver
    with `add_key`. Only then are en0's link and the station "joined".
  - Frames: received ones go to `lx_os_net_input()`; `lx_brcm_transmit()` copies a frame into a buffer of the
    radio's pool and calls `ndo_start_xmit`.
- `mk-ath11k.sh` (like `mk-brcmfmac.sh` before this) never recompiles when only its own flags change: it
  passes a relative `$0` to `find` after `cd`. `mk-brcmfmac.sh` now uses the absolute path.

## Ground rules (read first)

- **Build:** `common/build.sh y2q FLASHBOOT=1 CRYPTEXES=flash` (it builds the kernel too). The kernel build
  always ends with `IOKit.symbolset` / `Private.symbolset` errors. That is expected: judge by
  `build: kernel N bytes` and by no `error:` in `out/kernel-build.log` for the files you changed.
- **Run:** the phone must be in the loader's USB mode. Check with `fastboot getvar product`, which should
  print `xnuloader`; if not, ask the user to press the volume key during the 2 s autoboot wait. Run with the
  command in "How to run a step" above, then `grep -a "pcie:\|wifi:\|lx:" logs/last.log`. A run takes about
  2 minutes, and the `launchd` panic at the end is expected.
- **Save:** run `xnu/export.sh` after changing anything in `xnu-src/` (it updates `xnu/redwood.patch` and
  `xnu/files/`). Driver patches go in `wifi/lx/patches/` the way `tools/mk-ath11k.sh` applies its own.
- **Never touch GPIO19..23 or QUP0 serial engine 7 (0x99c000):** the phone resets.
- **The phone is not reset between runs.** Step 5 powers the radio off first, because a warm radio does not
  train the link again. Keep that in any new entry point.
- **SMMU:** only `redwood_wifi_radio_map()` may set up the radio's stream. The hypervisor accepts only stream
  0x1c01 with mask 0, and only after the BDF-to-SID table is filled (`bdf_to_sid_fill()`, step 12). The SMMU
  cannot translate on request (no ATOS). Radio addresses come from a 256 MB pool at 0xa0000000, in 4 KB
  units, and are never given back. The kernel's `PAGE_SIZE` is 16 KB.
- **Another session may be editing the same files.** Check the modification times of
  `redwood_pcie.c` and this document before you start, and read them again before writing.

## Task 1: make brcmfmac link

`tools/mk-brcmfmac.sh` builds the PCIe and msgbuf part of brcmfmac (Linux 7.0.14). All 22 files compile,
but the object does not link: `xnu-deps/build/brcmfmac/out/missing.txt` lists 52 names nothing defines, and
`out/stubs.txt` lists 3.

The missing names fall into these groups:
- **C and kernel basics:** `malloc`, `M_KMALLOC`, `DELAY`, `pause_sbt`, `nanouptime`, `panic`, `kasprintf`,
  `lkpi_devm_kasprintf`, `strsep`, `strnlen`, `strnstr`, `strchrnul`, `strtoul`, `sx_sysinit`, `sx_destroy`,
  `system_wq`, `linux_waitqueue_active`, `timer_shutdown_sync`, `sched_relinquish`, `cold`, `linux_copyin`,
  `device_printf`, `device_get_ivars`, `device_get_parent`, `kobj_lookup_method`, `__this_linker_file`,
  `debugfs_create_file`.
  ath11k's object does not lack these, because `wifi/lx/src/lx_base.c` (and parts of `lx_80211.c`) define
  them on top of the kernel's `lx_os_*` functions (`bsd/dev/redwood_lx.c`). mk-brcmfmac.sh does not build
  `wifi/lx/src`. Split what is driver-independent out of `lx_base.c` (and `lx_80211.c`) into a shared file
  that both scripts build, and keep the ath11k-only parts (`lx_bus.c` is ath11k's bus) out of brcmfmac.
  Do not break ath11k: `tools/mk-ath11k.sh` must still produce `out/ath11k.o`.
- **PCI:** `linux_pci_register_driver`, `linux_pci_unregister_driver`, `pci_resource_start`, `pci_resource_len`,
  `pci_read_config_desc`, `pci_write_config_desc`, `pci_enable_busmaster_desc`, `pci_disable_busmaster_desc`,
  `pci_enable_io_desc`, `_lkpi_pci_enable_msi_range`, `pci_release_msi_desc`, `lkpi_request_irq`,
  `lkpi_free_irq`. These belong to task 3.
- **cfg80211 and netdev:** `linuxkpi_wiphy_new`, `linuxkpi_wiphy_free`, `linuxkpi_80211_wiphy_register`,
  `linuxkpi_alloc_netdev`, `linuxkpi_free_netdev`, `linuxkpi_ieee80211_channel_to_frequency`,
  `linuxkpi_ieee80211_frequency_to_channel`, `linuxkpi_ieee80211_get_channel` (already in `lx_80211.c`),
  `linuxkpi_net_ratelimit`, `linuxkpi_debug_80211`. These belong to task 4.
- **Firmware:** `linuxkpi_request_firmware`, `linuxkpi_request_firmware_nowait`. Back them with
  `lx_os_firmware()`, which reads the small image's `fw/`. See task 2 for the names.
- **Stubs:** `brcmf_bca_ops`, `brcmf_cyw_ops`, `brcmf_wcc_ops`, the vendor operation tables of the `bca/`,
  `cyw/` and `wcc/` directories. Only the vendor brcmfmac picks for the BCM4375 needs a real one (see task 2).
  Build that directory's file, or give the others empty tables.

Start with stubs that say their name, so that every missing piece shows in the log when it is first reached.
**Done when** `BRCMFMAC_STRICT=1 tools/mk-brcmfmac.sh` succeeds and produces `out/brcmfmac.o`.

## Task 2: teach brcmfmac the BCM4375

brcmfmac 7.0.14 knows the 4378 and 4387 (Apple's) but not the 4375. Add the 4375 next to them, as patches in
`wifi/lx/patches/`:
- **IDs:** chip ID 0x4375 (`brcm_hw_ids.h`) and PCIe device ID 0x4475. The device table at the end of
  `pcie.c` (`BRCMF_PCIE_DEVICE(..., WCC_SEED)` for the 4378/4387) gives the vendor and says whether the
  random seed is written. The 4375 needs the seed: step 9 showed the firmware stops early without it.
- **RAM base 0x170000:** brcmfmac's `chip.c` chooses the RAM base by chip ID. Add 0x4375 to that switch.
  (RAM size 0x190000 comes from the CR4 banks and needs nothing.)
- **PMU `max_res_mask` 0x7fffffff:** without it the fast clock (HT) never comes up (step 9). brcmfmac has
  nothing like this. Write it in the PMU core (0x18012000, register 0x61c) before the ARM is released, as
  `step_firmware()` does.
- **Firmware names:** add a `BRCMF_FW_CLM_DEF` / `BRCMF_FW_ENTRY` for chip 0x4375 revision 5 (`pcie.c`, near
  line 71 and line 112). Map the names it asks for to our files in the firmware hook: the firmware is
  `fw/bcm4375_sta.bin` and the CLM blob is `fw-wifi-y2q/bcmdhd_clm.blob` (add it to the small image in
  `userland/build.sh`, next to line 563). brcmfmac reads the NVRAM as **text** and converts it itself, but
  `fw/bcm4375_nvram.bin` is already converted by `tools/brcm-nvram.py`. Either ship the text file
  (`fw-wifi-y2q/nvram.txt_CS01_semco_b1`) under the name brcmfmac asks for, or make it take the converted
  file as it is. The first is less code.

**Done when** the driver downloads the firmware itself and reaches the state of step 10: brcmfmac's log shows
the shared area version 7 and the firmware console prints the `RTE (SDIO-PCIE-MSGBUF) 18.41.117` banner.

## Task 3: the PCI device under brcmfmac, from redwood_pcie.c

brcmfmac's probe expects a `pci_dev` for 14e4:4475 with BARs, config space, bus mastering, DMA and an
interrupt. All of it exists in `redwood_pcie.c` as steps. Turn it into functions the PCI glue can call:
- **Bring-up before probe:** steps 1 to 8 only: power, PHY, link (from a cold radio), config window, BARs
  (BAR0 64 KB at 0x60300000, BAR2 4 MB at 0x60400000), command register, EROM. Then the SMMU part of step 12
  (`bdf_to_sid_fill()`). Steps 9 to 14 are the driver's job from here on (firmware download, rings).
- **Config space:** reads and writes through the iATU configuration window to bus 1 (step 6's `conf`).
  brcmfmac moves its BAR0 window through config register 0x80.
- **BARs:** `pci_resource_start`/`len` return the two above; the driver maps them through
  `lx_os_map_registers()`.
- **DMA:** `dma_alloc_coherent` goes to `lx_os_radio_memory()` (`redwood_lx.c`), which already maps through
  `redwood_wifi_radio_map()`. Pass the radio's address (0xa0000000 and up) to the driver as the
  `dma_addr_t`, never the physical address. Streaming DMA for packets (`dma_map_single`) needs the same:
  give out buffers from radio-mapped memory.
- **Interrupts:** first make it work with **polling**. A thread reads the PCIe2 mailbox interrupt status
  (`0xc30`), writes it back to clear it, and calls brcmfmac's interrupt thread function, as `lx_bus.c`'s
  `bus_looks()` polls ath11k. MSI comes later: the device tree's `qcom,pcie0_msi@17a10040` is the GIC
  register the radio writes to. Map that page into the radio's SMMU context as a device page, the way
  `redwood_wifi_radio_msi()` does for redwood, and attach handlers with `lx_os_irq_attach()`.
- **Teardown:** on the way back to the loader the SMMU stream is already put back (`redwood_wifi_stop()` /
  `redwood_wifi_quiesce()`). Also hold PERST low and WL_REG_ON low there, so the next run finds a cold radio.

**Done when** brcmfmac probes the device, downloads the firmware, sets up its rings and finishes its
first ioctls without SMMU faults (`redwood_wifi_radio_faults()` after probe).

## Task 4: cfg80211, the network interface and joining

brcmfmac is a FullMAC driver: the firmware does the 802.11 work, and the driver speaks cfg80211. redwood's
`lx_80211.c` is a mac80211 layer for ath11k, so most of it does not apply. What is needed:
- `wiphy_new` / `wiphy_register` / `wiphy_free` and `alloc_netdev` / `free_netdev`, enough for brcmfmac's
  `brcmf_cfg80211_attach()` and its interface setup.
- A path from brcmfmac's net device to the kernel's: `lx_os_net_attach()` (with the MAC address),
  `lx_os_net_input()` for received frames, `lx_os_net_link()`, and a transmit entry like ath11k's
  `lx_transmit()` (`lx_bus.c`, around line 860) that calls the netdev's `ndo_start_xmit`.
- **Scan and join** through brcmfmac's cfg80211 operations (scan, connect) for the network in the small
  image's `fw/network` (`lx_os_network()`). For WPA2, the firmware can run the 4-way handshake itself (the
  firmware supplicant, which brcmfmac uses when the firmware reports it). Check that before porting the
  host supplicant (`lx_wpa.c`).
- How iOS's side asks for a station: `bsd/sys/redwood_sta.h` and `redwood_apple80211`. Look at how
  ath11k's driver serves it in `redwood_lx.c` / `lx_80211.c`.

**Done when** a scan lists networks in the log, the phone joins `fw/network`, and traffic passes (DHCP and
a ping, or whatever redwood's ath11k test uses).

## Task 5: start it with the system

`redwood_wifi_init()` returns early on anything but redwood (it checks the SoC base), and
`redwood_pcie_init()` only runs the steps under `pcie_step=N`. Add a y2q path to start the driver: run the
task 3 bring-up, then the driver's probe, in a thread so the boot does not wait (as `wifi=1` does on
redwood with `whole_thread()`). Keep `pcie_step=N` working for experiments.

## Task 6: firmware details

- **NVRAM:** SEMCO `CS01_semco_b1` or Murata `1rh_es4x_b1`? Samsung chooses from a `.cid.info` that was lost
  with userdata. The firmware runs with SEMCO's; try both and compare the console and the scan results.
- **CLM blob:** `fw-wifi-y2q/bcmdhd_clm.blob`. Without it the radio may refuse channels.
- **MAC address:** from the EFS backup `y2q-dump/pre-repartition/efs.img`, file `/wifi/.mac.info`. Give it as
  `wifi_mac=` the way xl does for redwood.

Display off and brightness wait until Wi-Fi works (plan and DisplayDxe offsets are in the session memory
note `y2q-display-power-uefi`).
