#!/bin/sh
# Runs unchanged on Linux CI or in the disposable Linux VM. No root required.
set -eu
B=$(cd "$1" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/sd-app-tests.XXXXXX")
overlay=
cleanup() {
    if [ -n "$overlay" ]; then kill "$overlay" 2>/dev/null || :; wait "$overlay" 2>/dev/null || :; fi
    rm -rf "$T"
}
trap cleanup EXIT
mkdir -p "$T/apps"
cp -R "$B/apps/hello" "$T/apps/hello"
R="$B/sd_app_runner"
passed=0
expect() {
    wanted=$1; label=$2; shift 2
    if "$@" >"$T/out" 2>"$T/err"; then actual=0; else actual=$?; fi
    if [ "$actual" -ne "$wanted" ]; then
        echo "FAIL: $label (expected $wanted, got $actual)"
        cat "$T/out" "$T/err"
        exit 1
    fi
    passed=$((passed+1))
    echo "PASS: $label"
}
run() { "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" "$@"; }
wait_until() {
    n=0
    while ! "$@" 2>/dev/null; do
        n=$((n+1)); [ "$n" -lt 100 ] || { echo "FAIL: timed out waiting for: $*"; exit 1; }
        sleep 0.1
    done
}
# Console form of the example (no "display"): these tests run without an overlay.
manifest() {
    cat >"$T/apps/hello/app.json" <<EOF
{"schema":1,"id":"hello","name":"Hello Goggles","version":"0.1.0","entry":"bin/hello","args":$1}
EOF
}
display_manifest() {
    cat >"$T/apps/hello/app.json" <<EOF
{"schema":1,"id":"hello","name":"Hello Goggles","version":"0.1.0","entry":"bin/hello","args":$1,"display":true}
EOF
}
restore() {
    manifest '[]'
    cp "$B/apps/hello/bin/hello" "$T/apps/hello/bin/hello"
}
restore

expect 0 'SHA-256 known vectors' "$B/hash_check"
expect 0 'valid static executable' run validate hello
expect 0 'catalog discovery' run list
grep -q '"id":"hello"' "$T/out"
mkdir -p "$T/search-only/apps"
cp -R "$B/apps/hello" "$T/search-only/apps/hello"
cp "$R" "$T/runner"
chmod 755 "$T" "$T/runner" "$T/search-only/apps" "$T/search-only/apps/hello" \
    "$T/search-only/apps/hello/bin" "$T/search-only/apps/hello/assets"
chmod 644 "$T/search-only/apps/hello/app.json" "$T/search-only/apps/hello/bin/hello" \
    "$T/search-only/apps/hello/assets/message.txt"
chmod 111 "$T/search-only"
expect 0 'root traversal needs search permission only' "$B/fixture" unprivileged \
    "$T/runner" --apps "$T/search-only/apps" validate hello
chmod 700 "$T" "$T/search-only"
expect 0 'stage package' run stage hello
staged=$(cat "$T/out")
[ -x "$staged/bin/hello" ] && [ ! -x "$staged/assets/message.txt" ]
expect 0 'reuse identical cache' run stage hello
[ "$staged" = "$(cat "$T/out")" ]
expect 0 'run and save data' run run hello
grep -q 'Hello Goggles completed.' "$T/cache/logs/last-run.log"
grep -q "came from the app's package" "$T/cache/logs/last-run.log"
# The app is told which folder it was staged from (it runs from the cache copy).
grep -qx "source=$(cd "$T/apps/hello" && pwd -P)" "$T/cache/logs/last-run.log"
grep -qx 'Loaded from internal storage' "$T/cache/logs/last-run.log"
[ -f "$T/data/hello/launches.txt" ]
expect 0 'clear inherited preload and library paths' env LD_PRELOAD=/no-such-library LD_LIBRARY_PATH=/no-such-path \
    "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" run hello
manifest '["$(touch SHOULD_NOT_EXIST); *", "two words", "quote\"slash\\", "\ud83e\udd7d"]'
expect 0 'literal argv including shell metacharacters and Unicode' run run hello
grep -Fq '$(touch SHOULD_NOT_EXIST); *' "$T/cache/logs/last-run.log"
[ ! -e SHOULD_NOT_EXIST ] && [ ! -e "$staged/SHOULD_NOT_EXIST" ]

cat >"$T/apps/hello/app.json" <<'EOF'
{"schema":1,"id":"hello","id":"hello","name":"x","version":"1","entry":"bin/hello"}
EOF
expect 125 'duplicate manifest field rejected' run validate hello
manifest '[]'
sed 's/"schema":1/"schema":2/' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'unknown schema rejected' run validate hello
manifest '[]'
sed 's|bin/hello|../hello|' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'traversal entry rejected' run validate hello
manifest '[]'
sed 's|bin/hello|/bin/hello|' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'absolute entry rejected' run validate hello
manifest '[]'
sed 's/"id":"hello"/"id":"other"/' "$T/apps/hello/app.json" >"$T/change"
cp "$T/change" "$T/apps/hello/app.json"
expect 125 'directory and manifest ids must match' run validate hello
manifest '["\u0000"]'
expect 125 'embedded NUL rejected' run validate hello
manifest '["\ud800"]'
expect 125 'unpaired surrogate rejected' run validate hello
printf '{"schema":1}' >"$T/apps/hello/app.json"
expect 125 'missing fields rejected' run validate hello
restore
printf '\003\000' | dd of="$T/apps/hello/bin/hello" bs=1 seek=18 conv=notrunc 2>/dev/null
expect 125 'wrong ELF architecture rejected' run validate hello
restore
printf '\003\000\000\000' | dd of="$T/apps/hello/bin/hello" bs=1 seek=64 conv=notrunc 2>/dev/null
expect 125 'ELF interpreter rejected' run validate hello
head -c 20 "$B/apps/hello/bin/hello" >"$T/apps/hello/bin/hello"
expect 125 'truncated ELF rejected' run validate hello
restore
ln -s /etc/passwd "$T/apps/hello/assets/link"
expect 125 'symlink asset rejected' run validate hello
rm "$T/apps/hello/assets/link"
ln "$T/apps/hello/assets/message.txt" "$T/apps/hello/assets/hardlink"
expect 125 'hard-linked file rejected' run validate hello
rm "$T/apps/hello/assets/hardlink"
mkfifo "$T/apps/hello/assets/fifo"
expect 125 'FIFO rejected without blocking' run validate hello
rm "$T/apps/hello/assets/fifo"
truncate -s 67108864 "$T/apps/hello/assets/oversize"
expect 125 'package byte budget enforced' run validate hello
rm "$T/apps/hello/assets/oversize"
ln -s "$T/apps" "$T/source-link"
expect 125 'symlink in source root rejected' "$R" --apps "$T/source-link" validate hello
expect 125 'unsafe selection rejected' run stage ../hello
expect 125 'source/cache overlap rejected' "$R" --apps "$T/apps" --cache "$T/apps/cache" stage hello
expect 125 'missing SD directory reported' "$R" --apps "$T/missing" list

restore
expect 0 'find baseline cache' run stage hello
staged=$(cat "$T/out")
printf 'corrupt' >"$staged/assets/message.txt"
expect 125 'modified cache never executed' run run hello
expect 0 'clean cache only' run clean hello
[ -f "$T/data/hello/launches.txt" ] && [ ! -d "$staged" ]
mkdir -p "$T/cache/packages/hello/.partial-interrupted/assets" "$T/cache/packages/hello/keep-me"
echo partial >"$T/cache/packages/hello/.partial-interrupted/assets/file"
expect 0 'interrupted staging removed before retry' run stage hello
[ ! -d "$T/cache/packages/hello/.partial-interrupted" ]
expect 0 'cleanup preserves unrelated directories' run clean hello
[ -d "$T/cache/packages/hello/keep-me" ]
truncate -s 268435457 "$T/cache/packages/too-large"
expect 125 'total cache byte budget enforced' run stage hello
rm "$T/cache/packages/too-large"

cp "$B/fixture" "$T/apps/hello/bin/hello"
manifest '["exit7"]'
expect 7 'app exit status preserved' run run hello
manifest '["crash"]'
expect 139 'app crash reported' run run hello
manifest '["ignoreterm"]'
expect 124 'hung app escalates to kill after timeout' run --timeout 1 run hello
manifest '["closeout"]'
expect 124 'closed output does not prevent timeout' run --timeout 1 run hello
manifest '["flood"]'
expect 0 'large output drained without deadlock' run run hello
[ "$(wc -c <"$T/cache/logs/last-run.log")" -eq 1048576 ]
manifest '["child"]'
expect 0 'child process group cleaned on exit' run run hello
child=$(cat "$T/data/hello/child.pid")
if [ -e "/proc/$child/stat" ]; then grep -q ' Z ' "/proc/$child/stat"; fi
manifest '["sleep"]'
"$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" run hello >"$T/background.out" 2>"$T/background.err" &
leader=$!
sleep 1
expect 125 'concurrent runner blocked' run run hello
expect 125 'cleanup blocked while app is running' run clean hello
kill -TERM "$leader"
expect 143 'runner cancellation exits after terminating app' wait "$leader"
restore
expect 0 'launch still works after crash and cancellation' run run hello
expect 0 'repeat launch' run run hello

# ---- Menu listing (read by mod_sd_apps.so while dji_glasses starts) ----
expect 0 'menu listing' run menu-list
[ "$(cat "$T/out")" = "$(printf 'hello\t0.1.0\tHello Goggles')" ]
cat >"$T/apps/hello/app.json" <<'EOF'
{"schema":1,"id":"hello","name":"Café 🥽","version":"1.0","entry":"bin/hello"}
EOF
expect 0 'menu labels reduced to ASCII' run menu-list
[ "$(cat "$T/out")" = "$(printf 'hello\t1.0\tCaf? ?')" ]
mkdir -p "$T/apps/broken"
echo '{' >"$T/apps/broken/app.json"
expect 0 'invalid package left out of menu listing' run menu-list
if grep -q broken "$T/out"; then echo "FAIL: broken package listed"; exit 1; fi
grep -q 'skip broken' "$T/err"
rm -r "$T/apps/broken"
restore

# ---- mod_sd_apps.so logic: listing -> menu entries -> launch command ----
mod_check() { (cd "$T" && "$B/mod_check" "$@"); }
cat >"$T/sd-runner" <<EOF
#!/bin/sh
exec "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" "\$@"
EOF
chmod 755 "$T/sd-runner"
expect 0 'SD Apps mod registers one entry per app' mod_check
[ "$(cat "$T/out")" = "$(printf 'sd:hello\tHello Goggles\tSD card app 0.1.0\t300\tnohup ./sd-runner run hello >/dev/null 2>>/dev/null &')" ]
before=$(wc -l <"$T/data/hello/launches.txt")
(cd "$T" && sh -c "$(cut -f5 "$T/out")")
launched() { [ "$(wc -l <"$T/data/hello/launches.txt")" -gt "$before" ]; }
wait_until launched
wait_until "$R" --cache "$T/cache" clean hello   # succeeds once that runner has exited
expect 0 'registered menu command launches the app' launched
cat >"$T/sd-runner" <<'EOF'
#!/bin/sh
printf 'doom\t1\tNot Doom\n'
printf 'bad;id\t1\tEvil\n'
printf 'Upper\t1\tEvil\n'
printf 'two\ttabs\tin\tname\n'
printf 'ok\t2\tGood\n'
printf 'cut\t1\tNo newline'
EOF
expect 0 'SD Apps mod skips unsafe listing lines' mod_check
[ "$(cut -f1 "$T/out" | tr '\n' ' ')" = "sd:doom sd:ok " ]
cat >"$T/sd-runner" <<'EOF'
#!/bin/sh
i=0; while [ $i -lt 30 ]; do printf 'app%d\t1\tApp %d\n' $i $i; i=$((i+1)); done
EOF
expect 0 'SD Apps mod caps its entries' mod_check
[ "$(wc -l <"$T/out")" -eq 24 ]
expect 0 'SD Apps mod stops when the menu is full' mod_check 3
[ "$(wc -l <"$T/out")" -eq 3 ]
grep -q 'menu full' "$T/err"
rm "$T/sd-runner"
expect 0 'SD Apps mod without a runner registers nothing' mod_check
[ ! -s "$T/out" ]

# ---- Descriptors: a runner started from dji_glasses must not leak its files ----
cp "$B/fixture" "$T/apps/hello/bin/hello"
manifest '["fds"]'
exec 8</dev/null
expect 0 'inherited descriptors closed before launch' run run hello
exec 8<&-
restore

# ---- Display apps: overlay hand-off (fb_override / fb_shared / fb_stop) ----
D="$T/display"
mkdir -p "$D"
mkfifo "$T/keys"
exec 7<>"$T/keys"   # keeps the FIFO open, so apps never see end-of-input
# --timeout turns a regression (app never stopped) into exit 124 instead of a hang.
drun() { "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" --display-dir "$D" --input "$T/keys" --timeout 20 "$@"; }
key() { "$B/fixture" key "$1" "$2" >&7; }
drawn() { c=$(od -An -tu4 -N4 "$D/fb_shared" | tr -d ' '); [ -n "$c" ] && [ "$c" -gt 0 ]; }
released() { [ ! -e "$D/fb_override" ] && [ ! -e "$D/fb_shared" ] && [ ! -e "$D/fb_stop" ] && [ ! -e "$D/cleaning" ]; }
start_overlay() { rm -f "$D/attached"; "$B/overlay_fixture" "$D" "${1:-0}" & overlay=$!; }
stop_overlay() { kill "$overlay"; wait "$overlay"; overlay=; }
start_display() {
    # Direct exec matters: $app must be the runner, not a shell wrapping it.
    "$R" --apps "$T/apps" --cache "$T/cache" --data "$T/data" \
        --display-dir "$D" --input "$T/keys" --timeout 20 run hello >"$T/display.out" 2>"$T/display.err" &
    app=$!
}

cp "$B/apps/hello/app.json" "$T/apps/hello/app.json"
expect 0 'example manifest is a display app' run validate hello
grep -q '"display": true' "$T/apps/hello/app.json"
cat >"$T/apps/hello/app.json" <<'EOF'
{"schema":1,"id":"hello","name":"Hello Goggles","version":"0.1.0","entry":"bin/hello","display":"yes"}
EOF
expect 125 'display flag must be boolean' run validate hello
display_manifest '[]'
before=$(wc -l <"$T/data/hello/launches.txt")
expect 125 'missing overlay refuses display app before launch' drun run hello
grep -q 'overlay did not acknowledge' "$T/err"
[ "$(wc -l <"$T/data/hello/launches.txt")" -eq "$before" ]
released

# An old frame acknowledgement cannot acknowledge a new frame inode.
truncate -s 16588864 "$D/fb_shared"
printf 'NATSD01\000' | dd of="$D/fb_shared" bs=1 seek=8 conv=notrunc 2>/dev/null
printf '\001\000\000\000' | dd of="$D/fb_shared" bs=1 seek=16 conv=notrunc 2>/dev/null
touch "$D/fb_override"
expect 125 'stale acknowledgement does not bypass overlay handshake' drun run hello
grep -q 'overlay did not acknowledge' "$T/err"
[ "$(wc -l <"$T/data/hello/launches.txt")" -eq "$before" ]
released

start_overlay 1
expect 125 'overlay without display buffers cannot start the app' drun run hello
[ "$(wc -l <"$T/data/hello/launches.txt")" -eq "$before" ]
released
start_display
wait_until test -e "$D/attached"
kill -TERM "$app"
expect 143 'cancelling the overlay handshake never starts the app' wait "$app"
[ "$(wc -l <"$T/data/hello/launches.txt")" -eq "$before" ]
released
stop_overlay
start_overlay

# The overlay still supports the zero-filled legacy header, and the SD
# runner must not reclaim a legacy app merely because it has no owner lease.
truncate -s 16588864 "$D/fb_shared"
touch "$D/fb_override"
wait_until test -e "$D/attached"
expect 125 'legacy display session remains active and cannot be reclaimed' drun run hello
[ -e "$D/fb_override" ] && [ -e "$D/fb_shared" ]
touch "$D/fb_stop"
wait_until released
drun run hello >"$T/display.out" 2>"$T/display.err" &
app=$!
wait_until test -f "$D/fb_override"
wait_until drawn
[ "$(wc -c <"$D/fb_shared")" -eq 16588864 ]
[ "$(wc -l <"$T/data/hello/launches.txt")" -eq $((before + 1)) ]
[ ! -e "$D/cleaning" ]
expect 125 'display owner lease covers runners using a different cache' "$R" \
    --apps "$T/apps" --cache "$T/other-cache" --data "$T/data" --display-dir "$D" run hello
grep -q 'another display app runner is active' "$T/err"
key 28 1; key 28 0       # 5D press: colour change, redraw
key 158 1; key 158 0     # BACK: Hello exits on release
expect 0 'display app exits on BACK and returns the screen' wait "$app"
released
grep -q 'presses=1' "$T/cache/logs/last-run.log"

touch "$D/fb_override"
expect 125 'display app refused while another app owns the screen' drun run hello
[ -f "$D/fb_override" ]
[ ! -e "$D/fb_shared" ]
rm "$D/fb_override"

drun run hello >"$T/display.out" 2>"$T/display.err" &
app=$!
wait_until drawn
touch "$D/fb_stop"
expect 143 'overlay stop request ends display app' wait "$app"
released

drun run hello >"$T/display.out" 2>"$T/display.err" &
app=$!
wait_until drawn
rm -f "$D/fb_override" "$D/fb_shared"   # what the overlay's cleanup does
expect 143 'overlay cleanup ends display app' wait "$app"
released

cp "$B/fixture" "$T/apps/hello/bin/hello"
display_manifest '["ignoreterm"]'
rm -f "$T/data/hello/self.pid"
drun run hello >"$T/display.out" 2>"$T/display.err" &
app=$!
wait_until test -f "$T/data/hello/self.pid"
wait_until test -f "$D/fb_override"
touch "$D/fb_stop"
expect 143 'unresponsive display app killed after stop request' wait "$app"
released
pid=$(cat "$T/data/hello/self.pid")
if [ -e "/proc/$pid/stat" ]; then grep -q ' Z ' "/proc/$pid/stat"; fi
display_manifest '["crash"]'
expect 139 'crashed display app returns the screen' drun run hello
released

display_manifest '["sleep"]'
rm -f "$T/data/hello/self.pid"
start_display
wait_until test -f "$T/data/hello/self.pid"
pid=$(cat "$T/data/hello/self.pid")
kill -KILL "$app"
expect 137 'runner killed during display app' wait "$app"
wait_until released
if [ -e "/proc/$pid/stat" ]; then grep -q ' Z ' "/proc/$pid/stat"; fi
passed=$((passed+1)); echo 'PASS: overlay reclaims ownership when the runner dies'

# Kill both participants so no process is left to clean the flags. The next
# runner may reclaim this recognised SD session, but must do a fresh handshake.
rm -f "$T/data/hello/self.pid"
start_display
wait_until test -f "$T/data/hello/self.pid"
kill -STOP "$app"
kill -KILL "$overlay"
wait "$overlay" 2>/dev/null || :
overlay=
kill -KILL "$app"
expect 137 'runner and overlay killed without cleanup' wait "$app"
[ -e "$D/fb_override" ] && [ -e "$D/fb_shared" ]
start_overlay
cp "$B/apps/hello/bin/hello" "$T/apps/hello/bin/hello"
display_manifest '[]'
start_display
wait_until drawn
key 158 1; key 158 0
expect 0 'next launch recovers stale display ownership' wait "$app"
released

cp "$B/fixture" "$T/apps/hello/bin/hello"
display_manifest '["sleep"]'
rm -f "$T/data/hello/self.pid"
start_display
wait_until test -f "$T/data/hello/self.pid"
kill -KILL "$overlay"
wait "$overlay" 2>/dev/null || :
overlay=
expect 143 'overlay death stops the display app and releases ownership' wait "$app"
released
exec 7<&-
restore

expect 0 'final cleanup' run clean hello
[ -f "$T/data/hello/launches.txt" ]
echo "SD_APPS_TESTS_PASSED=$passed"
