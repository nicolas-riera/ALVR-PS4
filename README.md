# ALVR PS4

Use a **PlayStation VR** headset as a **SteamVR** headset: this homebrew app for a jailbroken
PS4 is an [ALVR](https://github.com/alvr-org/ALVR) client. The PC runs the ALVR streamer and
SteamVR. The PS4 receives the video stream and shows it in the PSVR. It sends back the
headset tracking (PS Camera) and the PS Move controllers (tracking, buttons, vibration).

The app behaves like a regular PSVR game: it uses the system VR mode (PSVR quick menu,
lens distortion, 120 Hz reprojection), and closing it from the PS4 menu puts the console
back in normal mode.

> Status: work in progress. Headset tracking, the PS Move controllers (emulated as HTC
> Vive wands), the lobby, and 60 Hz video (hardware H.264 decoding, shown by the system
> at 120 Hz through reprojection) work. Game audio and the microphone are new and still
> being tested.

## Requirements

| Side | What |
| --- | --- |
| PS4 | PS4 or PS4 Pro with [GoldHEN](https://github.com/GoldHEN/GoldHEN) (tested on a Pro, firmware 11.00) |
| VR | PSVR (CUH-ZVR1 or ZVR2), PS Camera, two PS Move controllers |
| PC | Windows, SteamVR, **ALVR streamer 20.14.1 exactly** ([release page](https://github.com/alvr-org/ALVR/releases/tag/v20.14.1), `ALVR-Windows.zip` or the portable streamer) |
| Network | PC and PS4 on the same local network, PS4 on Ethernet (or at least 5 GHz Wi-Fi) |

The PS4 client implements the ALVR **20.14.1** protocol only. ALVR changes its protocol
between versions, so any other streamer version will not connect.

## 1. Install the app on the PS4

1. Start GoldHEN on the PS4.
2. Copy `IV0000-ALVR00001_00-ALVRPS4CLIENT000.pkg` to the console, either to a USB drive
   or over GoldHEN's FTP server (port 2121, to `/data/pkg/`).
3. In GoldHEN, open **Package Installer** and install the package. The app **ALVR PS4**
   appears on the home screen.

## 2. Set up the ALVR streamer on the PC

### Automatic setup (recommended)

Double-click **`pc-setup\Setup ALVR for PS4.bat`** and accept the administrator prompt. It:

1. closes the ALVR dashboard and SteamVR if they run;
2. downloads ALVR streamer **20.14.1** into `%LOCALAPPDATA%\Programs\ALVR-PS4\alvr_streamer_windows`
   (skipped if already there);
3. patches its SteamVR driver (see "Driver patches" below; the download is checked by its
   SHA-256 first, and the original is kept as `driver_alvr_server.dll.orig`);
4. installs [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) for the microphone,
   unless Virtual Audio Cable or VB-Cable is already installed;
5. writes the PS4 settings (the table below) into ALVR's `session.json`, after backing up
   any existing one;
6. opens the ALVR ports (9943-9944, UDP and TCP) in the Windows firewall;
7. registers the driver with SteamVR, creates an "ALVR (PS4)" desktop shortcut and starts
   the dashboard.

It is safe to run again. To install elsewhere:
`"Setup ALVR for PS4.bat" -InstallDir D:\VR`.

### Manual setup

1. Extract [ALVR streamer 20.14.1](https://github.com/alvr-org/ALVR/releases/tag/v20.14.1)
   (`alvr_streamer_windows.zip`) and start **ALVR Dashboard.exe**. Follow its setup wizard,
   which registers the SteamVR driver and adds the firewall rules.
2. Apply the driver patches: `python tools\alvr_driver_patch.py "C:\path\to\alvr_streamer_windows\bin\win64\driver_alvr_server.dll"`.
3. Apply the PS4 settings: `python tools\alvr_setup.py --mic "C:\path\to\alvr_streamer_windows\session.json"`,
   or set them by hand (table below). The dashboard rewrites its settings while it runs,
   so the script waits until you close it. Leave out `--mic` without a virtual audio cable.

### Microphone

ALVR delivers the PSVR microphone to Windows through a virtual audio cable. Install
[Virtual Audio Cable](https://vac.muzychenko.net/en/) (its first cable is "Line 1"; ALVR's
**VAC** preset), or [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (then choose the
**VB Cable** preset in the dashboard). Then run the setup script with `--mic`, or set
Audio → Microphone to on with that preset. In Windows and in games, choose the cable's
**output** ("Line 1" / "CABLE Output") as the microphone. Do not make the cable the
default Windows output: ALVR refuses to connect if game audio and the microphone use the
same device.

The Windows output device used for game audio must run at **48 kHz** (Sound settings →
device properties → Advanced), the only rate the PS4 plays.

### By hand, in the dashboard (Settings tab)

| Setting | Value | Why |
| --- | --- | --- |
| Video → Preferred codec | **H264** | the PS4 hardware decoder is used for H.264 only |
| Video → Foveated encoding | **on**: center region width 0.5, height 0.5, center shift X 0, Y 0, horizontal and vertical edge ratio 2 | keeps the center of each eye at full resolution and squeezes the edges 2:1 (the lenses blur them anyway): at the default 130% resolution the PS4 decodes a 1920×1056 frame instead of 2496×1408, the size its decoder handles quickly. The client reads these values from the streamer, so other values work too; a smaller center or a higher edge ratio decodes faster but blurs more of the view |
| Video → Preferred FPS | **60** | the client offers 60 Hz (the PSVR reprojects it to 120 Hz) |
| Video → Encoder → Quality preset | **Quality** (NVENC: **P4**) | the fastest preset blurs text; the PC's GPU pays for it, not the PS4 |
| Video → Bitrate | **Constant, 60 Mbps** | 30 Mbps is too low for readable text |
| Headset → Extra OpenVR props | `TrackingSystemNameString` = `htc`, `ModelNumberString` = `PlayStation VR`, `ManufacturerNameString` = `Sony Interactive Entertainment`, `RenderModelNameString` = `generic_hmd`, `RegisteredDeviceTypeString` = `sony/psvr`, `DriverVersionString` = `20.14.1` | the "Custom" headset mode declares no identity at all; VRChat left the head at the origin |
| Headset → Controllers → Extra OpenVR props | also `CurrentUniverseIdUint64` = `2` | same tracking universe as the headset |
| Audio → Game audio | **on** | played in the PSVR headphones |
| Audio → Microphone | **on**, devices **VAC**, only with a virtual audio cable (see below) | the PSVR microphone becomes a Windows microphone |
| Connection → Stream protocol | **UDP** | TCP streaming is not implemented |
| Headset → Controllers → Emulation mode | **Vive Wand** | PS Move buttons map onto a Vive wand |
| Headset → Controllers → Hand skeleton | **off** | the PS Move has no finger tracking |
| Headset → Controllers → Left controller position offset | **0, 0, 0** | the default −11 cm is for Quest controllers and puts the wand ahead of the Move |
| Headset → Controllers → Left controller rotation offset | **0, 0, 0** | |
| Headset → Controllers → Extra OpenVR props | add `InputProfilePathString` = `{htc}/input/vive_controller_profile.json` | ALVR 20.14.1 gives its Vive wands the Oculus Touch profile by mistake, so SteamVR used Touch bindings |
| Headset → Controllers → Button mappings | enabled, with the 18 entries below | ALVR's automatic mapping never reaches the Vive **system** button |

Button mappings: for `left` and `right`, each source maps to one destination with the
**Passthrough** type:

| Source (`/user/hand/<hand>/input/…`) | Destination (`/user/hand/<hand>/input/…`) |
| --- | --- |
| `menu/click` | `menu/click` |
| `system/click` | `system/click` |
| `squeeze/click` | `squeeze/click` |
| `trigger/click` | `trigger/click` |
| `trigger/value` | `trigger/value` |
| `thumbstick/x` | `trackpad/x` |
| `thumbstick/y` | `trackpad/y` |
| `thumbstick/click` | `trackpad/click` |
| `thumbstick/touch` | `trackpad/touch` |

### Driver patches

Two behaviours of the ALVR 20.14.1 SteamVR driver cannot be changed by any setting.
`tools/alvr_driver_patch.py` (and the automatic setup) patches `driver_alvr_server.dll`:

* **Menu is not system.** The driver wires every controller's menu button to both the Vive
  **application menu** and **system** inputs (`Paths.cpp`), so the PS Move menu buttons
  (left □, right △) also opened the SteamVR dashboard. One byte per hand makes menu feed
  only the application menu; START stays the system button.
* **Headset "searching".** The driver always reports the headset as tracked. When the PS
  Camera has not seen the headset for 3 seconds, the PS4 client sends a marker height
  (below −500 m). The patched driver then reports the headset as out of range, and SteamVR
  shows it as searching, with its grey screen. The PS4 lobby is not affected.

The script waits until SteamVR is closed, because the DLL is locked while it runs. It checks
that the DLL really is 20.14.1 and keeps the original as `driver_alvr_server.dll.orig`.
Run it with `--undo` to restore the original. Run it again after reinstalling ALVR.

### Recommended: disable unused SteamVR add-ons

When the PS4 connects, the ALVR driver gives SteamVR only 1 second to activate each
controller. If SteamVR is still loading other drivers, the activation times out and the
controller stays greyed out ("standby") for the whole session. The PS4 client already
waits 6 seconds after SteamVR starts before it connects. Disabling the drivers you do not
use with ALVR makes this more reliable, and SteamVR also starts faster. To do so, open
SteamVR → Settings → Startup / Shutdown → **Manage Add-ons**, then turn off Steam Link
(vrlink), Oculus, OculusTouchLink, SlimeVR, Amethyst, PSMoveServiceEx and similar add-ons.

## 3. First connection

1. Plug in the PSVR and the PS Camera, then turn on the headset.
2. Turn on the PS Moves: **right hand first, then left hand**. The first Move connected
   (magenta sphere) is the right hand.
3. Start **ALVR PS4** on the PS4. The headset enters VR mode and shows the lobby: a white
   grid with your Moves and the camera. An information panel in front of you shows the
   PS4's IP address, its ALVR hostname (for example `5026.client`) and the connection state.
4. On the PC, start the ALVR dashboard. The PS4 appears under **Devices** with its
   hostname. Click **Trust**. If it does not appear, add it by hand with the PS4 IP address
   shown in the lobby.
5. Start SteamVR from the dashboard. The lobby panel goes through "PC found, waiting for
   SteamVR…" (6 s) and "Connecting…", then "Connected". The first time, ALVR may restart
   SteamVR once to apply the PS4's resolution.
6. The SteamVR image replaces the lobby.

To quit, press the PS button and close the app from the PS4 menu. The console leaves VR
mode by itself.

## Controls (PS Move as a Vive wand)

| Vive wand | Left Move | Right Move |
| --- | --- | --- |
| Trigger (analog) | T | T |
| Trackpad touch | hold **MOVE** and tilt the controller (roll = left/right, pitch = up/down); the touch starts at the pad centre | same |
| Trackpad click | **△** (+ tilt for the position) | **□** (+ tilt) |
| Grip | **○** | **✕** |
| Menu | **□** | **△** |
| System (SteamVR dashboard) | **START** | **START** |

**SELECT** is never bound: the PS4 takes it for screenshots. **PS** opens the PS4 menu.
The Moves vibrate with the game's haptics. In the lobby, they also tick on button presses
while no PC is connected.

**Recenter / fix drift:** open the PS4 menu (PS button), then go back to the app: the
tracking is reset, as in PSVR games.

**Tracking loss:** a Move hidden from the camera keeps its last position. SteamVR shows it
as "searching" after 10 s. A headset the camera has lost is shown as "searching", with
SteamVR's grey screen, after 3 s (needs the driver patch).

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Lobby says "Waiting for the PC" | Is the dashboard running and the PS4 trusted? Are the PC and PS4 on the same network? Check the ALVR firewall rules (the dashboard's setup wizard adds them) |
| "SteamVR is restarting" at every connection | Normal once after changing the resolution or the foveated encoding settings. At every connection: the PS4 client is older than v0.9.3 (it did not support foveated encoding); update it or turn foveated encoding off |
| Stream stutters in busy scenes (log: `decode` above 10 ms) | Foveated encoding off, or a resolution too high for it: the decoded frame (log line `video: ... decoded frame WxH`) should stay within 1920×1088 |
| Controllers greyed out / "standby" in SteamVR | Controller activation timed out: disable unused SteamVR add-ons, then restart SteamVR |
| SteamVR shows Oculus Touch controllers or wrong bindings | Missing `InputProfilePathString` extra OpenVR prop |
| START does nothing | Button mappings not applied (run `tools/alvr_setup.py`) |
| Menu (□ / △) also opens the SteamVR dashboard | Driver patch not applied (run `tools/alvr_driver_patch.py`) |
| Wand drawn ahead of the Move | Controller position offset not set to 0, 0, 0 |
| The lobby stays although "Connected" | The codec must be H264. Look at the PS4 logs (below) for `video:` lines |
| No sound in the headset | Game audio must be on, and the Windows output device must be at 48 kHz (the PS4 logs say "game audio at … Hz is not supported" otherwise) |
| "Game audio and microphone cannot point to the same device" | The virtual cable is the default Windows output: pick your real speakers/headset as default |

## For developers

The client is C/C++ built with the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain),
in WSL (Debian):

```
wsl -d Debian -- bash tools/build.sh      # builds client/IV0000-ALVR00001_00-ALVRPS4CLIENT000.pkg
python tools/deploy.py [ps4_ip] [port]     # uploads it over GoldHEN FTP to /data/pkg/
python tools/log_receiver.py               # receives the PS4 logs (UDP broadcast, port 9950) into logs/
```

`tools/build.sh` expects the toolchain in `~/ps4/OpenOrbis/PS4Toolchain` and a local
OpenSSL 1.1 in `~/ps4/libssl11` (PkgTool needs it; Debian 13 no longer ships it).

| Path | Contents |
| --- | --- |
| `client/src/main.cpp` | startup, main loop (60 Hz), lobby/video display, tracking uplink |
| `client/src/alvr_client.*` | ALVR 20.14.1 protocol: discovery, handshake, streams, video reassembly |
| `client/src/video.*` | hardware H.264 decoding (libSceVideodec2), NV12 → RGB |
| `client/src/foveation.*` | foveated encoding math (expansion of the squeezed eye edges), same as the official client |
| `client/src/audio.*` | game audio playback (libSceAudioOut) and microphone capture (libSceAudioIn) |
| `client/src/hmd.*`, `reproj.*`, `screen.*` | PSVR (libSceHmd) and the system reprojection |
| `client/src/tracker.*`, `camera.*`, `move.*`, `wand.*` | camera tracking (libSceVrTracker), PS Move, Vive wand emulation |
| `client/src/lobby.*` | software-rendered lobby |
| `docs/alvr-20.14.1-protocol.md` | the ALVR 20.14.1 wire protocol, as implemented |
| `tools/alvr_setup.py` | PC-side ALVR settings for this client |
| `tools/alvr_driver_patch.py` | ALVR 20.14.1 driver patches: menu is not system, headset "searching" |
| `pc-setup/` | one-click PC setup (`Setup ALVR for PS4.bat`, `setup.ps1`, settings template) |
| `tools/re/` | reverse-engineering helpers (headless Ghidra on dumped system modules) |

The PS4 client's settings are stored in `/data/alvr-ps4/config.txt` on the console. Edit
it over GoldHEN's FTP server, then restart the app:

| Key | Default | Meaning |
| --- | --- | --- |
| `hostname` | random `NNNN.client` | the name the PS4 announces to ALVR |
| `resolution_percent` | `130` | resolution per eye, in percent of the PSVR panel (960×1080), 50–160. The PS4 decoder slows down sharply above about 1920×1088 per frame (both eyes): with foveated encoding at the recommended settings, 130% decodes a 1920×1056 frame. Without foveated encoding, use 100 |
| `controller_prediction_ms` | `0` | extra controller prediction on top of SteamVR's, 0–60 |
