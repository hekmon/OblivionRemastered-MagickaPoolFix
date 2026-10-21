#!/usr/bin/env bash
# Build MagickaPoolFix.asi, run the unit tests and generate the default .conf.
#
# Runs on Linux (or WSL) with:
#   - MinGW-w64, POSIX threads variant, for the plugin
#       Debian/Ubuntu: sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64
#   - any host C++20 compiler for the tests (g++ by default)
#   - python3, for the .conf generator and its checks
#
# The plugin embeds __DATE__ and __TIME__ so each log identifies its own binary,
# which makes every build unique: the checksum printed at the end changes on every
# build.
set -euo pipefail
cd "$(dirname "$0")"

CXXWIN="${CXXWIN:-x86_64-w64-mingw32-g++-posix}"
OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
CXXHOST="${CXXHOST:-g++}"
WARN="-Wall -Wextra -Wpedantic"

mkdir -p build

echo "== unit tests (host)"
for t in math calibration corrector; do
    "$CXXHOST" -std=c++20 $WARN -I src -o "build/test_$t" "tests/test_$t.cpp"
    "./build/test_$t"
done

echo
echo "== plugin (win64)"
"$CXXWIN" -std=c++20 -O2 -s $WARN -shared -municode -I src \
    -o build/MagickaPoolFix.asi src/Plugin.cpp \
    -static -static-libgcc -static-libstdc++ -lpsapi \
    -Wl,--exclude-all-symbols

echo
echo "== default .conf"
python3 tools/gen_conf.py src/Config.h build/MagickaPoolFix.conf

echo
echo "== result"
file build/MagickaPoolFix.asi
imports=$("$OBJDUMP" -p build/MagickaPoolFix.asi | awk '/DLL Name:/ {print $3}' | sort -f | tr '\n' ' ')
echo "imports: $imports"
for dll in $imports; do
    case "${dll,,}" in
        kernel32.dll|msvcrt.dll|user32.dll) ;;
        *) echo "FAIL: unexpected import $dll (only KERNEL32, msvcrt, USER32 are allowed)"; exit 1 ;;
    esac
done
sha256sum build/MagickaPoolFix.asi build/MagickaPoolFix.conf
