"""Configure the ALVR 20.14.1 streamer (PC side) for the PS4 client.

Applies every setting the PS4 client needs (see README.md, "ALVR streamer settings"):
  - video: H.264 (the PS4 decodes H.264 only), 60 fps preferred (the client only offers
    60 Hz), foveated encoding on with PSVR settings: the center half of each eye keeps the
    full resolution and the edges are squeezed 2:1, so at the client's default 130%
    resolution (1248x1408 per eye) the PS4 decodes a 1920x1056 frame, the size its decoder
    handles quickly (above it, decoding took up to 17 ms per frame in busy scenes);
  - headset identity (PlayStation VR, same tracking system and universe as the controllers);
  - encoder quality: the "Quality" preset (NVENC P4) at 60 Mbps instead of the fastest
    preset at 30 Mbps, which blurred text (the PC's GPU pays for it, not the PS4);
  - game audio on; with --mic, the microphone too (needs VB-Audio Virtual Cable on the PC);
  - stream over UDP;
  - controllers: Vive wand emulation, hand skeleton off (PS Moves have no fingers),
    position/rotation offsets 0 (the default -11 cm is meant for Quest controllers and
    moved the wand ahead of the Move), Vive input profile (20.14.1 wrongly gives its Vive
    wands the Oculus Touch profile, so SteamVR used Touch bindings);
  - a manual button mapping: in Vive wand emulation the automatic mapping never routes any
    button to the Vive system button, so START did nothing.

The dashboard rewrites session.json while it runs: this waits until "ALVR Dashboard.exe"
is closed, backs session.json up, then patches it. Run it again after an ALVR update or
a settings reset; it is idempotent.

Usage: python tools/alvr_setup.py [--mic] <path\\to\\ALVR-PS4_PC-Streamer\\session.json>
"""
import json
import shutil
import subprocess
import sys
import time

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
if not ARGS:
    sys.exit(__doc__)
SESSION = ARGS[0]
MIC = "--mic" in sys.argv
BITRATE_MBPS = 60
FOVEATION = {
    "center_size_x": 0.5,
    "center_size_y": 0.5,
    "center_shift_x": 0.0,
    "center_shift_y": 0.0,
    "edge_ratio_x": 2.0,
    "edge_ratio_y": 2.0,
}

# client path suffix -> Vive destination suffix
BUTTON_PAIRS = [
    ("menu/click", "menu/click"),
    ("system/click", "system/click"),
    ("squeeze/click", "squeeze/click"),
    ("trigger/click", "trigger/click"),
    ("trigger/value", "trigger/value"),
    ("thumbstick/x", "trackpad/x"),
    ("thumbstick/y", "trackpad/y"),
    ("thumbstick/click", "trackpad/click"),
    ("thumbstick/touch", "trackpad/touch"),
]
VIVE_INPUT_PROFILE = "{htc}/input/vive_controller_profile.json"


def dashboard_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ALVR Dashboard.exe"], capture_output=True, text=True).stdout
    return "ALVR Dashboard.exe" in out


def copy(o):
    return json.loads(json.dumps(o))


def set_button_mappings(c):
    bm = c["button_mappings"]
    template = bm["content"]["value"]["element"]
    entries = []
    for hand in ("left", "right"):
        for src, dst in BUTTON_PAIRS:
            t = copy(template)
            t["destination"] = f"/user/hand/{hand}/input/{dst}"
            t["mapping_type"]["variant"] = "Passthrough"
            t["binary_conditions"]["content"] = []
            value = copy(bm["content"]["value"])
            value["content"] = [t]
            entries.append([f"/user/hand/{hand}/input/{src}", value])
    bm["set"] = True
    bm["content"]["content"] = entries


def set_props(container, values):
    props = container["extra_openvr_props"]["content"]
    props[:] = [p for p in props if p["key"]["variant"] not in values]
    for key, value in values.items():
        props.append({"key": {"variant": key}, "value": value})


