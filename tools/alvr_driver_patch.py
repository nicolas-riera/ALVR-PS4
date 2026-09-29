"""Patch ALVR 20.14.1's SteamVR driver (driver_alvr_server.dll) for the PS4 client.

1. Menu button no longer also presses "system".
   /user/hand/*/input/menu/click is hard-wired to two SteamVR components (Paths.cpp, lines
   91 and 168): {"/input/system/click", "/input/application_menu/click"}, so the PS4
   client's menu buttons (left SQUARE, right TRIANGLE) also opened the SteamVR dashboard.
   Each list is built on the stack as two pointers, [rbp-9] = system and [rbp-1] =
   application_menu, passed as the range [rbp-9, rbp+7). Starting it at [rbp-1] leaves
   only application_menu: one byte per hand.
     left  @ RVA 0xa7a973, right @ RVA 0xa7b707: lea rax,[rbp-9] -> lea rax,[rbp-1]

2. Headset "searching" state.
   Hmd::OnPoseUpdated always reports the headset as tracked (poseIsValid = true,
   result = TrackingResult_Running_OK). The PS4 client signals a headset the camera has
   lost for 3 s by a head height below -500 m (ALVR's recentering only moves x/z, so the
   marker survives). At RVA 0xa78b7a, "mov dword [rsp+0x170], 200" (result) becomes a call
   to a check placed in the unused tail of .text (RVA 0xae2d40, the section's VirtualSize
   grows to its raw size to map it):
       result = 200; if (motion.position.y < -500) { result = 201 /*Running_OutOfRange*/;
                                                     poseIsValid = false; }
   SteamVR then shows the headset as searching, with its grey screen.

3. Controller "searching" state.
   Controller::OnPoseUpdate reports a controller the client sends as tracked, and one the
   client omits as disconnected; Controller::SetButton drops every input while the last
   pose is not valid. The PS4 client keeps sending a controller the camera has lost for
   10 s, with a height below -500 m. The jmp ending the controllerMotion branch
   (RVA 0xa75916) goes through a second check placed after the first one (RVA 0xae2d70):
       if (motion.position.y < -500) { result = 201; poseIsValid = false; }
   deviceIsConnected stays true, so SteamVR shows the controller as searching (hidden).
   SetButton then tests last_pose.deviceIsConnected instead of poseIsValid
   (RVA 0xa766aa: cmp byte [r14+0x12c] -> [r14+0x12f]), so its buttons keep working.
   A controller switched off (omitted by the client) stays disconnected.

4. Controllers registered even when SteamVR activates them late.
   InitializeStreaming registers the controllers with TrackedDevice::register_device(true),
   which waits only 1 s for SteamVR to call Activate. When SteamVR is busy (just started,
   many drivers, another headset), the wait times out: the driver then drops its pointer to
   the controller, SteamVR activates it anyway a moment later, and it never gets a pose or
   an input until SteamVR restarts. The final test "activation_state == Success" becomes
   "activation_state != Failure" (RVA 0xa6ee3f: cmp dword [rbx+0x13c], 1 / sete bl ->
   cmp ..., 2 / setne bl), so a controller still pending is kept and works as soon as
   SteamVR activates it (OnPoseUpdate and SetButton skip it until then).

The original bytes are checked first and the DLL is backed up as
driver_alvr_server.dll.orig. SteamVR locks the DLL, so this waits until vrserver.exe exits.

Usage: python tools/alvr_driver_patch.py <path\\to\\driver_alvr_server.dll> [--undo]
"""
import os
import shutil
import struct
import subprocess
import sys
import time

args = [a for a in sys.argv[1:] if not a.startswith("--")]
if not args:
    sys.exit(__doc__)
DLL = args[0]
UNDO = "--undo" in sys.argv

SYSTEM_CLICK_RVA = 0xBB2C38    # "/input/system/click", to recognise the 20.14.1 DLL
APP_MENU_RVA = 0xBB2CB8        # "/input/application_menu/click"

HMD_RESULT_RVA = 0xA78B7A      # mov dword [rsp+0x170], 0xc8 (DriverPose_t.result = 200)
CAVE_RVA = 0xAE2D40            # tail of .text, zero padding in the file
TEXT_VSIZE_ORIG, TEXT_VSIZE_NEW = 0xAE1D3F, 0xAE1E00

