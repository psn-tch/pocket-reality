#!/bin/bash
# Pocket Write tests, run on the PC with memory checking on.
# Each test drives the real program with a key script, then checks the files.
set -u

P=$(cd "$(dirname "$0")/.." && pwd)
BIN=$P/build/write-host
export POCKET_DICT=$P/build/words.txt
T=$(mktemp -d)
export POCKET_FB_FILE=$T/fb
export ASAN_OPTIONS=detect_leaks=0
cd "$T" || exit 1

passed=0
failed=0

check() {
	if [ "$2" = "$3" ]; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		echo "FAIL: $1"
		echo "  wanted: $(printf %s "$3" | head -c 200)"
		echo "  got:    $(printf %s "$2" | head -c 200)"
	fi
}

check_true() {
	if eval "$2"; then passed=$((passed + 1)); else failed=$((failed + 1)); echo "FAIL: $1"; fi
}

run() {
	POCKET_KEYS=keys "$BIN" "$@" 2> err
	local rc=$?
	if [ $rc -ne 0 ] || grep -q 'Sanitizer\|runtime error' err; then
		failed=$((failed + 1))
		echo "FAIL: program error, exit $rc, running $*"
		head -12 err
	fi
}

fresh() {
	rm -rf work
	mkdir work
}

# Saving
fresh
printf 'T hello world\nC s\n' > keys
run work/a.txt
check "save writes the text" "$(cat work/a.txt)" "hello world"
check_true "no temp file left" "[ ! -e work/a.txt.tmp ]"
check_true "no autosave left" "[ ! -e work/a.txt.autosave ]"

printf 'K end\nT  again\nC s\n' > keys
run work/a.txt
check "second save" "$(cat work/a.txt)" "hello world again"
check "previous version kept" "$(cat work/a.txt.bak)" "hello world"

printf 'T x\nC q\nK enter\n' > keys
run work/a.txt
check "closing and choosing Save saves" "$(cat work/a.txt)" "xhello world again"

# Power loss: the script ends with no save, as if the battery died.
fresh
printf 'T first draft\nC s\nT  and a new unsaved line\n' > keys
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 'W 500' >> keys; done
run work/b.txt
check "story untouched before saving" "$(cat work/b.txt)" "first draft"
check "autosave holds the new work" "$(cat work/b.txt.autosave)" "first draft and a new unsaved line"

printf 'C s\n' > keys
run work/b.txt
check "autosave is recovered on open" "$(cat work/b.txt)" "first draft and a new unsaved line"
check_true "recovered autosave is cleared after saving" "[ ! -e work/b.txt.autosave ]"

# Power loss inside a save: only the temp copy or the backup survived.
fresh
printf 'tmp copy' > work/c.txt.tmp
printf 'C s\n' > keys
run work/c.txt
check "story rebuilt from temp file" "$(cat work/c.txt)" "tmp copy"

fresh
printf 'bak copy' > work/d.txt.bak
printf 'C s\n' > keys
run work/d.txt
check "story rebuilt from backup" "$(cat work/d.txt)" "bak copy"

# A failed save must never damage the story or quit.
if [ "$(id -u)" != 0 ]; then
	fresh
	printf 'precious' > work/e.txt
	chmod 555 work
	printf 'T more\nC q\nK enter\nT still running\n' > keys
	run work/e.txt
	chmod 755 work
	check "failed save leaves the story alone" "$(cat work/e.txt)" "precious"
fi

# Editing
fresh
printf 'T one two three\nK backspace\nK backspace\nC z\nC z\nC s\n' > keys
run work/f.txt
check "undo restores both deletions" "$(cat work/f.txt)" "one two three"

fresh
printf 'T one two three\nK backspace\nC z\nC z\nC s\n' > keys
run work/f.txt
check "undo removes the typed word as one step" "$(cat work/f.txt)" "one two"

fresh
printf 'T cut me out\nK home\nS right\nS right\nS right\nS right\nC x\nK end\nC v\nC s\n' > keys
run work/g.txt
check "cut and paste" "$(cat work/g.txt)" "me outcut "

fresh
printf 'T replace all of this\nC a\nT gone\nC s\n' > keys
run work/h.txt
check "typing over a selection" "$(cat work/h.txt)" "gone"

