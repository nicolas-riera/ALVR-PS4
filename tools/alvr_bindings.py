"""Set the ALVR 20.14.1 streamer's manual button mapping for the PS4 client (Vive wands).

In ViveWand emulation, the streamer's automatic mapping never routes any client button to
the Vive's system button ("Received button not mapped: .../system/click"), so START did
nothing. A manual mapping replaces the automatic one: every button the PS4 client sends
is passed through to its Vive counterpart, with system kept separate from menu.

The dashboard rewrites session.json while it runs, so this waits until
"ALVR Dashboard.exe" is closed, backs session.json up, then patches it.

Usage: python tools/alvr_bindings.py [path/to/session.json]
"""
import json
import shutil
import subprocess
import sys
import time

SESSION = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\habbo\Downloads\alvr_streamer_windows\session.json"

# client path suffix -> Vive destination suffix
PAIRS = [
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


def dashboard_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ALVR Dashboard.exe"], capture_output=True, text=True).stdout
    return "ALVR Dashboard.exe" in out


def target(template, destination):
    t = json.loads(json.dumps(template))  # same default-form layout as the dashboard's template
    t["destination"] = destination
    t["mapping_type"]["variant"] = "Passthrough"
    t["binary_conditions"]["content"] = []
    return t


def main():
    if dashboard_running():
        print("Waiting for the ALVR dashboard to be closed...", flush=True)
        while dashboard_running():
            time.sleep(1)
        time.sleep(2)
    s = json.load(open(SESSION, encoding="utf-8"))
    bm = s["session_settings"]["headset"]["controllers"]["content"]["button_mappings"]
    template = bm["content"]["value"]["element"]
    entries = []
    for hand in ("left", "right"):
        for src, dst in PAIRS:
            value = json.loads(json.dumps(bm["content"]["value"]))
            value["content"] = [target(template, f"/user/hand/{hand}/input/{dst}")]
            entries.append([f"/user/hand/{hand}/input/{src}", value])
    bm["set"] = True
    bm["content"]["content"] = entries
    backup = SESSION + ".bak-" + time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(SESSION, backup)
    json.dump(s, open(SESSION, "w", encoding="utf-8"), indent=2)
    print(f"Manual button mapping written ({len(entries)} entries), backup: {backup}", flush=True)


main()
