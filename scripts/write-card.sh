#!/bin/bash
# Writes build/card.img to the 4 GB CF card, then reads it back to prove it.
# Refuses anything except that exact card. "verify" skips writing and only
# re-checks the card against the image, for the card health check.
set -euo pipefail

ID=/dev/disk/by-id/usb-Generic-_Compact_Flash_058F84618461-0:0
SECTORS=7831152
P=$(cd "$(dirname "$0")/.." && pwd)
IMG=$P/build/card.img
MODE=${1:-write}

die() { echo "REFUSING: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "no image at $IMG"
[ -e "$ID" ] || die "the 4 GB CF card is not plugged in"
DEV=$(readlink -f "$ID")
B=$(basename "$DEV")

[ "$(cat "/sys/block/$B/size")" = "$SECTORS" ] || die "$DEV is not the 4 GB card size"
[ "$(cat "/sys/block/$B/removable")" = 1 ] || die "$DEV is not removable"
ROOT=$(findmnt -no SOURCE / | sed 's/\[.*//')
case "$ROOT" in "$DEV"*) die "$DEV holds the running system" ;; esac
BYTES=$(stat -c %s "$IMG")
[ "$BYTES" -lt $((SECTORS * 512)) ] || die "image is larger than the card"

verify() {
	# The desktop automounts the new partitions, and a mounted FAT volume has
	# its dirty flag set, which reads as a false mismatch.
	sync
	sudo udevadm settle
	sleep 5
	for p in "$DEV"?*; do
		if findmnt -n "$p" > /dev/null; then
			sudo umount "$p"
		fi
	done
	sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
	sudo blockdev --flushbufs "$DEV"
	if sudo cmp -n "$BYTES" "$DEV" "$IMG"; then
		echo "VERIFIED: $BYTES bytes on the card match the image"
	else
		echo "VERIFY FAILED: the card does not match the image" >&2
		exit 2
	fi
}

if [ "$MODE" = verify ]; then
	verify
	exit 0
fi

[ -z "$(lsblk -no MOUNTPOINTS "$DEV" | tr -d '[:space:]')" ] || die "$DEV has mounted partitions"

echo "Device: $DEV"
echo "Model:  $(cat "/sys/block/$B/device/model")"
echo "Size:   $(( $(cat "/sys/block/$B/size") / 2048 )) MiB"
echo "Everything on this card will be erased. Type ERASE to continue:"
read -r answer
[ "$answer" = ERASE ] || die "not confirmed"

sudo dd if="$IMG" of="$DEV" bs=1M oflag=direct conv=fsync status=none
verify
