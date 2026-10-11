#!/bin/bash
# tests/run-tests.sh NPP_DIR SRC_DIR OUT_DIR
# Runs both test suites under Wine + Xvfb on a copy of a packaged KEX Notepad++:
#   1. KexTest (tests/KexTest.cpp): the four-view core patch, 30 checks incl. mouse drags
#   2. the KEX Suite plugin's built-in self-test (plugin/test/run-selftest.sh)
# SRC_DIR is the patched Notepad++ source tree (for headers). Exits non-zero if any check fails.
# Needs wine64, xvfb, xdotool, imagemagick and MinGW-w64.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PKG=$1; SRC=$2; OUT=$(mkdir -p "$3" && cd "$3" && pwd)
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml="   # no Mono/Gecko install prompts
NPP=$OUT/npp; rm -rf "$NPP"; cp -r "$PKG" "$NPP"; rm -rf "$NPP/plugins/KexSuite"

S=$SRC/PowerEditor/src
mkdir -p "$NPP/plugins/KexTest"
x86_64-w64-mingw32-g++-posix -std=c++20 -O2 -shared -municode -DUNICODE -D_UNICODE -w \
  -I"$S" -I"$S/MISC/PluginsManager" -I"$SRC/scintilla/include" "$ROOT/tests/KexTest.cpp" \
  -static -lcomctl32 -o "$NPP/plugins/KexTest/KexTest.dll" || exit 1

# --- 1. core test ---
export WINEPREFIX=$HOME/.wine-npp
wineboot -i >/dev/null 2>&1; wineserver -w
D=$WINEPREFIX/drive_c/kextest; LOG=$D/log.txt; rm -rf "$D"; mkdir -p "$D" "$OUT/core"
for f in a b c d e; do printf "This is file %s.\r\nLine two of %s.\r\n" $f $f > "$D/$f.txt"; done
Xvfb :77 -screen 0 1280x800x24 >/dev/null 2>&1 & XP=$!; export DISPLAY=:77; sleep 2
NPPWIN="Z:$(echo "$NPP" | sed 's#/#\\#g')\\notepad++.exe"
(cd "$NPP" && wine explorer /desktop=kex,1280x800 "$NPPWIN" -nosession >/dev/null 2>&1) &
for i in $(seq 1 480); do
  sleep 0.5
  [ -f "$LOG" ] || continue
  for n in $(grep -ao '^SHOT [0-9]*' "$LOG" | awk '{print $2}'); do
    [ -f "$D/ack$n" ] && continue
    sleep 0.8; import -display :77 -window root "$OUT/core/shot$n.png"; touch "$D/ack$n"
  done
  grep -a '^DRAG ' "$LOG" | tr -d '\r' | while read -r _ n x1 y1 x2 y2; do
    [ -f "$D/ack$n" ] && continue
    sleep 0.6
    xdotool mousemove $x1 $y1 mousedown 1 sleep 0.3 mousemove $((x1+3)) $y1 sleep 0.1 mousemove $((x1+6)) $y1 sleep 0.1 mousemove $((x1+9)) $y1 sleep 0.1 mousemove $((x1+12)) $y1 sleep 0.2 mousemove $(((x1+x2)/2)) $(((y1+y2)/2)) sleep 0.2 mousemove $x2 $y2 sleep 0.3 mouseup 1
    sleep 0.8; touch "$D/ack$n"
  done
  grep -aq '^DONE' "$LOG" && break
done
wineserver -k 2>/dev/null; kill $XP 2>/dev/null; wait $XP 2>/dev/null
tr -d '\r' < "$LOG" > "$OUT/core/log.txt" 2>/dev/null
grep -a -E '^(FAIL|RESULT)' "$OUT/core/log.txt"
grep -aq '^RESULT 0 failure' "$OUT/core/log.txt"; CORE=$?

# --- 2. plugin self-test ---
rm -rf "$NPP/plugins/KexTest"
WINEPREFIX=$HOME/.wine-kexsuite wineboot -i >/dev/null 2>&1; WINEPREFIX=$HOME/.wine-kexsuite wineserver -w
mkdir -p "$OUT/plugin"
bash "$ROOT/plugin/test/run-selftest.sh" "$NPP" "$OUT/plugin" > "$OUT/plugin/result.txt" 2>&1
cp "$HOME/.wine-kexsuite/drive_c/kexsuite-test/selftest-log.txt" "$OUT/plugin/log.txt" 2>/dev/null
grep -a -E '^(FAIL|RESULT)' "$OUT/plugin/result.txt"
grep -aq '^RESULT [0-9]* passed, 0 failed' "$OUT/plugin/result.txt"; PLUG=$?

echo "core test: $([ $CORE = 0 ] && echo pass || echo FAIL)   plugin self-test: $([ $PLUG = 0 ] && echo pass || echo FAIL)"
[ $CORE = 0 ] && [ $PLUG = 0 ]
