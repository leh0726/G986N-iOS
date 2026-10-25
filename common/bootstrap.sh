#!/bin/bash
# From a fresh clone to images for a device, in one go: everything the repository leaves out --
# because it is big, or Apple's, or the phone's own -- is fetched or built here, in order. A step
# whose result is already there is skipped, so this can be run again after any failure.
#
#   1. common/setup.sh      the host's tools (and, for the two-repository layout, the userland checkout)
#   2. ios165/fetch.sh      iOS 16.5 out of Apple's IPSW, on this machine (7.4 GB download, ~18 GB on disk)
#   3. xnu/apply.sh         Apple's XNU sources with our patch on top        -> xnu-src/
#   4. xnu-deps/setup.sh    the kernel's build dependencies and SDK         -> xnu-deps/
#   5. uefi/apply.sh        Mu-Silicium with our ports, then its build      -> Mu-Silicium/ (phones; ~3 GB)
#   6. tools/y2q-firmware.sh  the y2q's GPU and Wi-Fi firmware off the phone -> fw-gpu/, fw-wifi-y2q/
#   7. common/build.sh      kernel, loader, images, boot image               -> out/
#
# usage: common/bootstrap.sh <device> [build variables...]
#   common/bootstrap.sh y2q                 the Galaxy S20+ (FLASHBOOT=1 CRYPTEXES=flash by default)
#   common/bootstrap.sh qemu-virt
#   SKIP="ios uefi firmware"                leave steps out (names: setup ios xnu deps uefi firmware build)
#
# The kernel and our programs need theos with its iPhoneOS16.5.sdk ($THEOS, default ~/theos); the UEFI
# needs the packages of Mu-Silicium's setup_env.sh (see uefi/README.md). Step 6 needs the phone in TWRP
# (or a rooted Android) on adb; without it the GPU and Wi-Fi are left out of the images, and step 6 can
# be run later on its own, followed by step 7.
. "$(dirname "$(readlink -f "$0")")/lib.sh"
DEVICE_ARG=$1
load_device "$1"; shift
skip() { [[ " $SKIP " == *" $1 "* ]]; }
step() { echo; echo "=== bootstrap: $* ==="; }
BUILD_VARS=("$@")
[ ${#BUILD_VARS[@]} = 0 ] && [ "$DEVICE_ARG" = y2q ] && BUILD_VARS=(FLASHBOOT=1 CRYPTEXES=flash)

if ! skip setup; then
	step "1. the host"
	common/setup.sh
fi

if ! skip ios; then
	step "2. iOS 16.5 from Apple's IPSW"
	if [ -d ios165/rootfs ]; then echo "already there (ios165/rootfs)"; else ios165/fetch.sh; fi
fi

if ! skip xnu; then
	step "3. the XNU sources"
	if [ -d xnu-src/xnu-8796.121.2 ]; then echo "already there (xnu-src/)"; else xnu/apply.sh; fi
fi

if ! skip deps; then
	step "4. the kernel's dependencies"
	if [ -d "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" ]; then
		xnu-deps/setup.sh
	else
		echo "no theos + iPhoneOS16.5.sdk: the kernel cannot be built here (common/build.sh then uses prebuilt/)"
	fi
fi

if ! skip uefi && [ "$KIND" = phone ]; then
	step "5. the UEFI (Mu-Silicium) for $UEFI_TARGET"
	FD=Mu-Silicium/Build/${UEFI_TARGET}Pkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
	if [ -n "$UEFI_FD" ] || [ -f "$FD" ]; then
		echo "already there (${UEFI_FD:-$FD})"
	else
		[ -d Mu-Silicium ] || uefi/apply.sh
		[ -x venv/bin/python3 ] || python3 -m venv venv
		venv/bin/pip install -q -r Mu-Silicium/pip-requirements.txt
		# mu_nasm comes through NuGet, which needs Mono; without Mono the system's nasm stands in (uefi/README.md)
		D=Mu-Silicium/Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
		if ! command -v mono >/dev/null && command -v nasm >/dev/null && [ ! -e $D/Linux-x86-64/nasm ]; then
			mkdir -p $D/Linux-x86-64
			echo 'version: 20016.1.1' > $D/extdep_state.yaml
			ln -sf "$(command -v nasm)" "$(command -v ndisasm)" $D/Linux-x86-64/
		fi
		(cd Mu-Silicium && . ../venv/bin/activate && python3 build_uefi.py -d "$UEFI_TARGET" -r RELEASE)
		[ -f "$FD" ] || die "the UEFI build made no $FD (Mu-Silicium/Build/BUILDLOG_${UEFI_TARGET}Pkg.txt)"
	fi
fi

if ! skip firmware && [ "$DEVICE_ARG" = y2q ]; then
	step "6. the phone's own firmware"
	if [ -s fw-gpu/a650_sqe.fw ] && [ -s fw-wifi-y2q/bcmdhd_sta.bin_b1 ]; then
		echo "already there (fw-gpu/, fw-wifi-y2q/)"
	elif command -v adb >/dev/null && [[ "$(adb get-state 2>/dev/null)" =~ ^(recovery|device)$ ]]; then
		tools/y2q-firmware.sh
	else
		echo "no phone on adb: the images are made without the GPU's and the Wi-Fi's firmware"
		echo "(later: the phone into TWRP, tools/y2q-firmware.sh, then: common/build.sh y2q ${BUILD_VARS[*]})"
	fi
fi

if ! skip build; then
	step "7. kernel, loader and images"
	common/build.sh "$DEVICE_ARG" "${BUILD_VARS[@]}"
fi

echo
echo "bootstrap: done. Next:"
case "$DEVICE_ARG" in
y2q) cat <<EOF
  XL_DEVICE=y2q ./xl enter              # the phone in TWRP: writes the loader's image to boot (asks first)
  XL_DEVICE=y2q ./xl run                # a run over USB (a volume key during the loader's 2 s wait)
  XL_DEVICE=y2q ./xl install-autoboot   # or: a boot image that starts iOS by itself (asks first)
  Keep a backup of Android's own boot image: XL_DEVICE=y2q ./xl restore-boot FILE puts it back.
EOF
	;;
qemu-virt) echo "  common/run.sh qemu-virt" ;;
*) echo "  see README.md" ;;
esac
