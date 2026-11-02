#!/bin/bash
# Desktop tests, run on the PC with memory checking on. Stand-in programs record
# what the desktop tries to launch.
set -u

P=$(cd "$(dirname "$0")/.." && pwd)
BIN=$P/build/finder-host
T=$(mktemp -d)
cd "$T" || exit 1
export POCKET_ROOT=$T/root
export POCKET_ART=$T/no-art
export POCKET_BIN=$T/bin
export POCKET_FB_FILE=$T/fb
export ASAN_OPTIONS=detect_leaks=0
export LAUNCH_LOG=$T/log

mkdir -p bin
cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "write:$1" >> "$LAUNCH_LOG"
E
cat > bin/pocket-doom <<'E'
#!/bin/sh
echo "doom:$1" >> "$LAUNCH_LOG"
E
cat > bin/pdoom <<'E'
#!/bin/sh
echo "doom:$7" >> "$LAUNCH_LOG"
E
cat > bin/pwolf <<'E'
#!/bin/sh
echo "wolf:$2" >> "$LAUNCH_LOG"
E
chmod +x bin/*

passed=0
failed=0

check() {
	if [ "$2" = "$3" ]; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		echo "FAIL: $1"
		echo "  wanted: $3"
		echo "  got:    $2"
	fi
}

# The desktop starts with no window, so most tests open Pocket HD, then Stories.
run() {
	: > log
	{ echo 'K enter'; echo 'K enter'; cat keys; } > all-keys
	POCKET_KEYS=all-keys "$BIN" 2> err
	status=$?
	if grep -q 'Sanitizer\|runtime error' err; then
		failed=$((failed + 1))
		echo "FAIL: memory error"
		head -12 err
	fi
}

fresh() {
	rm -rf root
	mkdir -p root/stories root/doom root/wolf3d root/trash
	: > root/doom/Doomu.wad
	: > root/wolf3d/vswap.wl1
	: > "root/stories/Harbor Part 1.txt"
	: > root/stories/Tracks.txt
	: > root/stories/my-words.txt
	: > root/stories/Tracks.txt.bak
}

fresh
printf 'K enter\n' > keys
: > log
POCKET_KEYS=keys "$BIN" 2> err
check "no window opens by itself, so the first Enter launches nothing" "$(cat log)" ""

printf 'K enter\n' > keys
run
check "Enter opens the first story, spaces and all" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

printf 'K down\nK down\nK down\nK enter\n' > keys
run
check "the list stops at the last story, skipping other files" "$(cat log)" "write:$T/root/stories/Tracks.txt"

printf 'K right\nK down\nK down\nK down\nK enter\n' > keys
run
check "Games opens Doom at the chosen size" "$(cat log)" "doom:fit"

printf 'K right\nK down\nK down\nK down\nK down\nK enter\n' > keys
run
check "speed test is offered" "$(cat log)" "doom:bench"

printf 'K right\nK down\nK down\nK down\nK down\nK down\nK enter\n' > keys
mkdir -p root/wolf3d
run
check "Games opens Wolfenstein 3D" "$(cat log)" "wolf:box"
{ printf 'K right\n'; for i in 1 2 3 4 5 6 7 8; do echo 'K down'; done; echo 'K enter'; } > keys
run
check "Wolfenstein 3D opens at the chosen size" "$(cat log)" "wolf:fit"

printf 'K esc\nK down\nK enter\n' > keys
run
check "New Story from the File menu" "$(cat log)" "write:$T/root/stories/Story 1.txt"

: > "root/stories/Story 1.txt"
printf 'K esc\nK down\nK enter\n' > keys
run
check "New Story skips names in use" "$(cat log)" "write:$T/root/stories/Story 2.txt"

printf 'K esc\nK right\nK down\nK down\nK enter\n' > keys
run
check "Login Prompt exits with 0" "$status" "0"
printf 'K esc\nK right\nK down\nK down\nK down\nK enter\nK enter\n' > keys
run
check "Power Off exits with 10" "$status" "10"
check "nothing launches after Power Off" "$(cat log)" ""

fresh
printf 'K esc\nK esc\nK enter\n' > keys
run
check "Esc closes the menu" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

# Stylus: positions follow the calibration in touch.cal, so set an exact one.
fresh
mkdir -p root/system
echo '1 0 0 0 1 0 1' > root/system/touch.cal
printf 'P 100 84\nP 100 84\n' > keys
run
check "tap selects, second tap opens" "$(cat log)" "write:$T/root/stories/Tracks.txt"

printf 'P 100 84\n' > keys
run
check "one tap only selects" "$(cat log)" ""

printf 'P 570 40\nP 100 72\nP 100 72\nP 100 72\nP 100 72\n' > keys
run
check "tapping Pocket HD lists its folders" "$(cat log)" "doom:tall"

: > root/trash/Old.txt
printf 'P 570 180\nP 100 60\nP 100 60\nK esc\nK right\nK right\nK down\nK enter\nK enter\n' > keys
run
check "tapping the Trash opens the Trash" "$(ls root/stories | grep -c Old.txt)" "1"
rm root/stories/Old.txt

printf 'P 60 8\nP 60 37\nP 60 37\n' > keys
run
check "tapping outside the list does nothing" "$(cat log)" ""

printf 'P 16 8\nP 40 34\n' > keys
run
check "tapping a menu item chooses it" "$(cat log)" "write:$T/root/stories/Story 1.txt"

printf 'P 600 230\n' > keys
run
check "a tap on the empty desktop does nothing" "$(cat log)" ""

# Calibration: a touchscreen with swapped axes, turned, scaled and offset.
fresh
mkdir -p root/system
echo '1 0 0 0 1 0 1' > root/system/touch.cal
# File, Special, Calibrate, then raw taps for targets (40,30) (600,40) (320,210).
# Raw is rx = 900 - y*2, ry = 100 + x/2 rounded down, which swaps the axes.
{
	echo 'P 60 8'
	echo 'P 60 24'
	echo "P $((900 - 30 * 2)) $((100 + 40 / 2))"
	echo "P $((900 - 40 * 2)) $((100 + 600 / 2))"
	echo "P $((900 - 210 * 2)) $((100 + 320 / 2))"
} > keys
run
python3 -I - "$T/root/system/touch.cal" > cal-check <<'E'
import sys
a, b, c, d, e, f, g = map(int, open(sys.argv[1]).read().split())
worst = 0
for x, y in [(40, 30), (600, 40), (320, 210), (100, 100), (500, 200)]:
    rx, ry = 900 - y * 2, 100 + x // 2
    worst = max(worst, abs((a * rx + b * ry + c) / g - x), abs((d * rx + e * ry + f) / g - y))
print("ok" if worst <= 2 else f"off by {worst}")
E
check "calibration handles swapped axes" "$(cat cal-check)" "ok"

# Folders
fresh
printf 'K esc\nK down\nK down\nK enter\nT Field Notes\nK enter\nK enter\nK esc\nK down\nK enter\n' > keys
run
check "New Folder creates the folder" "$(ls -d root/stories/*/ | wc -l)" "1"
check "a new story lands inside the open folder" "$(cat log)" "write:$T/root/stories/Field Notes/Story 1.txt"

