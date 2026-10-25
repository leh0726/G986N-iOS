#!/bin/bash
# Build Broadcom's FullMAC driver, brcmfmac, for our kernel: the Galaxy S20+'s radio is a BCM4375 on PCIe
# (docs/y2q-wifi.md). The same three things meet as in tools/mk-ath11k.sh, and the same way:
#
#   Linux's brcmfmac      drivers/net/wireless/broadcom/brcm80211/{brcmfmac,brcmutil,include} of the
#                         same pinned release of Linux as ath11k (ISC);
#   FreeBSD's LinuxKPI    the headers FreeBSD compiles Linux drivers with, patched with wifi/lx/patches
#                         (the tree mk-ath11k.sh unpacks; this script unpacks it the same way if needed);
#   wifi/lx               ours: the functions underneath, on the kernel's bsd/sys/redwood_lx.h.
#
# Differences from ath11k: brcmfmac is a cfg80211 driver (the firmware does the 802.11 work, there is no
# mac80211 underneath), and its bus here is PCI, not a platform device. Of its files only those of the
# PCIe bus and the msgbuf protocol are built (no SDIO, USB, BCDC, device tree or ACPI).
#
# Not all of it builds yet, and not all that it calls exists yet. A file that does not compile is left out
# and its first errors are shown; a function of the driver that is not built becomes a stub that says its
# name and answers 0 (out/stubs.txt); anything else missing (cfg80211, PCI, network stack functions that
# LinuxKPI or wifi/lx do not have yet) is listed in out/missing.txt. Unlike mk-ath11k.sh this does not stop
# at the first missing name: it says how far the driver is from linking (BRCMFMAC_STRICT=1 makes missing
# names an error, for when the list is empty).
#
# What comes out, in xnu-deps/build/brcmfmac:
#   src/brcmfmac/ src/brcmutil/ src/include/   the driver's files, with wifi/lx/brcm/patches applied
#   out/brcmfmac.o   the object (when nothing is missing)   out/stubs.txt, out/missing.txt, out/failed.txt
# Needs: what mk-ath11k.sh needs. The Linux archive is shared with mk-ath11k.sh's when that has run.
#
# usage: tools/mk-brcmfmac.sh [fetch]        "fetch" stops when the driver's sources are there
set -e
VERSION=7.0.14
SHA256=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/brcmfmac
A=$ROOT/xnu-deps/build/ath11k           # where mk-ath11k.sh keeps the two archives
BRCM=drivers/net/wireless/broadcom/brcm80211
mkdir -p "$B" "$A"
cd "$B"

