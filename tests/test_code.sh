#!/bin/bash
# Pocket Code's runner, on the PC with memory checking on.
set -u

P=$(cd "$(dirname "$0")/.." && pwd)
LUA=$P/build/pocketlua-host
T=$(mktemp -d)
cd "$T" || exit 1
export POCKET_FB_FILE=$T/fb
export POCKET_ROOT=$T/root
export ASAN_OPTIONS=detect_leaks=0
mkdir root

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

# When the script of keys ends the program stops, so some tests need a long, quiet one.
quiet() {
	for i in $(seq 1 3000); do echo W; done > keys
}

# Runs p.lua, with the keys in the file named keys.
run() {
	POCKET_KEYS=keys POCKET_LINE_FD=9 timeout 30 "$LUA" p.lua 2> err 9> line
	status=$?
	if grep -q 'Sanitizer' err; then
		failed=$((failed + 1))
		echo "FAIL: memory error"
		head -12 err
	fi
}

: > keys
cat > p.lua <<'L'
local f = io.open("out", "w")
f:write(WIDTH, " ", HEIGHT, " ", 7 + 8, " ", 2 ^ 3, " ", math.type(1))
f:close()
L
run
check "a program runs" "$(cat out)" "640 240 15 8 integer"
check "a clean finish reports no problem" "$status" "0"

quiet
cat > p.lua <<'L'
cls(RED)
fill(-50, -50, 2000, 2000, BLUE)
pixel(-1, -1, RED)
pixel(9999, 9999, RED)
line(-100, -100, 700, 300, GREEN)
rect(630, 230, 50, 50, GREEN)
text(-20, 230, "off the edge of the screen and past it", WHITE)
text(5, 5, 12345)
print(("long words "):rep(100))
for i = 1, 40 do print(i) end
show()
local f = io.open("out", "w") f:write("drew") f:close()
L
run
check "drawing off the screen is safe" "$(cat out)" "drew"

cat > p.lua <<'L'
fill(0, 0, 640, 240, rgb(255, 0, 0))
show()
L
run
check "drawing reaches the screen" "$(od -An -tx2 -j2000 -N2 fb | tr -d ' ')" "f800"

quiet
{ printf 'K up\nT a\nP 30 40\nK enter\n'; cat keys; } > keys2
mv keys2 keys
cat > p.lua <<'L'
local f = io.open("out", "w")
f:write(waitkey(), " ", waitkey(), " ")
local name, x, y = waitkey()
f:write(name, " ", x, " ", y, " ")
f:write(tostring(key()), " ", tostring(key()))
f:close()
L
run
check "keys and taps arrive" "$(cat out)" "up a tap 30 40 enter nil"

printf 'K esc\n' > keys
cat > p.lua <<'L'
while true do end
L
run
check "Esc stops an endless loop" "$status" "0"

: > keys
run
check "the end of input stops an endless loop too" "$status" "0"

printf 'K esc\nK enter\n' > keys
cat > p.lua <<'L'
while true do pcall(function() while true do end end) end
L
run
check "pcall cannot catch Esc forever" "$status" "0"

printf 'K enter\n' > keys
quiet
cat > p.lua <<'L'
local a = 1


local b = a + nil
L
run
check "an error names its line" "$(cat line)" "4"
check "an error reports failure" "$status" "1"

cat > p.lua <<'L'
local a = 1
if then
L
: > line
run
check "a typing mistake names its line" "$(cat line)" "2"

cat > p.lua <<'L'
local t = {}
while true do t[#t + 1] = ("x"):rep(100000) .. #t end
L
: > line
run
check "too much memory is an error, not a crash" "$status" "1"

cat > p.lua <<'L'
local function f() return 1 + f() end
f()
L
run
check "endless recursion is an error, not a crash" "$status" "1"

cat > p.lua <<'L'
line(0, 0, 100000000, 100000000)
L
run
check "a giant line cannot hang" "$status" "1"

for f in "$P"/src/code/examples/*.lua; do
	cp "$f" p.lua
	printf 'K enter\nK esc\nK enter\n' > keys
	: > line
	run
	check "$(basename "$f") runs" "$(cat line)" ""
done

cd /
rm -rf "$T"
echo "passed $passed, failed $failed"
[ "$failed" -eq 0 ]
