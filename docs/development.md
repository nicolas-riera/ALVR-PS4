# Development

[Back to the README](../README.md)

## Building

The client is C/C++ built with the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain),
in WSL (Debian):

```
wsl -d Debian -- bash tools/build.sh                  # Dev build: client/IV0000-ALVR00002_00-ALVRPS4CLIENT000.pkg
wsl -d Debian -- bash tools/build.sh VARIANT=stable   # stable build: client/IV0000-ALVR00001_00-ALVRPS4CLIENT000.pkg
python tools/deploy.py [--stable] [ps4_ip] [port]     # uploads the Dev (or stable) pkg over GoldHEN FTP to /data/pkg/
```

ALVR PS4 Tracking Viewer (the PC companion in `companion/`, see [Usage](usage.md#tracking-viewer-on-the-pc))
is a Win32 program built with MinGW-w64, also in WSL (`sudo apt install g++-mingw-w64-x86-64`):

```
wsl -d Debian -- bash tools/build_companion.sh       # companion/build/ALVR PS4 Tracking Viewer.exe (static, portable)
```

It draws with the lobby's renderer (`client/src/lobby.cpp`, compiled in) and shares the
wire format with the app (`client/src/trackview_proto.h`): it sends a hello to UDP port
9955 on the console every second, and the app answers with the tracked devices at up to
60 Hz while the hellos keep coming (`client/src/trackview.cpp`). Its icon comes from
`tools/logo/make_trackview_icon.py`. The setup .bat installs it:
`tools/make_release.py` embeds the built .exe (gzip + base64, about 280 KB of the .bat), so
build it before making a release. Its headset model is drawn by
`client/src/lobby.cpp` (`draw_headset`); `tools/test/lobby_preview.cpp` mode 11 renders it
alone through a narrow lens, for comparing with photos of the PS VR. Its `.psvrdata` recordings are described byte by
byte in [The .psvrdata recording format](psvrdata-format.md).

`tools/build.sh` expects the toolchain in `~/ps4/OpenOrbis/PS4Toolchain` and a local
OpenSSL 1.1 in `~/ps4/libssl11` (PkgTool needs it; Debian 13 no longer ships it).

The default build is the development variant, **"ALVR PS4 (Dev)"** (title ID ALVR00002,
DEV icon). It installs next to the stable app (title ID ALVR00001) instead of replacing
it, and shares its saved settings. Only the Dev build sends logs and runs the video bench.

The version is set in two places, bumped together for a release: `ALVR_PS4_VERSION` in
`client/src/main.cpp` (shown in the lobby) and `VERSION` in `client/Makefile` (the
param.sfo APP_VER, which must increase with every release).

## Logs

The Dev build broadcasts its log lines over UDP on the local network, port 9950:

```
python tools/log_receiver.py               # prints the PS4 logs and saves them into logs/
```

The stable build sends no logs.

While streaming, a `video:` line every 5 seconds sums up the stream: frames received,
decoded, dropped and lost, the decoder call and conversion times, the input queue, latency
(`m2p`), the headset prediction, and the frame pacing: `repeat` (display frames with no new
frame), `skip` (frames never shown), `late` (frames that came after the decision point they
were due at), `margin` (the jitter margin frames wait for), `hold` (how long the last frame
waited after its conversion), `resync` (safety net used; should stay 0), `decode` (received
to decoder picture), `wait` (picture to submission), `lead` (how long before a compositor
pass the frame is submitted) and `misses` (submissions that came after the pass); then
`arrival` (gaps in the frames received from the PC, frames bunched together, longest gap)
and `pose` (frames whose tracking pose was not found, went back in time, or was unusually
old). How the decoder and the pacing work: [PS4 video decoder](ps4-video-decoder.md).

## Release

```
python tools/make_release.py               # release/ALVR-PS4-v<version>.pkg and ALVR-PS4-Setup.bat
python tools/make_release.py --bat-only    # only the .bat (no stable pkg needed)
```

Build the stable pkg first. `ALVR-PS4-Setup.bat` embeds `pc-setup/setup.ps1`, the settings
template `pc-setup/alvr-ps4-session.json` and the SteamVR icons (`pc-setup/icons`); the
files in `release/` are uploaded to a GitHub release.

## Video bench

With the Dev app running in the lobby (SteamVR closed), `python tools/video_bench.py` encodes
test clips on the PC with NVENC (ALVR's encoder settings, one parameter changed per test),
plays them on the PS4 through the real decoder and conversion (TCP 9955), and prints the
timings. `--list` shows the test plans; the script's header describes every option.

## Source layout

| Path | Contents |
| --- | --- |
| `client/src/main.cpp` | startup, main loop (90 or 60 Hz), lobby/video display, frame pacing, tracking uplink |
| `client/src/alvr_client.*` | ALVR 20.14.1 protocol: discovery, handshake, streams, video reassembly |
| `client/src/video.*` | H.264 decoding (libSceVideodec2, the system decoder: CPU + GPU compute), paced display, NV12 → RGB |
| `client/src/foveation.*` | foveated encoding math (expansion of the squeezed eye edges), same as the official client |
| `client/src/audio.*` | game audio playback (libSceAudioOut) and microphone capture (libSceAudioIn) |
| `client/src/hmd.*`, `reproj.*`, `screen.*` | PSVR (libSceHmd), the system reprojection compositor, video output and VR display mode |
| `client/src/hmd_setup.*` | system dialogs: "connect your PlayStation VR" and "confirm your position" |
| `client/src/tracker.*`, `camera.*`, `move.*`, `move_predict.*`, `wand.*`, `pad.*`, `hid.*` | camera tracking (libSceVrTracker), PS Move, PS Move position while out of view (arm model, glide back), Vive wand emulation (trackpad behaviours), DualShock 4, raw HID reports |
| `client/src/config.*` | settings stored in the PS4 save data (libSceSaveData) |
| `client/src/settings.*` | lobby settings panel, first launch height panel and height calibration |
| `client/src/lobby.*` | software-rendered lobby |
| `client/src/bench.*` | video bench server (Dev build) |
| `client/src/log.*`, `bincode.*`, `vrmath.h`, `font_data.h`, `stroke_font.h` | UDP logs (Dev build), ALVR's binary serialization, quaternion helpers, the TV status font and the lobby's stroke font |
| `docs/alvr-20.14.1-protocol.md` | the ALVR 20.14.1 wire protocol, as implemented |
| `docs/psvr-technical-reference.md`, `docs/ps4-video-decoder.md` | technical references: the PSVR and the PS4's VR system; the PS4 video decoder |
| `tools/alvr_setup.py` | PC-side ALVR settings for this client |
| `tools/alvr_driver_patch.py` | ALVR 20.14.1 driver patches: menu is not system, headset and controllers "searching", late controller activation |
| `pc-setup/` | sources of the one-file PC setup (`setup.ps1`, settings template, SteamVR icons) |
| `tools/icons/make_icons.py` | draws the PSVR and PS Move SteamVR status icons (`pc-setup/icons`) |
| `tools/logo/` | app icons (stable, DEV, save data and Tracking Viewer icons) |
| `tools/make_release.py` | builds `release/` (`ALVR-PS4-v<version>.pkg`, `ALVR-PS4-Setup.bat`) for the GitHub release |
| `tools/re/` | reverse-engineering helpers (headless Ghidra on dumped system modules) |
| `tools/test/` | host-side tests: ALVR path hash, lobby preview rendered to an image |
| `tools/video_bench.py` | video bench (see above) |
| `tools/build.sh`, `tools/deploy.py`, `tools/log_receiver.py` | build in WSL, upload over FTP, receive the Dev build's logs |
| `tools/fontgen/` | generators of the two fonts |
| `companion/`, `tools/build_companion.sh` | ALVR PS4 Tracking Viewer, the PC window showing what the PS4 tracks |

## Saved settings

The client's settings are changed in the lobby (Start, or Options on the DualShock 4) and
kept in the PS4's save data of the user who started the app (save "settings", file
`settings.txt`, one `key=value` per line). The save is not reachable over FTP; Settings >
Application Saved Data Management shows it as "ALVR PS4 - Settings". The Dev build shares
the stable app's save. It holds these values:

| Key | Default | Meaning |
| --- | --- | --- |
| `hostname` | random `NNNN.client` | the name the PS4 announces to ALVR |
| `resolution_percent` | `130` | resolution per eye, in percent of the PSVR panel (960×1080), 50–160. The PS4 decoder slows down sharply above about 1920×1088 per frame (both eyes): with foveated encoding at the recommended settings, 130% decodes a 1920×1056 frame. Without foveated encoding, use 100 |
| `head_prediction` | `40` | "Headset prediction": share of the stream latency over which the headset position sent to SteamVR is predicted, 0–100 (0 = current position) |
| `controller_prediction_ms` | `0` | extra controller prediction on top of SteamVR's, 0–60 |
| `user_height_cm` | `0` (not set) | your height, set in the lobby (first launch wizard, then settings); `0` shows the wizard at the next launch (a settings Reset also shows it at once) |
| `camera_height_cm` | `0` (estimated) | height of the PS Camera above the floor: derived from your height, or measured by the height calibration |
| `center_on_steamvr_start` | `1` | `1`: the play area centre is placed on the headset each time SteamVR connects; `0`: only once, at the first tracking |
| `refresh_rate_hz` | `90` | `90`: PSVR at 90 Hz, 90 fps stream; `60`: PSVR at 120 Hz, 60 fps stream (each frame shown twice) |
| `vibration_percent` | `100` | PS Move and DualShock 4 vibration strength, 0-100 (0 = off), in steps of 10 |
| `hud` | `0` | `1`: performance overlay in the headset while playing |
