## Firmware Infos

- **Device:** Samsung Galaxy S20+ 5G (y2q, SM-G986N)
- **Region:** Korea
- **Version:** `G986NKSS8IYC2` / `BOOT.XF.3.2-00248-SM8250-1`

Extracted with [UEFIReader](https://github.com/WOA-Project/UEFIReader) from the phone's own `xbl` partition.
`Platforms/Samsung/y2qPkg/Include/*.inc` list the same 55 drivers as Mu-Silicium's `r8qPkg` (Galaxy S20 FE,
the same SoC and the same Samsung boot firmware generation); every one of them is present here.

## Patches/Fixes

The same five as Mu-Silicium's `Binaries/r8q` (Galaxy S20 FE, `BOOT.XF.3.2-00278-SM8250-2`), which keeps each
original next to its `.patched.efi`. The `.inf` files here name the `.patched.efi`; the originals stay beside them.

| Driver | y2q change | How it was placed |
|---|---|---|
| ButtonsDxe | r8q's `ButtonsDxe.patched.efi` as it is | y2q's original is byte-identical to r8q's |
| UsbMsdDxe | r8q's `UsbMsdDxe.patched.efi` as it is | y2q's original is byte-identical to r8q's |
| DALSYSDxe | `0x2c50c`: `00` -> `01` (UFS cache coherence) | 64 bytes on each side match r8q's patch site, once |
| TzDxeLA | `0xd469`: `00` -> `01` (TZ applet flag) | same `.data` layout; y2q's `ldrb` at `0x2e28` reads the same byte as r8q's |
| UsbConfigDxe | `0x5590`, `0x55ec`: `cbz w10, +0x44` -> `b +0x44` (no IOMMU detach at ExitBootServices) | the same function as r8q's, 0x104 bytes earlier |

What each patch is for, and who made it: Mu-Silicium's `Binaries/r8q/README.md`.