fresh
printf 'T find the needle here\nK home\nC f\nT NEEDLE\nK enter\nT pin\nC s\n' > keys
run work/i.txt
check "find ignores case and selects" "$(cat work/i.txt)" "find the pin here"

fresh
printf 'T ab\nK enter\nT cd\nK up\nK delete\nC s\n' > keys
run work/j.txt
check "delete at the end of a line joins it to the next" "$(cat work/j.txt)" "abcd"

# Windows line endings become plain ones.
fresh
printf 'one\r\ntwo\r\n' > work/k.txt
printf 'C s\n' > keys
run work/k.txt
check "CR LF cleaned" "$(od -An -c work/k.txt | tr -d ' \n')" 'one\ntwo\n'

# Spelling: names added once stay added.
fresh
printf 'T Hermione\nC d\nC s\n' > keys
run work/m.txt
check "word added to my-words.txt" "$(cat work/my-words.txt)" "hermione"

# Menus and suggestions
fresh
printf 'T menu saved\nK esc\nK enter\nT  not saved\n' > keys
run work/p.txt
check "File menu saves" "$(cat work/p.txt)" "menu saved"

fresh
printf 'T copy me\nC a\nK esc\nK right\nK down\nK down\nK enter\nK end\nK esc\nK right\nK down\nK down\nK down\nK enter\nC s\n' > keys
run work/q.txt
check "Edit menu copies and pastes" "$(cat work/q.txt)" "copy mecopy me"

fresh
printf 'T start\nK esc\nK left\nK left\nK left\nK left\nK esc\nC s\n' > keys
run work/r.txt
check "Esc leaves the menu without acting" "$(cat work/r.txt)" "start"

fresh
printf 'T teh cat\nK home\nC l\nK enter\nC s\n' > keys
run work/s.txt
check "suggestion fixes a swapped word" "$(cat work/s.txt)" "the cat"

fresh
printf 'T Teh cat\nK home\nC l\nK enter\nC s\n' > keys
run work/t.txt
check "suggestion keeps the capital" "$(cat work/t.txt)" "The cat"

fresh
printf 'T the libary door\nK home\nK right\nK right\nK right\nK right\nK right\nC l\nK esc\nC s\n' > keys
run work/u.txt
check "Esc keeps the misspelling" "$(cat work/u.txt)" "the libary door"

fresh
printf 'T the libary\nK home\nC l\nC s\n' > keys
run work/v.txt
check "correct word offers nothing" "$(cat work/v.txt)" "the libary"

fresh
printf 'T libary\nC l\nK enter\nC z\nC s\n' > keys
run work/w.txt
check "a fix can be undone" "$(cat work/w.txt)" "libary"

# One dictionary for all folders.
fresh
mkdir -p work/stories/sub
printf 'T Hermione\nC d\nC s\n' > keys
POCKET_ROOT=$T/work run "$T/work/stories/sub/x.txt"
check "words go in the shared list" "$(cat work/stories/my-words.txt)" "hermione"

# Switching away: unsaved work is autosaved, and power off saves it.
fresh
printf 'T paused work\nC w\nT  more\n' > keys
POCKET_SWITCHER=1 POCKET_KEYS=keys "$BIN" work/x.txt 2> err &
pid=$!
for i in $(seq 1 100); do
	[ "$(awk '{print $3}' /proc/$pid/stat 2>/dev/null)" = T ] && break
	sleep 0.1
done
check "program pauses itself" "$(awk '{print $3}' /proc/$pid/stat)" "T"
check "autosaved before leaving" "$(cat work/x.txt.autosave)" "paused work"
kill -TERM $pid
kill -CONT $pid
wait $pid
check "power off saves paused work" "$(cat work/x.txt)" "paused work"

fresh
printf 'T no finder\nC w\nC s\n' > keys
run work/y.txt
check "without the Finder, carries on" "$(cat work/y.txt)" "no finder"

# The blinking cursor only touches its own pixels.
fresh
printf 'T blink test line\nK left\nK left\n' > keys
run work/b1.txt
cp fb fb-still
printf 'W 500\nW 500\n' >> keys
rm -f work/b1.txt
run work/b1.txt
check_true "two blinks leave the screen as it was" "cmp -s fb fb-still"

