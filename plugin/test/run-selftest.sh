#!/bin/bash
# Runs the KEX Suite self-test in Notepad++ under Wine + Xvfb and collects the log and screenshots.
#
#   test/run-selftest.sh [NPP_DIR] [OUT_DIR]
#
# NPP_DIR  portable Notepad++ to test in (default ~/nppt-kex; the plugin DLL is copied into it)
# OUT_DIR  where screenshots and the log go (default: this plugin folder's screenshots/ and test/)
# Uses its own WINEPREFIX (~/.wine-kexsuite) and X display (:88).
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NPP=${1:-$HOME/nppt-kex}
SHOTS=${2:-$HERE/screenshots}
export WINEDEBUG=-all WINEPREFIX=$HOME/.wine-kexsuite DISPLAY=:88
TESTDIR=$WINEPREFIX/drive_c/kexsuite-test
WTESTDIR='C:\kexsuite-test'
LOG=$TESTDIR/selftest-log.txt

[ -f "$HERE/build/KexSuite.dll" ] || "$HERE/build.sh"
mkdir -p "$NPP/plugins/KexSuite" "$SHOTS"
cp "$HERE/build/KexSuite.dll" "$NPP/plugins/KexSuite/KexSuite.dll"
rm -f "$NPP/plugins/Config/KexSuite.ini" "$NPP/plugins/Config/KexSuite.log"
rm -rf "$TESTDIR"; mkdir -p "$TESTDIR"
rm -f "$SHOTS"/*.png

Xvfb :88 -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!
sleep 2
NPPWIN="Z:$(echo "$NPP" | sed 's#/#\\#g')\\notepad++.exe"
(cd "$NPP" && wine explorer /desktop=kexs,1280x800 "$NPPWIN" -nosession "-pluginMessage=KexSuite_selftest=1;KexSuite_testdir=$WTESTDIR" >/dev/null 2>&1) &

for i in $(seq 1 1500); do
  sleep 0.2
  [ -f "$LOG" ] || continue
  while read -r _ n name; do
    [ -f "$TESTDIR/ack$n" ] && continue
    sleep 0.15
    f=$(printf "%s/%02d-%s.png" "$SHOTS" "$n" "$name")
    import -display :88 -window root "$f"
    touch "$TESTDIR/ack$n"
  done < <(grep -a '^SHOT ' "$LOG" | tr -d '\r')
  grep -aq '^DONE' "$LOG" && break
done
sleep 0.5
wineserver -k 2>/dev/null
kill $XPID 2>/dev/null
cp "$LOG" "$HERE/test/selftest-log.txt" 2>/dev/null
cp "$NPP/plugins/Config/KexSuite.ini" "$HERE/test/selftest-KexSuite.ini" 2>/dev/null
grep -a -E '^(PASS|FAIL|SKIP|RESULT)' "$LOG" | tail -n 200