: > "root/stories/Field Notes/Inside.txt"
printf 'K enter\nK enter\n' > keys
run
check "folders list first, and open with Enter" "$(cat log)" "write:$T/root/stories/Field Notes/Inside.txt"

printf 'K enter\nK backspace\nK down\nK enter\n' > keys
run
check "Backspace leaves a folder" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

mkdir -p root/system
echo '1 0 0 0 1 0 1' > root/system/touch.cal
printf 'K enter\nP 16 30\nK down\nK enter\n' > keys
run
check "the close box leaves a folder" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

fresh
printf 'K esc\nK down\nK down\nK enter\nT a/b:c\nK enter\n' > keys
run
check "characters FAT forbids are ignored" "$(ls root/stories | grep -c '^abc$')" "1"

printf 'K esc\nK down\nK down\nK enter\nT abc\nK enter\n' > keys
run
check "a folder name already used is refused" "$(ls -d root/stories/*/ | wc -l)" "1"

printf 'K esc\nK down\nK down\nK enter\nT zzz\nK esc\n' > keys
run
check "Esc cancels New Folder" "$(ls root/stories | grep -c zzz)" "0"

printf 'K esc\nK down\nK down\nK enter\nT  \nK enter\nK esc\n' > keys
run
check "a blank name creates nothing" "$(ls -d root/stories/*/ | wc -l)" "1"