# Closing and Save As
fresh
printf 'keep' > work/z.txt
printf 'T changed \nC q\nK down\nK enter\n' > keys
run work/z.txt
check "Don't Save leaves the story alone" "$(cat work/z.txt)" "keep"
check_true "Don't Save leaves no autosave" "[ ! -e work/z.txt.autosave ]"

printf 'T changed \nC q\nK down\nK down\nK enter\nT more \nC s\n' > keys
run work/z.txt
check "Cancel goes back to the story" "$(cat work/z.txt)" "changed more keep"

fresh
printf 'keep' > work/z.txt
printf 'T unwritten\nC q\nK enter\n' > keys
printf 'C q\nT typed after closing\n' > keys
run work/z.txt
check "closing with nothing changed asks nothing" "$(cat work/z.txt)" "keep"

mkdir -p work/system
fresh
printf 'first' > work/old.txt
printf 'C e\nK backspace\nK backspace\nK backspace\nT new\nK enter\nT x \nC s\n' > keys
run work/old.txt
check "Save As leaves the original" "$(cat work/old.txt)" "first"
check "Save As writes the new name, then keeps using it" "$(cat work/new.txt)" "x first"

fresh
printf 'one' > work/a.txt
printf 'two' > work/b.txt
printf 'C e\nK backspace\nT b\nK enter\nC s\n' > keys
run work/a.txt
check "Save As refuses a name in use" "$(cat work/b.txt)" "two"

fresh
printf 'one' > work/a.txt
printf 'C e\nK esc\nC s\n' > keys
run work/a.txt
check "Esc cancels Save As" "$(ls work | tr '\n' ' ')" "a.txt a.txt.bak "

# Long stories, wide lines, an undo log overflow and an empty file.
fresh
python3 -I -c "
import random
random.seed(1)
w=['the','dragon','castle','wizard','quietly','walked','toward','an','enormous','door']
print('\n'.join(' '.join(random.choice(w) for _ in range(random.randint(0,90))) for _ in range(3000)))
print('x'*2000)
" > work/n.txt
size=$(stat -c %s work/n.txt)
{ for i in $(seq 1 400); do echo 'K pagedown'; echo 'T a '; done; for i in $(seq 1 300); do echo 'C z'; done; echo 'C s'; } > keys
run work/n.txt
check_true "long story still about the same size" "[ \$(stat -c %s work/n.txt) -gt $((size - 2000)) ]"

fresh
: > work/o.txt
printf 'K backspace\nK delete\nK up\nK down\nK left\nK right\nK home\nK end\nC z\nC v\nC a\nC x\nC s\n' > keys
run work/o.txt
check "keys on an empty story are harmless" "$(cat work/o.txt)" ""


# Passwords
QEMU=$P/tools/root/usr/bin/qemu-sh4-static

lock_it() {
	printf 'C p\nT %s\nK enter\nT %s\nK enter\n' "$2" "$2" > keys
	run "$1"
}

# Prints a locked story by removing its password on a throwaway copy.
read_locked() {
	cp "$1" work/peek.txt
	rm -f work/peek.txt.*
	printf 'T %s\nK enter\nC p\nK down\nK enter\nT %s\nK enter\n' "$2" "$2" > keys
	run work/peek.txt
	cat work/peek.txt
	rm -f work/peek.txt work/peek.txt.*
}

flip_byte() {
	printf '\xff' | dd of="$1" bs=1 seek="$2" conv=notrunc 2> /dev/null
}

fresh
printf 'T secret words\nC s\nT  and more\nC s\n' > keys
run work/a.txt
check_true "a plain copy exists before locking" "grep -q secret work/a.txt.bak"
lock_it work/a.txt hunter2
check "a locked story starts with the marker" "$(head -c 8 work/a.txt)" "PKTLOCK1"
check_true "the words are not in the locked file" "! grep -q secret work/a.txt"
check "the old plain copies are gone" "$(ls work)" "a.txt"
check "the right password opens it" "$(read_locked work/a.txt hunter2)" "secret words and more"

cp work/a.txt before
printf 'T wrong\nK enter\nK esc\n' > keys
run work/a.txt
cmp -s work/a.txt before
check "a wrong password then Esc changes nothing" "$?" "0"
check "and leaves nothing behind" "$(ls work)" "a.txt"

