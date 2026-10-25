**English** | [한국어](README.ko.md)

# G986N-iOS — iOS 16.5 on the Galaxy S20+ (SM-G986N)

This repository runs Apple's open-source **XNU kernel** (`xnu-8796.121.2`, the kernel of iOS 16.5) and the
**unmodified iOS 16.5 user space** on the **Samsung Galaxy S20+ 5G (y2q, SM-G986N, Snapdragon 865)**. It is a
port of [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG. Both of HackiOS's repositories,
hackios-xnu-kernel and [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland), are in it with
their histories, so one clone is the whole tree.

> **No Apple files are in this repository.** `ios165/fetch.sh` downloads the iOS 16.5 IPSW from Apple's own CDN to
> *your* machine and extracts what is needed there. This is an independent research project, not affiliated with,
> authorised or endorsed by Apple Inc. iOS, iPhone and Apple are trademarks of Apple Inc.

## What works on the y2q

iOS 16.5 starts from the phone's boot partition by itself, without a computer (`xl install-autoboot`).

- **Root:** the iOS volume on the phone's flash; data that iOS writes is kept across boots.
- **GPU:** the Adreno 650 draws for CoreAnimation through Mesa/freedreno (`gpu=1`).
- **Wi-Fi:** the BCM4375 through Linux's brcmfmac on a compatibility layer (`wifi=1`, [docs/y2q-wifi.md](docs/y2q-wifi.md)).
- **Display:** frames are page-flipped and enlarged by the display controller's own pipes, at the panel's 60 Hz
  ([docs/y2q-display.md](docs/y2q-display.md)).
- **Also:** the touch screen, the battery and charger readings (MAX77705), up to 8 CPU cores.
- **Not yet:** brightness and turning the display off.

## What the repository leaves out, and how it comes back

Nothing big, nothing of Apple's and nothing of the phone's own is in git (`.gitignore` is a whitelist). One script
fetches or builds all of it, in order, skipping whatever is already there:

```sh
common/bootstrap.sh y2q
```

| Left out | ~Size | Made by |
|---|---|---|
| iOS 16.5 (`ios165/`) | 18 GB | `ios165/fetch.sh`: from Apple's IPSW, on your machine |
| Apple's XNU sources (`xnu-src/`) | 0.5 GB | `xnu/apply.sh`: Apple's tag + `xnu/redwood.patch` + `xnu/files/` |
| the kernel's dependencies and SDK (`xnu-deps/`) | 1.2 GB | `xnu-deps/setup.sh` |
| Mu-Silicium with our UEFI port (`Mu-Silicium/`) | 6 GB | `uefi/apply.sh`, then `build_uefi.py -d y2q` |
| the phone's GPU and Wi-Fi firmware, its MAC address (`fw-gpu/`, `fw-wifi-y2q/`) | 5 MB | `tools/y2q-firmware.sh`, from the phone in TWRP |
| the images, kernel and boot images (`userland/*.apfs`, `out/`) | 2.5 GB | `common/build.sh y2q FLASHBOOT=1 CRYPTEXES=flash` |

**Needs:**
- Linux x86-64 and about 40 GB of free disk.
- theos with `iPhoneOS16.5.sdk`, to build the kernel and our programs.
- The packages of Mu-Silicium's `setup_env.sh`, to build the UEFI ([uefi/README.md](uefi/README.md)).
- `adb`, and a Galaxy S20+ (SM-G986N) with TWRP.

## Installing on the phone

Back up the phone's own boot image first (it holds Android's kernel). With the phone in TWRP:

```sh
XL_DEVICE=y2q ./xl install-autoboot     # writes out/y2q-autoboot.img to the boot partition (asks first)
```

The phone then starts iOS by itself. A volume key during the loader's two-second wait gives its USB commands
instead, for test runs from the computer (`XL_DEVICE=y2q ./xl run`, `./xl log`, ...). To go back to Android, from
TWRP:

```sh
XL_DEVICE=y2q ./xl restore-boot BACKUP.img
```

## Where things are

| Path | What |
|---|---|
| `common/devices/y2q.conf` | the device profile: addresses, boot arguments, what the run needs |
| `common/` | `bootstrap.sh`, `setup.sh`, `build.sh`, `run.sh` |
| `xl` | drives the loader on the phone over USB |
| `xnu/` | our changes to XNU: `redwood.patch` (Apple's files) and `files/` (new ones; the drivers are `bsd/dev/redwood_*.c`) |
| `xnuloader/` | our EFI loader, which starts XNU from the UEFI |
| `uefi/` | the UEFI port (Mu-Silicium): `files/Platforms/Samsung/y2qPkg` |
| `wifi/` | the Linux driver compatibility layer and brcmfmac's glue |
| `userland/` | our init, injected libraries, launchd jobs, the image builder |
| `docs/` | `y2q-wifi.md`, `y2q-display.md`, `PORTING.md` |

## Credits and licenses

- **HackiOS** by OOSEX GANG ([t.me/oosexgangdevs](https://t.me/oosexgangdevs)) is the base of all of this: the
  kernel work, the loader, the user space and the redwood and QEMU ports.
- The kernel side (`xnu/`, `xnuloader/`, scripts) is under the [Apple Public Source License 2.0](LICENSE): our changes
  to XNU are a derivative of XNU. The userland side (`userland/`, `ios165/`'s scripts, the userland's tools) is under
  the [MIT License](LICENSE-userland).
- Third-party code keeps its own license: see [prebuilt/NOTICES.md](prebuilt/NOTICES.md).
  - In the repository: libzbitmap (MIT), libdeflate (MIT).
  - Fetched by our scripts, not stored here: Linux's brcmfmac and ath11k (ISC, BSD-3-Clause-Clear), FreeBSD's
    LinuxKPI (BSD-2-Clause) and Mesa (MIT).
- `uefi/files/Binaries/` holds UEFI drivers from the phones' own firmware (Samsung's, Xiaomi's), distributed the way
  Project Silicium distributes them for other devices.
- The device tree in `uefi/files/Resources/DTBs/` was read off the phone; `tools/dtb-scrub.py` has taken the phone's
  serial numbers out of it.
