#!/bin/bash
# Builds build/card.img: FAT16 boot partition, ext2 root, swap.
# Works on plain files only and never opens a real disk or needs root.
#
# Layout in MiB. Nothing is allocated between 1500 and 1600, because the
# 4 GB card showed unstable bits near 1530 and 1540 (see NOTES.md).
#   p1    1 -  512  FAT16, type 0x0e, the type the Jornada already read on this card
#   p2  512 - 1500  ext2, Linux root
#   p3 1600 - 1632  swap
set -euo pipefail

P=$(cd "$(dirname "$0")/.." && pwd)
V=$P/vendor/jlime-donkey
B=$P/build/card
IMG=$P/build/card.img
PROBE=$P/build/probe
PDOOM=$P/build/pdoom
PWRITE=$P/build/write
FINDER=$P/build/finder
SPLASH=$P/build/pocketsplash
WORDS=$P/build/words.txt
PRIVATE=$HOME/pocket-reality-private
WAD=$HOME/pocket-reality-wad/Doomu.wad
export PATH=$P/tools/root/usr/bin:$PATH MTOOLS_SKIP_CHECK=1

P1_START=1 P1_SIZE=511
P2_START=512 P2_SIZE=988
P3_START=1600 P3_SIZE=32
TOTAL=1632

[ "$(md5sum < "$V/vargtass-image-5.0.tar.bz2" | cut -d' ' -f1)" = e23064bfebad6aa0f82ac94a6239ed0b ]
[ -x "$PROBE" ]
[ -x "$PDOOM" ]
[ -x "$PWRITE" ]
[ -x "$FINDER" ]
[ -x "$SPLASH" ]
[ -f "$WORDS" ]
[ -f "$WAD" ]

rm -rf "$B"
mkdir -p "$B/root"

# Everything touching root-owned files and device nodes stays inside one
# fakeroot session, so ownership survives into the ext2 filesystem.
fakeroot bash -euo pipefail -s "$V" "$B" "$PROBE" "$PDOOM" "$P/src/finder/S02pocket" "$P2_SIZE" "$PWRITE" "$FINDER" "$WORDS" "$SPLASH" "$P" <<'FAKEROOT'
V=$1 B=$2 PROBE=$3 PDOOM=$4 HOOK=$5 SIZE=$6 PWRITE=$7 FINDER=$8 WORDS=$9 SPLASH=${10} P=${11}
tar -xjf "$V/vargtass-image-5.0.tar.bz2" -C "$B/root" --same-owner --numeric-owner -p
cat >> "$B/root/etc/jlime.conf" <<CONF
CONFIGURED=true
NOSETTIME=true
SESSION=none
CONF
install -m 755 "$PROBE" "$B/root/usr/bin/probe"
install -m 755 "$PDOOM" "$B/root/usr/bin/pdoom"
[ -x "$P/build/pwolf" ] && install -m 755 "$P/build/pwolf" "$B/root/usr/bin/pwolf"
[ -x "$P/build/pocketlua" ] && install -m 755 "$P/build/pocketlua" "$B/root/usr/bin/pocketlua"
install -m 755 "$PWRITE" "$B/root/usr/bin/pocketwrite"
install -m 755 "$FINDER" "$B/root/usr/bin/finder"
install -m 755 "$SPLASH" "$B/root/usr/bin/pocketsplash"
install -m 755 "$P/src/finder/pocket-doom" "$B/root/usr/bin/pocket-doom"
install -m 755 "$P/src/finder/psplash" "$B/root/etc/init.d/psplash"
install -m 755 "$P/src/finder/pocket-done" "$B/root/etc/init.d/pocket-done"
ln -s ../init.d/pocket-done "$B/root/etc/rc0.d/S85pocket-done"
rm -f "$B/root/usr/bin/psplash-write"
if ls "$P"/build/art/*.img > /dev/null 2>&1; then
	install -d "$B/root/usr/share/pocket/art"
	install -m 644 "$P"/build/art/*.img "$B/root/usr/share/pocket/art/"
fi
install -D -m 644 "$WORDS" "$B/root/usr/share/pocket/words.txt"
install -m 755 "$HOOK" "$B/root/etc/init.d/pocket"
ln -s ../init.d/pocket "$B/root/etc/rc5.d/S95pocket"
# Nothing here needs apmd, and without it the power button sleeps directly.
rm -f "$B/root/etc/rc5.d/S20apmd"
truncate -s "${SIZE}M" "$B/p2.img"
# Conservative features: e2fsprogs 1.47 defaults are untested on Linux 2.6.17.
mke2fs -q -F -t ext2 -b 4096 -I 128 -m 1 -L jlime -O none,filetype,sparse_super \
	-d "$B/root" "$B/p2.img"
FAKEROOT
tune2fs -c 0 -i 0 "$B/p2.img" > /dev/null

truncate -s "${P1_SIZE}M" "$B/p1.img"
# Windows CE showed the mkfs.vfat default layout as an empty card, so copy the
# boot sector settings of an old 1 GB card, which the Jornada reads correctly.
mkfs.vfat -a -F 16 -S 512 -s 32 -R 6 -r 512 -g 255/32 -n POCKET \
	-h $((P1_START * 2048)) "$B/p1.img" > /dev/null
printf 'MSWIN4.1' | dd of="$B/p1.img" bs=1 seek=3 conv=notrunc status=none
mcopy -i "$B/p1.img" "$V/shlo.exe" "$V/shlo.txt" ::/
mcopy -i "$B/p1.img" "$B/root/boot/zImage-2.6.17" ::/zImage
# The WAD is commercial: it goes on the card only, never in the repository.
mmd -i "$B/p1.img" ::/doom
mcopy -i "$B/p1.img" "$WAD" ::/doom/Doomu.wad
# Wolfenstein's game files live only on the card and in the private folder.
WOLF_DATA=$HOME/pocket-reality-private/wolf3d/dl/wolf3d14
if [ -d "$WOLF_DATA" ]; then
	mmd -i "$B/p1.img" ::/wolf3d
	for f in "$WOLF_DATA"/*.WL1; do
		mcopy -i "$B/p1.img" "$f" "::/wolf3d/$(basename "$f" | tr 'A-Z' 'a-z')"
	done
fi
# Stories are private too, and are only added when they exist on this PC.
mmd -i "$B/p1.img" ::/stories
for f in "$PRIVATE"/stories/*.txt; do
	[ -f "$f" ] && mcopy -i "$B/p1.img" "$f" "::/stories/$(basename "$f")"
done

truncate -s "${P3_SIZE}M" "$B/p3.img"
mkswap -q -L swap "$B/p3.img"

rm -f "$IMG"
truncate -s "${TOTAL}M" "$IMG"
sfdisk -q "$IMG" <<TABLE
label: dos
start=$((P1_START * 2048)), size=$((P1_SIZE * 2048)), type=e
start=$((P2_START * 2048)), size=$((P2_SIZE * 2048)), type=83
start=$((P3_START * 2048)), size=$((P3_SIZE * 2048)), type=82
TABLE
dd if="$B/p1.img" of="$IMG" bs=1M seek=$P1_START conv=notrunc,sparse status=none
dd if="$B/p2.img" of="$IMG" bs=1M seek=$P2_START conv=notrunc,sparse status=none
dd if="$B/p3.img" of="$IMG" bs=1M seek=$P3_START conv=notrunc,sparse status=none

sha256sum "$IMG" | tee "$IMG.sha256"