CAVE = bytes.fromhex(
    "c7842478010000c8000000"   # mov dword [rsp+0x178], 200    (rsp+8: we were called)
    "448b531c"                 # mov r10d, [rbx+0x1c]          (FfiDeviceMotion.position.y)
    "4181fa0000fac3"           # cmp r10d, 0xc3fa0000          (-500.0f)
    "7613"                     # jbe done                      (unsigned: y >= -500 or positive)
    "c7842478010000c9000000"   # mov dword [rsp+0x178], 201    (TrackingResult_Running_OutOfRange)
    "c684247c01000000"         # mov byte [rsp+0x17c], 0       (poseIsValid = false)
    "c3"                       # done: ret
)
CTRL_JMP_RVA = 0xA75916       # jmp 0xa75ba3, end of the controllerMotion branch
CTRL_TARGET_RVA = 0xA75BA3
CTRL_CAVE_RVA = 0xAE2D70       # after the headset check
SETBUTTON_RVA = 0xA766AA       # cmp byte [r14+0x12c], 0 (last_pose.poseIsValid)
ACTIVATION_RVA = 0xA6EE3F      # register_device: cmp dword [rbx+0x13c], 1 (activation_state)

CTRL_CAVE = bytes.fromhex(
    "418b461c"                 # mov eax, [r14+0x1c]           (FfiDeviceMotion.position.y)
    "3d0000fac3"               # cmp eax, 0xc3fa0000           (-500.0f)
    "760b"                     # jbe back
    "c74550c9000000"           # mov dword [rbp+0x50], 201     (result = Running_OutOfRange)
    "c6455400"                 # mov byte [rbp+0x54], 0        (poseIsValid = false)
)
CTRL_CAVE += b"\xe9" + struct.pack("<i", CTRL_TARGET_RVA - (CTRL_CAVE_RVA + len(CTRL_CAVE) + 5))

CALL_CAVE = b"\xe8" + struct.pack("<i", CAVE_RVA - (HMD_RESULT_RVA + 5)) + bytes.fromhex("660f1f440000")

# (where, original bytes, patched bytes); where = RVA, or ("text_vsize",) for the header
PATCHES = [
    (0xA7A973, bytes.fromhex("488d45f7"), bytes.fromhex("488d45ff")),
    (0xA7B707, bytes.fromhex("488d45f7"), bytes.fromhex("488d45ff")),
    (CAVE_RVA, bytes(len(CAVE)), CAVE),
    (HMD_RESULT_RVA, bytes.fromhex("c7842470010000c8000000"), CALL_CAVE),
    (("text_vsize",), struct.pack("<I", TEXT_VSIZE_ORIG), struct.pack("<I", TEXT_VSIZE_NEW)),
    (CTRL_CAVE_RVA, bytes(len(CTRL_CAVE)), CTRL_CAVE),
    (CTRL_JMP_RVA, bytes.fromhex("e988020000"), b"\xe9" + struct.pack("<i", CTRL_CAVE_RVA - (CTRL_JMP_RVA + 5))),
    (SETBUTTON_RVA, bytes.fromhex("4180be2c01000000"), bytes.fromhex("4180be2f01000000")),
    (ACTIVATION_RVA, bytes.fromhex("83bb3c01000001488bcf0f94c3"), bytes.fromhex("83bb3c01000002488bcf0f95c3")),
]


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    sec = pe + 24 + opt
    for i in range(nsec):
        hdr = sec + i * 40
        name = bytes(data[hdr:hdr + 8]).rstrip(b"\0")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, hdr + 8)
        yield name, hdr, vsize, va, rawsize, raw


def offset_of(data, where):
    if where == ("text_vsize",):
        for name, hdr, *_ in sections(data):
            if name == b".text":
                return hdr + 8
        raise ValueError(".text")
    for _, _, vsize, va, rawsize, raw in sections(data):
        if va <= where < va + max(vsize, rawsize):
            return where - va + raw
    raise ValueError(hex(where))


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
        o = offset_of(data, s)
        if bytes(data[o:o + len(text)]) != text:
            sys.exit("Unexpected driver_alvr_server.dll: this patch is for ALVR 20.14.1 (Windows) only.")
    # Validate everything before writing anything.
    todo = []
    for where, orig, patched in PATCHES:
        o = offset_of(data, where)
        cur = bytes(data[o:o + len(orig)])
        want_from, want_to = (patched, orig) if UNDO else (orig, patched)
        if cur == want_to:
            continue
        if cur != want_from:
            sys.exit(f"Unexpected bytes at {where}: {cur.hex()} (not ALVR 20.14.1?)")
        todo.append((o, want_to))
    if not todo:
        print("Nothing to do: already " + ("original." if UNDO else "patched."))
        return
    for o, b in todo:
        data[o:o + len(b)] = b
    backup = DLL + ".orig"
    if not os.path.exists(backup):
        shutil.copy2(DLL, backup)
    open(DLL, "wb").write(data)
    print(("Restored" if UNDO else "Patched") + f" {DLL} ({len(todo)} site(s)). Original kept in {backup}", flush=True)


main()
