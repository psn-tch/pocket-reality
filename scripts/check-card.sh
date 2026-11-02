#!/bin/bash
# Read-only health check of the 4 GB card. Compares the Linux partition with
# the JLime download, because the card can return wrong data without any error.
# Files that first boot or update-card.sh change on purpose are skipped.
set -euo pipefail

ID=/dev/disk/by-id/usb-Generic-_Compact_Flash_058F84618461-0:0
SECTORS=7831152
P=$(cd "$(dirname "$0")/.." && pwd)
TAR=$P/vendor/jlime-donkey/vargtass-image-5.0.tar.bz2

die() { echo "REFUSING: $*" >&2; exit 1; }

[ -e "$ID" ] || die "the 4 GB CF card is not plugged in"
DEV=$(readlink -f "$ID")
[ "$(cat "/sys/block/$(basename "$DEV")/size")" = "$SECTORS" ] || die "$DEV is not the 4 GB card"

WORK=$(mktemp -d)
MNT=$WORK/card
REF=$WORK/ref
mkdir "$MNT" "$REF"
trap 'mountpoint -q "$MNT" && sudo umount "$MNT"; rm -rf "$WORK"' EXIT

tar -xjf "$TAR" -C "$REF" --no-same-owner --no-same-permissions --exclude='./dev/*' 2> /dev/null || true
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
sudo blockdev --flushbufs "$DEV"
sudo mount -t ext2 -o ro "${DEV}2" "$MNT"

bad=0
checked=0
while IFS= read -r -d '' file; do
	rel=${file#"$REF"/}
	case "$rel" in
	etc/group | etc/passwd | etc/jlime.conf | etc/init.d/psplash | etc/fstab | \
	usr/lib/opkg/status | usr/bin/psplash-write | var/* | tmp/* | etc/rc?.d/*)
		continue
		;;
	esac
	checked=$((checked + 1))
	if ! sudo cmp -s "$file" "$MNT/$rel"; then
		echo "DAMAGED: /$rel"
		bad=$((bad + 1))
	fi
done < <(find "$REF" -type f -print0)

echo "checked $checked files, $bad damaged"
[ "$bad" -eq 0 ]
