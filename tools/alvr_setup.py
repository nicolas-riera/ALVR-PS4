"""Configure the ALVR 20.14.1 streamer (PC side) for the PS4 client.

Applies every setting the PS4 client needs (see README.md, "ALVR streamer settings"):
  - video: H.264 (the PS4 decodes H.264 only), foveated encoding off (the client cannot
    undo it, and leaving it on makes the streamer restart SteamVR at every connection),
    60 fps preferred (the client only offers 60 Hz);
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

Usage: python tools/alvr_setup.py [path\\to\\alvr_streamer_windows\\session.json]
"""
import json
import shutil
import subprocess
import sys
import time

SESSION = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\habbo\Downloads\alvr_streamer_windows\session.json"

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


def set_input_profile(c):
    props = c["extra_openvr_props"]["content"]
    props[:] = [p for p in props if p["key"]["variant"] != "InputProfilePathString"]
    props.append({"key": {"variant": "InputProfilePathString"}, "value": VIVE_INPUT_PROFILE})


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
    video["foveated_encoding"]["enabled"] = False
    video["preferred_fps"] = 60.0
    ss["connection"]["stream_protocol"]["variant"] = "Udp"

    controllers = ss["headset"]["controllers"]
    controllers["enabled"] = True
    c = controllers["content"]
    c["tracked"] = True
    c["emulation_mode"]["variant"] = "ViveWand"
    c["hand_skeleton"]["enabled"] = False
    c["left_controller_position_offset"]["content"] = [0.0, 0.0, 0.0]
    c["left_controller_rotation_offset"]["content"] = [0.0, 0.0, 0.0]
    set_input_profile(c)
    set_button_mappings(c)

    backup = SESSION + ".bak-" + time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(SESSION, backup)
    json.dump(s, open(SESSION, "w", encoding="utf-8"), indent=2)
    print(f"ALVR settings updated for the PS4 client. Backup: {backup}", flush=True)


main()
