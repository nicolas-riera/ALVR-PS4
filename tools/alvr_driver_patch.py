"""Patch ALVR 20.14.1's SteamVR driver so the controller "menu" button stops also pressing "system".

In driver_alvr_server.dll (alvr/server_openvr/cpp/alvr_server/Paths.cpp, lines 91 and 168),
/user/hand/*/input/menu/click is hard-wired to two SteamVR components:
{"/input/system/click", "/input/application_menu/click"}. The PS4 client's menu buttons
(left SQUARE, right TRIANGLE) therefore also opened the SteamVR dashboard.

Each list is built on the stack as two pointers, [rbp-9] = system and [rbp-1] =
application_menu, and passed as the range [rbp-9, rbp+7). Starting the range at [rbp-1]
leaves only application_menu: one byte per hand, the displacement of "lea rax,[rbp-9]".

  left  @ RVA 0xa7a973: lea rax,[rbp-9] -> lea rax,[rbp-1]
  right @ RVA 0xa7b707: lea rax,[rbp-9] -> lea rax,[rbp-1]

START (system/click) stays the only system button.

The original bytes are checked first and the DLL is backed up as
driver_alvr_server.dll.orig. SteamVR locks the DLL, so this waits until vrserver.exe exits.

Usage: python tools/alvr_driver_patch.py [path\\to\\driver_alvr_server.dll] [--undo]
"""
import os
import shutil
import struct
import subprocess
import sys
import time

args = [a for a in sys.argv[1:] if not a.startswith("--")]
DLL = args[0] if args else r"C:\Users\habbo\Downloads\alvr_streamer_windows\bin\win64\driver_alvr_server.dll"
UNDO = "--undo" in sys.argv

SYSTEM_CLICK_RVA = 0xBB2C38    # "/input/system/click", to recognise the 20.14.1 DLL
APP_MENU_RVA = 0xBB2CB8        # "/input/application_menu/click"

# (rva, original bytes, patched bytes)
PATCHES = [
    (0xA7A973, bytes.fromhex("488d45f7"), bytes.fromhex("488d45ff")),
    (0xA7B707, bytes.fromhex("488d45f7"), bytes.fromhex("488d45ff")),
]


def rva_to_offset(data, rva):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    sec = pe + 24 + opt
    for i in range(nsec):
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, sec + i * 40 + 8)
        if va <= rva < va + max(vsize, rawsize):
            return rva - va + raw
    raise ValueError(hex(rva))


def steamvr_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq vrserver.exe"], capture_output=True, text=True).stdout
    return "vrserver.exe" in out


def main():
    if steamvr_running():
        print("Close SteamVR (the driver DLL is in use); waiting...", flush=True)
        while steamvr_running():
            time.sleep(1)
        time.sleep(2)
    data = bytearray(open(DLL, "rb").read())
    for s, text in ((SYSTEM_CLICK_RVA, b"/input/system/click\0"), (APP_MENU_RVA, b"/input/application_menu/click\0")):
        o = rva_to_offset(data, s)
        if bytes(data[o:o + len(text)]) != text:
            sys.exit("Unexpected driver_alvr_server.dll: this patch is for ALVR 20.14.1 (Windows) only.")
    done = 0
    for rva, orig, patched in PATCHES:
        o = rva_to_offset(data, rva)
        cur = bytes(data[o:o + len(orig)])
        want_from, want_to = (patched, orig) if UNDO else (orig, patched)
        if cur == want_to:
            continue
        if cur != want_from:
            sys.exit(f"Unexpected bytes at RVA {rva:#x}: {cur.hex()} (not ALVR 20.14.1?)")
        data[o:o + len(orig)] = want_to
        done += 1
    if not done:
        print("Nothing to do: already " + ("original." if UNDO else "patched."))
        return
    backup = DLL + ".orig"
    if not os.path.exists(backup):
        shutil.copy2(DLL, backup)
    open(DLL, "wb").write(data)
    print(("Restored" if UNDO else "Patched") + f" {DLL} ({done} site(s)). Original kept in {backup}", flush=True)


main()