fresh
mkdir -p "root/stories/d1/d2"
: > root/stories/d1/d2/deep.txt
printf 'K enter\nK enter\nK enter\nK backspace\nK backspace\nK backspace\nK backspace\nK enter\n' > keys
run
check "folders inside folders" "$(sed -n 1p log)" "write:$T/root/stories/d1/d2/deep.txt"

printf 'K right\nK esc\nK down\nK down\nK enter\nT Made From Games\nK enter\n' > keys
run
check "New Folder from Games goes to Stories" "$(ls root/stories | grep -c 'Made From Games')" "1"

# Art-free picture is still drawn safely, with folders.

# Closing windows and desktop folders
fresh
mkdir -p root/system
echo '1 0 0 0 1 0 1' > root/system/touch.cal
printf 'K backspace\nK enter\nK enter\n' > keys
run
check "Backspace goes up to Pocket HD, Enter goes back in" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

printf 'P 20 30\nP 20 30\nP 100 72\nP 100 72\n' > keys
run
check "the close box goes up, then closes, and the list ignores taps" "$(cat log)" ""

printf 'P 20 30\nP 20 30\nP 570 40\nK enter\nK enter\n' > keys
run
check "tapping an icon reopens its window" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

printf 'K esc\nK down\nK down\nK down\nK enter\nK enter\nK enter\n' > keys
run
check "Close Window is in the File menu" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

printf 'K backspace\nK backspace\nK down\nK enter\nK backspace\nK up\nK enter\nK enter\nK enter\n' > keys
run
check "up and down pick a desktop icon" "$(cat log)" "write:$T/root/stories/Harbor Part 1.txt"

fresh
printf 'K backspace\nK esc\nK down\nK down\nK enter\nT Field Notes\nK enter\n' > keys
run
check "New Folder in Pocket HD keeps it with the other folders" "$(ls root/desktop)" "Field Notes"
: > "root/desktop/Field Notes/Inside.txt"

printf 'K backspace\nK down\nK down\nK down\nK enter\nK enter\n' > keys
run
check "folders in Pocket HD open like any other" "$(cat log)" "write:$T/root/desktop/Field Notes/Inside.txt"

printf 'K right\nK right\nK right\nK esc\nK down\nK enter\n' > keys
run
check "a new story lands in that folder" "$(cat log)" "write:$T/root/desktop/Field Notes/Story 1.txt"

printf 'K backspace\nK backspace\nK esc\nK down\nK down\nK enter\nT Second\nK enter\n' > keys
run
check "New Folder with the window closed goes in Pocket HD" "$(ls root/desktop | wc -l)" "2"

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do mkdir -p "root/desktop/Folder $i"; done
printf 'K backspace\nK esc\nK down\nK down\nK enter\nT Too Many\nK enter\n' > keys
run
check "a full Pocket HD refuses more folders" "$(ls root/desktop | grep -c 'Too Many')" "0"

# Rename, move and Trash. Item menu: Esc, right twice, then the row.
fresh
printf 'K esc\nK right\nK right\nK enter\nK backspace\nK backspace\nT New Name\nK enter\n' > keys
: > "root/stories/Tracks.txt.bak"
printf 'text' > "root/stories/Harbor Part 1.txt"
run
check_file() { [ -e "$1" ] && echo yes || echo no; }
check "rename keeps .txt and edits the old name" "$(check_file 'root/stories/Harbor Part New Name.txt')" "no"
check "renamed story has its text" "$(ls root/stories | grep -c 'New Name')" "1"

fresh
printf 'text' > root/stories/Tracks.txt
printf 'old' > root/stories/Tracks.txt.bak
printf 'K down\nK esc\nK right\nK right\nK enter\n' > keys
for i in 1 2 3 4 5 6; do echo 'K backspace' >> keys; done
printf 'T Trees\nK enter\n' >> keys
run
check "rename moves the story text" "$(cat root/stories/Trees.txt)" "text"
check "rename takes the backup along" "$(cat root/stories/Trees.txt.bak)" "old"

fresh
: > root/stories/Trees.txt
printf 'K down\nK esc\nK right\nK right\nK enter\n' > keys
for i in 1 2 3 4 5 6; do echo 'K backspace' >> keys; done
printf 'T Trees\nK enter\n' >> keys
run
check "rename refuses a name in use" "$(check_file root/stories/Tracks.txt)" "yes"

