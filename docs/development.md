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
decoded and dropped, decode and conversion times, latency (`m2p`), and the frame pacing:
`repeat` (display frames with no new frame), `skip` (frames never shown), `late` (frames
that came after the decision point they were due at), `margin` (the jitter margin frames
wait for), `decode` (received to decoder picture), `wait` (picture to submission), `lead`
(how long before a compositor pass the frame is submitted) and `misses` (submissions that
came after the pass), and
`arrival` (gaps in the frames received from the PC, frames bunched together, longest gap).

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
| `client/src/hmd.*`, `reproj.*`, `screen.*` | PSVR (libSceHmd) and the system reprojection |
| `client/src/tracker.*`, `camera.*`, `move.*`, `wand.*`, `pad.*`, `hid.*` | camera tracking (libSceVrTracker), PS Move, Vive wand emulation, DualShock 4, raw HID reports |
| `client/src/config.*` | settings stored in the PS4 save data (libSceSaveData) |
| `client/src/settings.*` | lobby settings panel, first launch height panel and height calibration |
| `client/src/lobby.*` | software-rendered lobby |
| `client/src/bench.*` | video bench server (Dev build) |
| `docs/alvr-20.14.1-protocol.md` | the ALVR 20.14.1 wire protocol, as implemented |
| `tools/alvr_setup.py` | PC-side ALVR settings for this client |
| `tools/alvr_driver_patch.py` | ALVR 20.14.1 driver patches: menu is not system, headset and controllers "searching", late controller activation |
| `pc-setup/` | sources of the one-file PC setup (`setup.ps1`, settings template, SteamVR icons) |
| `tools/icons/make_icons.py` | draws the PSVR and PS Move SteamVR status icons (`pc-setup/icons`) |
| `tools/logo/` | app icons (stable, DEV and save data icons) |
| `tools/make_release.py` | builds `release/` (`ALVR-PS4-v<version>.pkg`, `ALVR-PS4-Setup.bat`) for the GitHub release |
| `tools/re/` | reverse-engineering helpers (headless Ghidra on dumped system modules) |
| `tools/test/` | host-side tests: ALVR path hash, lobby preview rendered to an image |
| `tools/video_bench.py` | video bench (see above) |

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
