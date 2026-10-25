#!/bin/bash
# Save our changes to the Mu-Silicium tree (../Mu-Silicium, a clone of Project-Silicium/Mu-Silicium
# with our ports in it) into this directory: mu-silicium.patch for the files that upstream has,
# files/ for the ones it does not. apply.sh puts them into a fresh clone.
#
# The ports: redwood (Xiaomi, POCO X5 Pro) and y2q (Samsung, Galaxy S20+). Each one's device tree
# was read off its phone, so it goes through tools/dtb-scrub.py on the way: the boot loader's
# arguments in it carry that phone's serial numbers.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../Mu-Silicium
PORTS="Xiaomi/redwood Samsung/y2q"
git -C $MU rev-parse HEAD > BASE
git -C $MU diff -- BootShim Silicon > mu-silicium.patch
rm -rf files
mkdir -p files/Resources/Configs files/Resources/DTBs files/Binaries \
	files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
for port in $PORTS; do
	vendor=${port%/*} device=${port#*/}
	mkdir -p files/Platforms/$vendor
	cp -r $MU/Platforms/$vendor/${device}Pkg files/Platforms/$vendor/
	cp $MU/Resources/Configs/$device.toml files/Resources/Configs/
	cp $MU/Resources/DTBs/$device.dtb files/Resources/DTBs/
	cp -r $MU/Binaries/$device files/Binaries/
done
python3 -I ../tools/dtb-scrub.py files/Resources/DTBs/*.dtb
cp $MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.[ch] files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/
find files -name __pycache__ -prune -exec rm -rf {} +
echo "uefi/export.sh: base $(cut -c1-12 BASE), patch $(wc -l < mu-silicium.patch) lines, $(find files -type f | wc -l) files"
