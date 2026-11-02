#!/bin/bash
# Replaces the programs on the 4 GB card in place, keeping stories, saves and
# the stylus calibration. Refuses anything except that exact card.
set -euo pipefail

ID=/dev/disk/by-id/usb-Generic-_Compact_Flash_058F84618461-0:0
SECTORS=7831152
P=$(cd "$(dirname "$0")/.." && pwd)
WOLF_DATA=$HOME/pocket-reality-private/wolf3d/dl/wolf3d14
cd "$P"

die() { echo "REFUSING: $*" >&2; exit 1; }

[ -e "$ID" ] || die "the 4 GB CF card is not plugged in"
DEV=$(readlink -f "$ID")
B=$(basename "$DEV")
[ "$(cat "/sys/block/$B/size")" = "$SECTORS" ] || die "$DEV is not the 4 GB card size"
[ "$(cat "/sys/block/$B/removable")" = 1 ] || die "$DEV is not removable"
ROOT=$(findmnt -no SOURCE / | sed 's/\[.*//')
case "$ROOT" in "$DEV"*) die "$DEV holds the running system" ;; esac
[ -z "$(lsblk -no MOUNTPOINTS "${DEV}2" | tr -d '[:space:]')" ] || die "${DEV}2 is mounted"

# card path, source, mode
FILES=(
	"/usr/bin/pdoom build/pdoom 0100755"
	"/usr/bin/pocketwrite build/write 0100755"
	"/usr/bin/finder build/finder 0100755"
	"/usr/bin/pocketsplash build/pocketsplash 0100755"
	"/usr/bin/pocket-doom src/finder/pocket-doom 0100755"
	"/etc/init.d/pocket src/finder/S02pocket 0100755"
	"/etc/init.d/psplash src/finder/psplash 0100755"
	"/etc/init.d/pocket-done src/finder/pocket-done 0100755"
	"/usr/share/pocket/words.txt build/words.txt 0100644"
)
for b in pdoom-o3 pdoom-os pwolf pocketlua; do
	[ -x "build/$b" ] && FILES+=("/usr/bin/$b build/$b 0100755")
done
for f in build/art/*.img; do
	[ -e "$f" ] && FILES+=("/usr/share/pocket/art/$(basename "$f") $f 0100644")
done

for entry in "${FILES[@]}"; do
	read -r _ source _ <<< "$entry"
	[ -f "$source" ] || die "missing $source"
done

status=0
sudo e2fsck -fy "${DEV}2" > /dev/null 2>&1 || status=$?
[ "$status" -le 1 ] || die "the Linux partition needs attention, e2fsck said $status"

CMD=$(mktemp)
trap 'rm -f "$CMD"' EXIT
{
	# mkdir on a name that exists leaves a stray folder, so check first.
	for dir in /usr/share/pocket /usr/share/pocket/art; do
		sudo debugfs -R "stat $dir" "${DEV}2" 2>&1 | grep -q 'Type: directory' ||
			echo "mkdir $dir"
	done
	# The old splash client could wait on a splash that no longer runs.
	echo "rm /usr/bin/psplash-write"
	# Runs after the filesystems are unmounted and just before the halt.
	sudo debugfs -R "stat /etc/rc0.d/S85pocket-done" "${DEV}2" 2>&1 | grep -q 'Type: symlink' ||
		echo "symlink /etc/rc0.d/S85pocket-done ../init.d/pocket-done"
	echo "rm /usr/bin/pmenu"
	echo "rm /usr/bin/pocketlid"
	# The desktop stays running, so it must start after the system services.
	echo "rm /etc/rc5.d/S02pocket"
	# Nothing here needs apmd, and without it the power button sleeps directly.
	echo "rm /etc/rc5.d/S20apmd"
	sudo debugfs -R "stat /etc/rc5.d/S95pocket" "${DEV}2" 2>&1 | grep -q 'Type: symlink' ||
		echo "symlink /etc/rc5.d/S95pocket ../init.d/pocket"
	for entry in "${FILES[@]}"; do
		read -r target source mode <<< "$entry"
		echo "rm $target"
		echo "write $source $target"
		echo "sif $target uid 0"
		echo "sif $target gid 0"
		echo "sif $target mode $mode"
	done
} > "$CMD"
sudo debugfs -w -f "$CMD" "${DEV}2" > /dev/null 2>&1 || die "debugfs failed"
sync

sudo e2fsck -fn "${DEV}2" > /dev/null 2>&1 || die "the Linux partition failed its check"
for entry in "${FILES[@]}"; do
	read -r target source _ <<< "$entry"
	sudo debugfs -R "dump $target /dev/stdout" "${DEV}2" 2> /dev/null | cmp - "$source" ||
		die "$target does not match $source"
done

# Boot messages off: the kernel line in shlo.txt, on the FAT partition.
MNT=$(mktemp -d)
findmnt -n "${DEV}1" > /dev/null && sudo umount "${DEV}1"
sudo mount -t vfat -o uid="$(id -u)",gid="$(id -g)" "${DEV}1" "$MNT"
sed -i 's#^\(set string = 0x8c001100 mem=16M root=/dev/hda2\) *\(quiet\)\? *#\1 quiet#' "$MNT/shlo.txt"
grep -q 'mem=16M root=/dev/hda2 quiet' "$MNT/shlo.txt" || { sudo umount "$MNT"; die "could not edit shlo.txt"; }
mkdir -p "$MNT/stories"
# Wolfenstein's game files live only on the card and in the private folder.
if [ -d "$WOLF_DATA" ]; then
	mkdir -p "$MNT/wolf3d"
	for f in "$WOLF_DATA"/*.WL1; do
		n=$(basename "$f" | tr 'A-Z' 'a-z')
		cp "$f" "$MNT/wolf3d/$n"
		cmp "$f" "$MNT/wolf3d/$n" || { sudo umount "$MNT"; die "$n did not copy cleanly"; }
	done
fi
rm -f "$MNT/system/lid.log" "$MNT/system/lid.try" "$MNT/system/lid-off"
# Examples arrive once, so a changed copy is never overwritten.
mkdir -p "$MNT/code"
for f in src/code/examples/*.lua; do
	[ -e "$MNT/code/$(basename "$f")" ] || cp "$f" "$MNT/code/"
done
sync
sudo umount "$MNT"
rmdir "$MNT"
echo "UPDATED: ${#FILES[@]} files on the card match this build"