fresh
mkdir root/stories/Sub root/desktop
printf 'K down\nK down\nK esc\nK right\nK right\nK down\nK enter\nK down\nK down\nK enter\n' > keys
run
check "Move To moves a story to a folder" "$(check_file root/stories/Sub/Tracks.txt)" "yes"
check "and it leaves the old place" "$(check_file root/stories/Tracks.txt)" "no"

printf 'K esc\nK right\nK right\nK down\nK enter\nK down\nK enter\n' > keys
run
check "a folder cannot go inside itself" "$(check_file root/stories/Sub)" "yes"

fresh
printf 'text' > root/stories/Tracks.txt
printf 'old' > root/stories/Tracks.txt.bak
printf 'K down\nK esc\nK right\nK right\nK down\nK down\nK enter\n' > keys
run
check "Move to Trash keeps the story" "$(cat root/trash/Tracks.txt)" "text"
check "and its backup" "$(cat root/trash/Tracks.txt.bak)" "old"
check "and leaves Stories" "$(check_file root/stories/Tracks.txt)" "no"

printf 'x' > root/stories/Tracks.txt
printf 'K down\nK delete\n' > keys
run
check "a second story of that name stays separate" "$(cat 'root/trash/Tracks 2.txt')" "x"

printf 'K backspace\nK right\nK enter\n' > keys
run
check "stories in the Trash do not open" "$(cat log)" ""

printf 'K backspace\nK right\nK esc\nK right\nK right\nK down\nK enter\nK enter\n' > keys
run
check "Move To brings something back from the Trash" "$(check_file root/stories/Tracks.txt)" "yes"

printf 'K backspace\nK right\nK esc\nK right\nK right\nK down\nK down\nK enter\nK enter\n' > keys
run
check "Empty Trash starts on Cancel" "$(ls root/trash | wc -l)" "1"

mkdir -p root/trash/box/inner
: > root/trash/box/inner/file
printf 'K backspace\nK right\nK esc\nK right\nK right\nK down\nK down\nK enter\nK down\nK enter\n' > keys
run
check "Empty Trash deletes everything inside" "$(ls root/trash | wc -l)" "0"
check "and nothing else" "$(check_file root/stories/Tracks.txt)" "yes"

: > root/trash/keep.txt
printf 'K esc\nK right\nK right\nK down\nK down\nK enter\n' > keys
run
check "outside the Trash that row trashes the item, it never empties" "$(check_file root/trash/keep.txt)" "yes"

fresh
: > root/trash/keep.txt
printf 'K backspace\nK right\nK delete\n' > keys
run
check "Delete in the Trash never deletes for good" "$(check_file root/trash/keep.txt)" "yes"

fresh
printf 'K down\nS backspace\n' > keys
run
check "Shift+Backspace also trashes" "$(check_file root/trash/Tracks.txt)" "yes"

fresh
mkdir -p "root/desktop/Old Name"
: > "root/desktop/Old Name/Inside.txt"
printf 'K backspace\nK down\nK down\nK down\nK esc\nK right\nK right\nK enter\n' > keys
for i in 1 2 3 4 5 6 7 8; do echo 'K backspace' >> keys; done
printf 'T Renamed\nK enter\n' >> keys
run
check "a folder in Pocket HD can be renamed" "$(ls root/desktop)" "Renamed"
check "and keeps what is inside" "$(check_file root/desktop/Renamed/Inside.txt)" "yes"

printf 'K backspace\nK esc\nK right\nK right\nK enter\nK esc\n' > keys
run
check "Stories cannot be renamed" "$(ls root | grep -c stories)" "1"

printf 'K backspace\nK delete\nK down\nK delete\n' > keys
run
check "Stories cannot go in the Trash" "$(ls root/stories | wc -l)" "4"
check "and neither can Games" "$(ls root/trash | wc -l)" "0"

printf 'K backspace\nK backspace\nK delete\nK esc\nK right\nK right\nK enter\nK esc\n' > keys
run
check "the desktop icons have to stay" "$(ls root | tr '\n' ' ')" "desktop doom stories trash wolf3d "

printf 'K backspace\nK down\nK down\nK down\nK delete\n' > keys
run
check "a folder in Pocket HD can go in the Trash" "$(check_file root/trash/Renamed/Inside.txt)" "yes"
check "and leaves Pocket HD" "$(ls root/desktop | wc -l)" "0"

