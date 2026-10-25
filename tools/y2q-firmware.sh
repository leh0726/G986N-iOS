#!/bin/bash
# Take the Galaxy S20+ (y2q)'s own firmware off the phone, into the folders the build reads it from
# (userland/build.sh puts it into the small image). None of it is in the repository: it is Qualcomm's,
# Broadcom's and Samsung's, and the MAC address is this phone's.
#
#   fw-gpu/        the Adreno 650's firmware, /vendor/firmware/a650_*         (gpu=1)
#   fw-wifi-y2q/   the BCM4375's firmware, CLM blob and NVRAM files, /vendor/firmware/{bcmdhd_*,nvram.txt_*,bcm4375*}
#                  and the radio's MAC address, /efs/wifi/.mac.info          (wifi=1; xl passes it as wifi_mac=)
#
# usage: tools/y2q-firmware.sh       with the phone in TWRP (adb: recovery), or in Android with root
#                                    (Magisk's su; adb: device)
# A file that is already here is left alone; delete it to fetch it again. The network to join is not
# fetched: write fw-wifi-y2q/network yourself if you want one (its name, then its passphrase, on two lines).
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
command -v adb >/dev/null || { echo "y2q-firmware: no adb" >&2; exit 1; }
case "$(adb get-state 2>/dev/null)" in
recovery)
	sh() { adb shell "$1"; }
	# TWRP mounts what it is asked to; both are read only here
	adb shell 'mount /vendor 2>/dev/null; mount /efs 2>/dev/null; true' ;;
device)
	sh() { adb shell "su -c '$1'"; }
	sh 'id' | grep -q 'uid=0' || { echo "y2q-firmware: Android without root (su): use TWRP instead" >&2; exit 1; } ;;
*)
	echo "y2q-firmware: no phone (adb get-state: $(adb get-state 2>&1)); start it into TWRP" >&2; exit 1 ;;
esac
model=$(sh 'getprop ro.product.device; getprop ro.product.vendor.device' | tr -d '\r' | grep -m1 . || true)
case "$model" in
y2q*|"") ;;
*) echo "y2q-firmware: this is a $model, not a y2q" >&2; exit 1 ;;
esac

# pull <dir> <phone path>: by way of cat, which works the same under su
pull() {
	local out="$1/$(basename "$2")"
	[ -s "$out" ] && return 0
	sh "cat '$2'" > "$out.part" 2>/dev/null && [ -s "$out.part" ] && mv "$out.part" "$out" && echo "  $out" ||
		{ rm -f "$out.part"; echo "  $2: not found on the phone"; return 1; }
}

mkdir -p fw-gpu fw-wifi-y2q
echo "y2q-firmware: the GPU's"
for f in a650_gmu.bin a650_sqe.fw a650_zap.mdt a650_zap.b00 a650_zap.b01 a650_zap.b02; do
	pull fw-gpu /vendor/firmware/$f || true
done
echo "y2q-firmware: the Wi-Fi's"
for f in $(sh 'cd /vendor/firmware && ls bcmdhd_sta.bin_b1 bcmdhd_mfg.bin_b1 bcmdhd_mon.bin_b1 bcmdhd_clm.blob nvram.txt_* bcm4375*.hcd 2>/dev/null' | tr -d '\r'); do
	pull fw-wifi-y2q /vendor/firmware/$f || true
done
if [ ! -s fw-wifi-y2q/mac ]; then
	mac=$(sh 'cat /efs/wifi/.mac.info' 2>/dev/null | tr -d '\r\n ' || true)
	if [[ $mac =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]]; then
		(umask 077; echo "$mac" > fw-wifi-y2q/mac)
		echo "  fw-wifi-y2q/mac"
	else
		echo "  /efs/wifi/.mac.info: not readable (the radio then uses an address of its own firmware)"
	fi
fi
missing=
for f in fw-gpu/a650_sqe.fw fw-gpu/a650_gmu.bin fw-gpu/a650_zap.mdt fw-wifi-y2q/bcmdhd_sta.bin_b1 fw-wifi-y2q/nvram.txt_CS01_semco_b1; do
	[ -s $f ] || missing="$missing $f"
done
[ -z "$missing" ] && echo "y2q-firmware: done" || echo "y2q-firmware: still missing:$missing"
