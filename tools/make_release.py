"""Build the GitHub release files into release/:

  - ALVR-PS4-v<version>.pkg: the stable PS4 app (build it first:
    wsl -d Debian -- bash tools/build.sh VARIANT=stable);
  - ALVR-PS4-Setup.bat: the PC setup as one double-clickable file (also copied into the
    installed streamer folder as ALVR-PS4-Reset.bat). It is a batch/PowerShell
    polyglot: cmd runs the header, which starts PowerShell on the same file; PowerShell sees
    the header as a <# block comment #>. pc-setup/setup.ps1 is embedded with the settings
    template (pc-setup/alvr-ps4-session.json) as gzip + base64 and the SteamVR status icons
    (pc-setup/icons, made by tools/icons/make_icons.py) as base64.

Usage: python tools/make_release.py [--bat-only]
  --bat-only: only rebuild ALVR-PS4-Setup.bat (no stable pkg needed).
"""
import base64
import gzip
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "release")

HEADER = """<# : ALVR PS4 - PC setup. Double-click this file (it asks for administrator rights).
@echo off
rem Batch part: runs the PowerShell part below (this whole file, as PowerShell).
rem Optional argument: install folder (default: the folder of this file).
set "ALVR_PS4_BAT=%~f0"
set "ALVR_PS4_DIR=%~1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Invoke-Expression ([IO.File]::ReadAllText($env:ALVR_PS4_BAT))"
exit /b
#>
"""


def gz64(data):
    return base64.b64encode(gzip.compress(data, 9, mtime=0)).decode("ascii")


def main():
    version = re.search(r'#define ALVR_PS4_VERSION "([^"]+)"',
                        open(os.path.join(ROOT, "client", "src", "main.cpp"), encoding="utf-8").read()).group(1)
    os.makedirs(OUT, exist_ok=True)

    template = open(os.path.join(ROOT, "pc-setup", "alvr-ps4-session.json"), "rb").read()
    script = open(os.path.join(ROOT, "pc-setup", "setup.ps1"), encoding="utf-8").read()
    assert script.count("@@SESSION_TEMPLATE@@") == 1 and script.count("@@ICONS@@") == 1
    script = script.replace("@@SESSION_TEMPLATE@@", gz64(template))
    icon_dir = os.path.join(ROOT, "pc-setup", "icons")
    icons = "|".join(f"{n}:{base64.b64encode(open(os.path.join(icon_dir, n), 'rb').read()).decode('ascii')}"
                     for n in sorted(os.listdir(icon_dir)))
    script = script.replace("@@ICONS@@", icons)
    text = HEADER + script
    text.encode("ascii")  # cmd and Windows PowerShell 5 read the file as ANSI
    bat = os.path.join(OUT, "ALVR-PS4-Setup.bat")
    with open(bat, "w", encoding="ascii", newline="\r\n") as f:
        f.write(text)

    if "--bat-only" in sys.argv:
        print(f"{os.path.relpath(bat, ROOT)}  {os.path.getsize(bat)} bytes")
        return

    pkg_name = "IV0000-ALVR00001_00-ALVRPS4CLIENT000.pkg"
    pkg = os.path.join(ROOT, "client", pkg_name)
    out_pkg = os.path.join(OUT, f"ALVR-PS4-v{version}.pkg")
    shutil.copyfile(pkg, out_pkg)

    for p in (out_pkg, bat):
        print(f"{os.path.relpath(p, ROOT)}  {os.path.getsize(p)} bytes")


main()