fresh
cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "open:$1" >> "$LAUNCH_LOG"
kill -STOP $$
E
printf 'K enter\nK delete\n' > keys
run
check "an open story cannot go in the Trash" "$(check_file 'root/stories/Harbor Part 1.txt')" "yes"
cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "write:$1" >> "$LAUNCH_LOG"
E

# Switching: stand-ins pause like the real programs, and keep the log honest.
fresh
cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "open:$1" >> "$LAUNCH_LOG"
kill -STOP $$
echo "back:$1" >> "$LAUNCH_LOG"
kill -STOP $$
echo "back2:$1" >> "$LAUNCH_LOG"
E
mkdir notes
export POCKET_TMP=$T/notes
cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "open:$1" >> "$LAUNCH_LOG"
printf '%s' "$POCKET_ROOT/stories/Tracks.txt" > "$POCKET_TMP/pocket-open.$$"
kill -STOP $$
echo "back:$1" >> "$LAUNCH_LOG"
E
fresh
printf 'K enter\nK down\nK enter\n' > keys
run
check "after Save As, the new name switches instead of opening twice" "$(cut -d: -f1 log | tr '\n' ' ')" "open back "
check "the Finder clears the note" "$(ls notes | wc -l)" "0"
unset POCKET_TMP
cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "open:$1" >> "$LAUNCH_LOG"
kill -STOP $$
echo "back:$1" >> "$LAUNCH_LOG"
kill -STOP $$
echo "back2:$1" >> "$LAUNCH_LOG"
E
S1="$T/root/stories/Harbor Part 1.txt"
S2="$T/root/stories/Tracks.txt"

printf 'K enter\nK down\nK enter\n' > keys
run
check "two stories can be open" "$(sed -n 1,2p log | cut -c1-5 | tr '\n' ' ')" "open: open: "

printf 'K enter\nK enter\n' > keys
run
check "opening a story twice switches back to it" "$(sed -n 1,2p log)" "open:$S1
back:$S1"

printf 'K enter\nK down\nK enter\nK tab\nK down\nK enter\n' > keys
run
check "the list brings back the chosen program" "$(sed -n 3p log)" "back:$S1"

printf 'K enter\nK tab\nK esc\nK enter\n' > keys
run
check "Esc leaves the list, story still open" "$(sed -n 2p log)" "back:$S1"

printf 'K tab\nK enter\n' > keys
run
check "with nothing open, Tab does nothing" "$(sed -n 1p log)" "open:$S1"

mkdir -p root/system
echo '1 0 0 0 1 0 1' > root/system/touch.cal
printf 'K enter\nP 600 8\nP 600 34\n' > keys
run
check "tapping Finder at the top right lists what is open" "$(sed -n 2p log)" "back:$S1"

fresh
printf 'K right\nK enter\n' > keys
run
check "Doom starts directly so it can be paused" "$(cat log)" "doom:box"

cat > bin/pocketwrite <<'E'
#!/bin/sh
echo "write:$1" >> "$LAUNCH_LOG"
E

# A lot of stories, and none at all.
fresh
for i in $(seq 1 240); do : > "root/stories/Long story name that goes on and on and on and on and on and on and on $i.txt"; done
{ for i in $(seq 1 260); do echo 'K down'; done; echo 'K enter'; } > keys
run
check "long lists scroll without trouble" "$(wc -l < log)" "1"

rm -rf root
mkdir root
printf 'K enter\nK right\nK left\nK esc\nK enter\n' > keys
run
check "missing card folders are created" "$(ls root | tr '\n' ' ')" "desktop stories trash "

mkfifo pipe
exec 3<> pipe
POCKET_KEYS=pipe "$BIN" 2> err 3>&- &
pid=$!
sleep 1
cp fb fb.before
dd if=/dev/zero of=fb bs=1024 count=64 conv=notrunc 2> /dev/null
kill -WINCH "$pid"
echo 'K home' >&3
sleep 0.5
cmp -s fb fb.before
woke=$?
exec 3>&-
wait "$pid"
check "the picture comes back after a sleep" "$woke" "0"