# The "Custom" headset emulation sets no identity at all (empty tracking system, model
# and manufacturer), unlike the other modes. VRChat left the head at the origin with it.
# The headset joins the controllers' tracking system and universe (2, set by ALVR).
def icons(base):
    """SteamVR status icons (vrmonitor): the PSVR and PS Move icons that the PC setup installs
    in the ALVR driver's resources/icons (made by tools/icons/make_icons.py; copy
    pc-setup/icons there when setting up by hand). ALVR's own Vive wand paths
    ({htc}/icons/controller_*) do not exist in SteamVR's htc driver: grey icons."""
    return {
        "NamedIconPathDeviceOffString": base + "_off.png",
        "NamedIconPathDeviceSearchingString": base + "_searching.gif",
        "NamedIconPathDeviceSearchingAlertString": base + "_searching_alert.gif",
        "NamedIconPathDeviceReadyString": base + "_ready.png",
        "NamedIconPathDeviceReadyAlertString": base + "_ready_alert.png",
        "NamedIconPathDeviceAlertLowString": base + "_ready_low.png",
        "NamedIconPathDeviceStandbyString": base + "_standby.png",
        "NamedIconPathDeviceStandbyAlertString": base + "_standby_alert.png",
        "NamedIconPathDeviceNotReadyString": base + "_error.png",
    }


HB, CB = "{alvr_server}/icons/psvr_status", "{alvr_server}/icons/psmove_status"
HEADSET_PROPS = {
    "TrackingSystemNameString": "htc",
    "ModelNumberString": "PlayStation VR",
    "ManufacturerNameString": "Sony Interactive Entertainment",
    "RenderModelNameString": "generic_hmd",
    "RegisteredDeviceTypeString": "sony/psvr",
    "DriverVersionString": "20.14.1",
    # The PSVR has no battery (ALVR declares one for standalone headsets).
    "DeviceProvidesBatteryStatusBool": "false",
    **icons(HB),
}
CONTROLLER_PROPS = {
    "InputProfilePathString": VIVE_INPUT_PROFILE,
    "CurrentUniverseIdUint64": "2",
    # PS Move battery, sent by the client in Battery packets.
    "DeviceProvidesBatteryStatusBool": "true",
    "DeviceIsWirelessBool": "true",
    **icons(CB),
}


def main():
    if dashboard_running():
        print("Close the ALVR dashboard (this also closes SteamVR); waiting...", flush=True)
        while dashboard_running():
            time.sleep(1)
        time.sleep(2)
    s = json.load(open(SESSION, encoding="utf-8"))
    ss = s["session_settings"]

    video = ss["video"]
    video["preferred_codec"]["variant"] = "H264"
    fov = video["foveated_encoding"]
    fov["enabled"] = True
    fov["content"].update(FOVEATION)
    video["preferred_fps"] = 60.0
    enc = video["encoder_config"]
    enc["quality_preset"]["variant"] = "Quality"
    enc["nvenc"]["quality_preset"]["variant"] = "P4"
    video["bitrate"]["mode"]["variant"] = "ConstantMbps"
    video["bitrate"]["mode"]["ConstantMbps"] = BITRATE_MBPS

    audio = ss["audio"]
    audio["game_audio"]["enabled"] = True
    if MIC:
        audio["microphone"]["enabled"] = True
        audio["microphone"]["content"]["devices"]["variant"] = "VAC"
    ss["connection"]["stream_protocol"]["variant"] = "Udp"

    controllers = ss["headset"]["controllers"]
    controllers["enabled"] = True
    c = controllers["content"]
    c["tracked"] = True
    c["emulation_mode"]["variant"] = "ViveWand"
    c["hand_skeleton"]["enabled"] = False
    c["left_controller_position_offset"]["content"] = [0.0, 0.0, 0.0]
    c["left_controller_rotation_offset"]["content"] = [0.0, 0.0, 0.0]
    set_props(c, CONTROLLER_PROPS)
    set_props(ss["headset"], HEADSET_PROPS)
    set_button_mappings(c)

    backup = SESSION + ".bak-" + time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(SESSION, backup)
    json.dump(s, open(SESSION, "w", encoding="utf-8"), indent=2)
    print(f"ALVR settings updated for the PS4 client. Backup: {backup}", flush=True)


main()