# our changes to the driver (the BCM4375: wifi/lx/brcm/patches): when they change, the sources are unpacked again
PATCHES=$(cat "$ROOT"/wifi/lx/brcm/patches/*.patch 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat src/.release 2>/dev/null)" != "$VERSION $SHA256 $PATCHES" ]; then
	[ -f "$A/linux-$VERSION.tar.xz" ] || curl -sL --fail -o "$A/linux-$VERSION.tar.xz" \
		https://cdn.kernel.org/pub/linux/kernel/v${VERSION%%.*}.x/linux-$VERSION.tar.xz
	echo "$SHA256  $A/linux-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf src unpacked
	mkdir unpacked
	tar xJf "$A/linux-$VERSION.tar.xz" -C unpacked --strip-components=1 linux-$VERSION/$BRCM/brcmfmac \
		linux-$VERSION/$BRCM/brcmutil linux-$VERSION/$BRCM/include
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in $(find unpacked/$BRCM -name '*.[ch]'); do
		case "$(head -12 "$f")" in
		*"SPDX-License-Identifier: ISC"*) ;;
		*"Permission to use, copy, modify, and/or distribute this software"*) ;;
		*) echo "mk-brcmfmac.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mkdir src
	mv unpacked/$BRCM/brcmfmac unpacked/$BRCM/brcmutil unpacked/$BRCM/include src/
	rm -rf unpacked
	for p in "$ROOT"/wifi/lx/brcm/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d src -p1 < "$p"
	done
	echo "$VERSION $SHA256 $PATCHES" > src/.release
fi
[ "$1" = fetch ] && { echo "mk-brcmfmac.sh: brcmfmac of Linux $VERSION: $B/src/brcmfmac"; exit 0; }
W=$ROOT/wifi/lx

# --- FreeBSD's LinuxKPI and its generated headers: mk-ath11k.sh's, made by it when they are not there
if [ ! -d "$A/gen" ] || [ ! -f "$A/freebsd/.release" ]; then
	echo "mk-brcmfmac.sh: preparing FreeBSD's LinuxKPI (tools/mk-ath11k.sh fetch, then its headers)"
	"$ROOT/tools/mk-ath11k.sh" > "$B/ath11k-prepare.log" 2>&1 || true
	[ -d "$A/gen" ] && [ -f "$A/freebsd/.release" ] ||
		{ echo "mk-brcmfmac.sh: no LinuxKPI tree (see $B/ath11k-prepare.log)" >&2; exit 1; }
fi
S=$A/freebsd/sys

# --- compile: the files of the PCIe bus and the msgbuf protocol, brcmutil, and ours
DRIVER="core common firmware fweh fwil fwvid feature cfg80211 chip pcie msgbuf commonring flowring proto p2p
	btcoex pno vendor xtlv"            # (DEBUG: the driver's messages, chosen at run time; debug.c's debugfs is wifi/lx/brcm's)
UTIL="utils d11"
VENDORS="wcc cyw bca"
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include -I$A/gen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc/brcmfmac -Isrc/include -include $W/include/lx_prelude.h -include $W/include/lx_brcmfmac.h -DCONFIG_BRCMFMAC_PCIE
	-DCONFIG_BRCMFMAC_PROTO_MSGBUF -DDEBUG -DCONFIG_BRCMDBG -DKBUILD_MODNAME=\"brcmfmac\""
source "$ROOT/xnu-env.sh"
[ -f "$XNU_SRC/bsd/sys/redwood_lx.h" ] || { echo "mk-brcmfmac.sh: the kernel tree has no bsd/sys/redwood_lx.h" >&2; exit 1; }
DFLAGS="$DFLAGS -DLX_REDWOOD_LX_H=\"$XNU_SRC/bsd/sys/redwood_lx.h\" -DLX_REDWOOD_QMI_H=\"$XNU_SRC/bsd/sys/redwood_qmi.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$ROOT/tools/mk-brcmfmac.sh" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
for f in $DRIVER; do compile src/brcmfmac/$f.c obj/brcmfmac_$f.o $DFLAGS & done
for f in $UTIL; do compile src/brcmutil/$f.c obj/brcmutil_$f.o $DFLAGS & done
# the three firmware vendors' tables (fwvid.c names all three; which one a chip has is the device table's)
for v in $VENDORS; do compile src/brcmfmac/$v/core.c obj/brcmfmac_$v.o $DFLAGS & done
# brcmfmac's own: its PCI bus, cfg80211 and the kernel's entry points (wifi/lx/brcm)
for f in "$W"/brcm/*.c; do compile "$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src -I$W/brcm & done
# wifi/lx's own files: lx_base.c (memory, locks, work, socket buffers, the C library) serves any driver;
# the rest of wifi/lx/src is ath11k's (mac80211, the AHB platform bus) and not part of this object
compile "$W/src/lx_base.c" obj/lx_base.o $DFLAGS -I$W/src &
compile "$W/src/lx_wpa.c" obj/lx_wpa.o $DFLAGS -I$W/src &		# (the four-way handshake: lx_brcm_sta.c's too)
wait
: > out/failed.txt
for f in obj/*.err; do
	if [ -s "$f" ] && [ ! -f "${f%.err}" ]; then
		name=$(basename "$f" .o.err)
		echo "$name" >> out/failed.txt
		echo "--- $name does not compile:"
		grep -m4 -A2 "error:" "$f" | cut -c1-200
	fi
done
OBJECTS=$(ls obj/*.o)
[ -n "$OBJECTS" ] || { echo "mk-brcmfmac.sh: nothing compiled" >&2; exit 1; }

# --- tie what compiled into one object, and sort what it still asks for
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
: > out/missing.txt
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|section\$*) continue ;;
	brcmf_*|brcmu_*|__brcmf_*) ;;
	*) echo $s >> out/missing.txt; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
echo "mk-brcmfmac.sh: $(echo $OBJECTS | wc -w) files compiled, $(wc -l < out/failed.txt) did not (out/failed.txt);" \
	"$(wc -l < out/stubs.txt) driver functions stubbed; $(wc -l < out/missing.txt) other names missing (out/missing.txt)"
if [ -s out/missing.txt ] || [ -s out/failed.txt ]; then
	[ -n "$BRCMFMAC_STRICT" ] && exit 1
	echo "mk-brcmfmac.sh: not linked into out/brcmfmac.o yet"
	exit 0
fi
$CC -c out/stubs.s -o out/stubs.o
printf '_lx_brcm_prepare\n_lx_brcm_start\n_lx_brcm_stop\n_lx_brcm_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/brcmfmac.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-brcmfmac.sh: out/brcmfmac.o"
