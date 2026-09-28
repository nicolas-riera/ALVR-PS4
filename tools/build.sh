#!/usr/bin/env bash
# Build the PS4 client .pkg. Run from Windows as:  wsl -d Debian -- bash tools/build.sh
set -euo pipefail
export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/ps4/OpenOrbis/PS4Toolchain}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
# PkgTool.Core (.NET Core 3) needs OpenSSL 1.1, absent from Debian 13: use a local copy.
export LD_LIBRARY_PATH="$HOME/ps4/libssl11/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$(dirname "$0")/../client"
make "$@"
ls -la ./*.pkg