printf 'T nope\nK enter\nT hunter2\nK enter\nK end\nT !\nC s\n' > keys
run work/a.txt
check "a wrong password can be retried" "$(read_locked work/a.txt hunter2)" "secret words and more!"
check_true "the backup copy is locked too" "[ \"$(head -c 8 work/a.txt.bak)\" = PKTLOCK1 ]"
check_true "the backup hides the words" "! grep -q secret work/a.txt.bak"

printf 'T hunter2\nK enter\nK end\nT  draft\n' > keys
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 'W 500' >> keys; done
run work/a.txt
check_true "an autosave of a locked story is locked" "[ \"$(head -c 8 work/a.txt.autosave)\" = PKTLOCK1 ]"
check_true "the autosave hides the words" "! grep -q draft work/a.txt.autosave"
check "a locked autosave is recovered" "$(cp work/a.txt.autosave work/b.txt; read_locked work/b.txt hunter2; rm -f work/b.txt)" "secret words and more! draft"

cp work/a.txt.autosave saved-autosave
flip_byte work/a.txt.autosave 200
printf 'T hunter2\nK enter\nC s\n' > keys
run work/a.txt
check "a damaged autosave falls back to the story" "$(read_locked work/a.txt hunter2)" "secret words and more!"

cp saved-autosave work/a.txt.autosave
flip_byte work/a.txt.autosave 33
printf 'T hunter2\nK enter\nC s\n' > keys
run work/a.txt
check "a damaged password check is not mistaken for a wrong password" "$(read_locked work/a.txt hunter2)" "secret words and more!"

rm -f work/a.txt.*
cp work/a.txt before
flip_byte work/a.txt 150
cp work/a.txt damaged
printf 'T hunter2\nK enter\nK enter\n' > keys
run work/a.txt
cmp -s work/a.txt damaged
check "a damaged only copy is never written over" "$?" "0"
cp before work/a.txt

printf 'plain old words' > work/a.txt.bak
cp work/a.txt before
printf 'T nope\nK enter\nC s\nK esc\n' > keys
run work/a.txt
cmp -s work/a.txt before
check "a wrong password cannot reach an old plain copy" "$?" "0"
rm -f work/a.txt.*

printf 'T hunter2\nK enter\nC p\nK enter\nT nope\nK enter\nC s\n' > keys
run work/a.txt
check "a wrong current password changes nothing" "$(read_locked work/a.txt hunter2)" "secret words and more!"
rm -f work/a.txt.*

printf 'T hunter2\nK enter\nC p\nK enter\nT hunter2\nK enter\nT swordfish\nK enter\nT swordfish\nK enter\n' > keys
run work/a.txt
check "a changed password works" "$(read_locked work/a.txt swordfish)" "secret words and more!"
check "copies under the old password are gone" "$(ls work)" "a.txt"
cp work/a.txt before
printf 'T hunter2\nK enter\nK esc\n' > keys
run work/a.txt
cmp -s work/a.txt before
check "the old password stops working" "$?" "0"

printf 'T swordfish\nK enter\nC s\n' > keys
run work/a.txt
first=$(cksum < work/a.txt)
printf 'T swordfish\nK enter\nC s\n' > keys
run work/a.txt
check_true "saving the same words encrypts them differently" "[ \"$first\" != \"$(cksum < work/a.txt)\" ]"

printf 'T swordfish\nK enter\nC p\nK down\nK enter\nT swordfish\nK enter\n' > keys
run work/a.txt
check "removing the password leaves plain text" "$(cat work/a.txt)" "secret words and more!"
check "no locked copy stays behind" "$(ls work)" "a.txt"

printf 'C p\nT one\nK enter\nT two\nK enter\n' > keys
run work/a.txt
check "a mistyped confirmation leaves the story plain" "$(cat work/a.txt)" "secret words and more!"
printf 'C p\nK esc\n' > keys
run work/a.txt
check "Esc cancels setting a password" "$(cat work/a.txt)" "secret words and more!"

lock_it work/a.txt 'sp ace&*(~'
printf 'T sp ace&*(~\nK enter\nC e\nK backspace\nT 2\nK enter\n' > keys
run work/a.txt
check "Save As keeps the password" "$(head -c 8 work/2.txt)" "PKTLOCK1"