# Battery percentage in the menu bar.
fresh
apm() { printf '1.0 1.2 0x03 0x00 0x00 0x01 %s%% 0 min\n' "$1" > apm; }
export POCKET_APM=$T/apm
printf 'K home\n' > keys
rm -f apm
run
cp fb fb.none
apm 84
run
cp fb fb.84
apm 41
run
cmp -s fb fb.84
check "a different charge draws a different number" "$?" "1"
cmp -s fb.84 fb.none
check "the number is drawn" "$?" "1"
apm 100
run
cp fb fb.100
apm 130
run
cmp -s fb fb.100
check "a reading over 100 shows 100%" "$?" "0"
echo junk > apm
run
cmp -s fb fb.none
check "an unreadable battery shows nothing" "$?" "0"

# The stored picture changes once the desktop sees a new reading after a wake.
apm 84
mkfifo apm-pipe
exec 3<> apm-pipe
POCKET_KEYS=apm-pipe "$BIN" 2> err 3>&- &
pid=$!
sleep 1
apm 41
kill -WINCH "$pid"
echo 'K home' >&3
sleep 0.5
cmp -s fb fb.84
stale=$?
exec 3>&-
wait "$pid"
check "the percentage refreshes after a wake" "$stale" "1"
unset POCKET_APM

# Brightness. The dialog is the second item in the Special menu.
export POCKET_BACKLIGHT=$T/light
open_dialog() { printf 'K esc\nK right\nK down\nK enter\n'; }
light() {
	mkdir -p light
	echo "$1" > light/max_brightness
	echo "$2" > light/brightness
	rm -f root/system/brightness
}
level() { tr -d '\n' < light/brightness; }

fresh
light 255 10
{ open_dialog; printf 'K right\nK right\nK enter\n'; } > keys
run
check "Right raises the brightness" "$(level)" "40"
check "Enter saves the brightness" "$(cat root/system/brightness)" "40"

light 255 10
{ open_dialog; printf 'K right\nK right\nK esc\n'; } > keys
run
check "Esc puts the brightness back" "$(level)" "10"
check "Esc saves nothing" "$(ls root/system | grep -c brightness)" "0"

light 255 10
{ open_dialog; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 'K left'; done; echo 'K enter'; } > keys
run
check "the backlight never goes fully off" "$(level)" "1"

light 255 10
{ open_dialog; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 'K right'; done; echo 'K enter'; } > keys
run
check "brightness stops at the last useful step" "$(level)" "161"

light 8 4
{ open_dialog; printf 'K right\nK enter\n'; } > keys
run
check "a short range moves one level at a time" "$(level)" "5"
light 8 4
{ open_dialog; for i in 1 2 3 4 5 6 7 8 9; do echo 'K left'; done; echo 'K enter'; } > keys
run
check "a short range stops at 1" "$(level)" "1"

light 255 10
echo 90 > root/system/brightness
printf 'K home\n' > keys
run
check "the saved brightness returns at startup" "$(level)" "90"
echo 200 > root/system/brightness
echo 8 > light/max_brightness
run
check "a saved brightness over the top is limited" "$(level)" "8"
for junk in 0 -5 hello ''; do
	echo 7 > light/brightness
	echo "$junk" > root/system/brightness
	run
	check "bad saved brightness '$junk' is ignored" "$(level)" "7"
done

rm -rf light root/system
{ open_dialog; printf 'K enter\nK home\n'; } > keys
run
check "missing screen controls are survivable" "$status" "0"
check "missing screen controls save nothing" "$(ls root/system 2> /dev/null | grep -c brightness)" "0"

# Contrast, in the stand-in for the chip: control at byte 2, data at byte 18.
export POCKET_LCD_FILE=$T/lcd
lcd() {
	python3 -I -c "
import struct,sys
d=bytearray(4096)
struct.pack_into('<H',d,2,int(sys.argv[1],16))
struct.pack_into('<H',d,18,int(sys.argv[2],16))
open('lcd','wb').write(d)" "$1" "$2"
}
control() { od -An -tx2 -j2 -N2 lcd | tr -d ' '; }
data() { od -An -tx2 -j18 -N2 lcd | tr -d ' '; }
light 255 10
rm -rf root/system

lcd d5c1 0001
{ open_dialog; printf 'K down\nK right\nK enter\n'; } > keys
run
check "contrast moves one level" "$(control)" "f5c1"
check "contrast keeps the other pins" "$(data)" "0079"
check "contrast is saved after Enter" "$(cat root/system/contrast)" "9"
check "contrast leaves brightness alone" "$(level)" "10"
check "and does not save a brightness" "$(ls root/system | grep -c brightness)" "0"

