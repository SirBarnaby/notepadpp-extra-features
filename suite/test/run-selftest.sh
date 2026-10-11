#!/bin/bash
# Runs the KEX suite's built-in self-test in a KEX Notepad++ under Wine + Xvfb and collects the log
# and screenshots.
#
#   suite/test/run-selftest.sh NPP_DIR [OUT_DIR]
#
# NPP_DIR  portable KEX Notepad++ to test in (its notepad++.exe has the suite built in)
# OUT_DIR  where screenshots go (default: suite/test/screenshots); the log is also copied to suite/test/
# Uses its own WINEPREFIX (~/.wine-kexsuite) and X display (:88).
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NPP=$1
SHOTS=${2:-$HERE/test/screenshots}
export WINEDEBUG=-all WINEPREFIX=$HOME/.wine-kexsuite DISPLAY=:88
TESTDIR=$WINEPREFIX/drive_c/kexsuite-test
WTESTDIR='C:\kexsuite-test'
LOG=$TESTDIR/selftest-log.txt

mkdir -p "$SHOTS"
rm -rf "$NPP/plugins/KexSuite"   # the old plugin form of the suite (Notepad++ KEX skips it anyway)
rm -f "$NPP/kex.ini" "$NPP/kex.log" "$NPP/kex-quicknote.txt" "$NPP/plugins/Config/KexSuite.ini" "$NPP/plugins/Config/KexQuickNote.txt"
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
cp "$NPP/kex.ini" "$HERE/test/selftest-kex.ini" 2>/dev/null
grep -a -E '^(PASS|FAIL|SKIP|RESULT)' "$LOG" | tail -n 200
