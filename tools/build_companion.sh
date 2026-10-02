#!/bin/bash
# Builds ALVR PS4 Tracking Viewer (companion/) as a portable Windows .exe with MinGW-w64, in
# WSL: wsl -d Debian -- bash tools/build_companion.sh
# (needs: sudo apt install g++-mingw-w64-x86-64). Output: companion/build/ALVR PS4 Tracking Viewer.exe
set -e
cd "$(dirname "$0")/.."
OUT=companion/build
mkdir -p "$OUT"
x86_64-w64-mingw32-windres -I companion companion/trackview.rc -O coff -o "$OUT/trackview_res.o"
x86_64-w64-mingw32-g++ -O2 -std=c++17 -Wall -municode -mwindows -static -s \
    -I client/src -I companion \
    companion/trackview.cpp client/src/lobby.cpp "$OUT/trackview_res.o" \
    -lws2_32 -lcomctl32 -lcomdlg32 -lshell32 -o "$OUT/ALVR PS4 Tracking Viewer.exe"
echo "built $OUT/ALVR PS4 Tracking Viewer.exe"