lcd d5c1 0001
rm -rf root/system
{ open_dialog; printf 'K down\nK right\nK right\nK esc\n'; } > keys
run
check "Esc puts contrast back" "$(control) $(data)" "d5c1 0001"
check "Esc saves no contrast" "$(ls root/system 2> /dev/null | grep -c contrast)" "0"

lcd d5c1 0001
rm -rf root/system
{ open_dialog; printf 'K down\nK right\n'; for i in 1 2 3 4 5 6 7 8 9; do echo W; done; } > keys
run
check "contrast waits before going back" "$(control)" "d5c1"
{ open_dialog; printf 'K down\nK right\n'; for i in 1 2 3 4 5 6 7 8 9 10; do echo W; done; printf 'K enter\n'; } > keys
lcd d5c1 0001
run
check "contrast goes back by itself" "$(control) $(data)" "d5c1 0001"
check "and is not saved" "$(ls root/system 2> /dev/null | grep -c contrast)" "0"

lcd d5c1 0001
rm -rf root/system
{ open_dialog; printf 'K down\n'; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 'K right'; done; printf 'K enter\n'; } > keys
run
check "contrast stops at the blackest" "$(control)" "ffc1"
lcd d5c1 0001
rm -rf root/system
{ open_dialog; printf 'K down\n'; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 'K left'; done; printf 'K enter\n'; } > keys
run
check "contrast stops at the lightest" "$(control)" "d541"

lcd d5c1 0001
mkdir -p root/system
echo 12 > root/system/contrast
printf 'K home\n' > keys
run
check "the saved contrast returns at startup" "$(control)" "d7c1"
for junk in 16 -1 hello ''; do
	lcd d5c1 0001
	echo "$junk" > root/system/contrast
	run
	check "bad saved contrast '$junk' is ignored" "$(control)" "d5c1"
done

rm -rf light root/system
lcd d5c1 0001
{ open_dialog; printf 'K right\nK enter\n'; } > keys
run
check "contrast works with no backlight control" "$(control)" "f5c1"
unset POCKET_LCD_FILE POCKET_BACKLIGHT

# Without game files the launchers explain, and start nothing.
fresh
rm root/doom/Doomu.wad root/wolf3d/vswap.wl1
printf 'K right\nK enter\n' > keys
run
check "Doom without its WAD starts nothing" "$(cat log)" ""
: > root/doom/freedoom1.wad
run
check "Freedoom works in its place" "$(cat log)" "doom:box"
{ printf 'K right\n'; for i in 1 2 3 4 5; do echo 'K down'; done; printf 'K enter\n'; } > keys
run
check "Wolf 3D without its data starts nothing" "$(cat log)" ""

# The Code folder is the third item in Pocket HD.
fresh
printf 'K backspace\nK down\nK down\nK enter\nK esc\nK down\nK enter\n' > keys
run
check "New Story in Code makes a program" "$(cat log)" "write:$T/root/code/Program 1.lua"
mkdir -p root/code
: > "root/code/Ball.lua"
: > "root/code/Ball.lua.bak"
printf 'K right\nK right\nK enter\n' > keys
run
check "programs open in the editor" "$(cat log)" "write:$T/root/code/Ball.lua"
printf 'K right\nK right\nK esc\nK right\nK right\nK enter\n' > keys
for i in 1 2 3 4 5 6 7 8; do echo 'K backspace' >> keys; done
printf 'T Bounce\nK enter\n' >> keys
run
check "renaming keeps .lua" "$(ls root/code | tr '\n' ' ')" "Bounce.lua Bounce.lua.bak "
printf 'K right\nK right\nK delete\n' > keys
run
check "the Trash keeps .lua" "$(ls root/trash | tr '\n' ' ')" "Bounce.lua Bounce.lua.bak "
printf 'K backspace\nK right\nK esc\nK right\nK right\nK down\nK enter\nK down\nK enter\n' > keys
run
check "Move To can put it back in Code" "$(ls root/code | grep -c '^Bounce.lua$')" "1"

cd /
rm -rf "$T"
echo "passed $passed, failed $failed"
[ "$failed" -eq 0 ]