if [ -x "$QEMU" ]; then
	fresh
	printf 'T made on the pc\nC s\n' > keys
	run work/a.txt
	lock_it work/a.txt shared
	printf 'T shared\nK enter\nK end\nT , changed on sh3\nC s\n' > keys
	POCKET_KEYS=keys "$QEMU" "$P/build/write" work/a.txt 2> err
	check "the Jornada build opens and saves a PC locked file" "$(read_locked work/a.txt shared)" "made on the pc, changed on sh3"
	lock_it work/b.txt x
	printf 'C p\nT fromsh3\nK enter\nT fromsh3\nK enter\n' > keys
	printf 'T words on sh3\nC s\n' | cat - keys > keys2
	POCKET_KEYS=keys2 "$QEMU" "$P/build/write" work/c.txt 2> err
	check "the PC build opens a Jornada locked file" "$(read_locked work/c.txt fromsh3)" "words on sh3"
fi

mkdir light
echo 255 > light/max_brightness
echo 10 > light/brightness
printf 'T bright\nC b\nK right\nK enter\nC s\n' > keys
POCKET_BACKLIGHT=$T/light POCKET_ROOT=$T/root POCKET_KEYS=keys "$BIN" work/bright.txt 2> err
check "Ctrl+B changes the brightness while writing" "$(tr -d '\n' < light/brightness)" "25"
check "the story is untouched by the brightness box" "$(cat work/bright.txt)" "bright"

# Save As tells the Finder, and a program stays a program.
mkdir notes
printf 'x\n' > w1.txt
printf 'C e\n' > keys
for i in 1 2 3 4 5 6; do echo 'K backspace' >> keys; done
printf 'T renamed\nK enter\n' >> keys
POCKET_TMP=$T/notes POCKET_SWITCHER=1 run w1.txt
check "Save As leaves a note for the Finder" "$(sed "s#.*/##" notes/pocket-open.* 2> /dev/null)" "renamed.txt"
rm -f notes/*

# Pocket Code's editor, with a stand-in for the program runner.
export POCKET_BIN=$T/bin
mkdir bin
cat > bin/pocketlua <<'L'
#!/bin/sh
cp "$1" ran.lua
echo ran >> runs
[ -z "$LINE" ] || echo "$LINE" >&"$POCKET_LINE_FD"
L
chmod +x bin/pocketlua

printf 'T print(1)\nC r\n' > keys
run a.lua
check "Ctrl+R saves, then runs" "$(cat ran.lua)" "print(1)"

printf 'T x = 1\nC r\nC r\n' > keys
run b.lua
check "it can run again" "$(wc -l < runs)" "3"

rm -f runs
printf 'T words\nC r\nC s\n' > keys
run c.txt
check "stories do not run" "$(ls | grep -c runs)" "0"

printf 'T if x then\nK enter\nT     y()\nK enter\nT z()\nC s\n' > keys
run d.lua
check "new lines keep the indent" "$(tail -1 d.lua)" "    z()"

printf 'one\ntwo\nthree\nfour\n' > e.lua
printf 'C r\nT X\nC s\n' > keys
LINE=3 run e.lua
check "a problem puts the cursor on its line" "$(sed -n 3p e.lua)" "Xthree"

rm -f runs
printf 'C p\nT sekrit\nK enter\nT sekrit\nK enter\nC r\nC s\n' > keys
run f.lua
check "a locked program does not run" "$(ls | grep -c runs)" "0"

printf 'T hello\nC r\n' > keys
POCKET_BIN=$T/nothing-here run g.lua
check "a missing runner is survivable" "$(cat g.lua)" "hello"

printf 'print(1)\n' > s.lua
printf 'C e\n' > keys
for i in 1 2 3 4 5 6; do echo 'K backspace' >> keys; done
printf 'T second\nK enter\nC r\n' >> keys
run s.lua
check "Save As keeps a program a program" "$(cat ran.lua)" "print(1)"
check "and keeps .lua" "$(ls | grep -c '^second.lua$')" "1"

printf 'T helo wrld\nC l\nC d\nC s\n' > keys
run h.lua
check "spelling is off in code" "$(cat h.lua)" "helo wrld"
unset POCKET_BIN

cd /
rm -rf "$T"
echo "passed $passed, failed $failed"
[ "$failed" -eq 0 ]
