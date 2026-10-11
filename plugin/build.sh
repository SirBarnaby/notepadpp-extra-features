#!/bin/bash
# Builds KexSuite.dll (x64) with MinGW-w64 on Linux.
# Usage: ./build.sh            -> build/KexSuite.dll
# Headers: include/npp (vendored from Notepad++ v8.9.8.1, commit 5ee7a76) and ../include/KexApi.h
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build"
mkdir -p "$OUT"
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
"$WINDRES" -I"$HERE/src" "$HERE/src/KexSuite.rc" -O coff -o "$OUT/KexSuite.res.o"
"$CXX" -std=c++17 -O2 -Wall -Wno-unknown-pragmas -Wno-misleading-indentation -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
    -I"$HERE/src" -I"$HERE/include/npp" -I"$HERE/../include" \
    -shared -static -static-libgcc -static-libstdc++ -s \
    -o "$OUT/KexSuite.dll" "$HERE"/src/*.cpp "$OUT/KexSuite.res.o" \
    -Wl,--enable-stdcall-fixup -luser32 -lgdi32 -lcomctl32 -lole32 -luuid -ladvapi32 -lshell32 -ldwmapi
echo "built $OUT/KexSuite.dll ($(stat -c %s "$OUT/KexSuite.dll") bytes)"
