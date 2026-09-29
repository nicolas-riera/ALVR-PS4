# ALVR PS4

> **Disclaimer: this whole project was entirely vibecoded.** Every line of code, every
> script and this README were written by an AI (Claude), directed and tested on real
> hardware by a human who did not write the code. It works on the setup it was tested on;
> expect rough edges elsewhere, and use it at your own risk.

Use a **PlayStation VR** headset as a **SteamVR** headset: this homebrew app for a jailbroken
PS4 is an [ALVR](https://github.com/alvr-org/ALVR) client. The PC runs the ALVR streamer and
SteamVR. The PS4 receives the video stream and shows it in the PSVR. It sends back the
headset tracking (PS Camera) and the PS Move controllers (tracking, buttons, vibration).

The app behaves like a regular PSVR game: it uses the system VR mode (PSVR quick menu,
lens distortion, reprojection), and closing it from the PS4 menu puts the console back in
normal mode.

> Status: headset tracking, the PS Move controllers (emulated as HTC Vive wands, with
> vibration), the lobby, video (hardware H.264 decoding with foveated encoding, 90 Hz by
> default, or 60 Hz shown by the system at 120 Hz through reprojection), game audio and the
> microphone work. Mainly tested at 60 Hz.
> Mainly tested with VRChat and Beat Saber on a PS4 Pro.

## Requirements

| Side | What |
| --- | --- |
| PS4 | PS4 or PS4 Pro with [GoldHEN](https://github.com/GoldHEN/GoldHEN) (tested on a FAT and on a Pro, both firmware 11.00) |
| VR | PSVR (CUH-ZVR1 or ZVR2), PS Camera, *two PS Move controllers (optional)* |
| PC | Windows 10 or 11, SteamVR, **ALVR streamer 20.14.1 exactly** (the setup below downloads it) |
| Network | PC and PS4 on the same local network, PS4 on Ethernet (or at least 5 GHz Wi-Fi) |

The PS4 client implements the ALVR **20.14.1** protocol only. ALVR may changes its protocol
between versions, so any other streamer version may not connect.

## 1. Install the app on the PS4

Both files come from the project's **Releases** page: `ALVR-PS4-v0.9.x.pkg` for the PS4 and
`ALVR-PS4-Setup.bat` for the PC.

1. Start GoldHEN on the PS4.
2. Copy `ALVR-PS4-v0.9.x.pkg` to the console, either to a USB drive or over GoldHEN's FTP
   server (port 2121, to `/data/pkg/`).
3. In GoldHEN, open **Package Installer** and install the package. The app **ALVR PS4**
   appears on the home screen.

## 2. Set up the ALVR streamer on the PC

### Automatic setup (recommended, Windows only)

Install SteamVR from Steam first. Then double-click **`ALVR-PS4-Setup.bat`** (from the
Releases page) and accept the administrator prompt. It:

1. closes the ALVR dashboard and SteamVR if they run;
2. downloads ALVR streamer **20.14.1** into `ALVR-PS4_PC-Streamer\`, next to the .bat
   (skipped if already there; a folder named `alvr_streamer_windows` by an earlier setup is
   renamed; any other ALVR driver registered with SteamVR is unregistered);
3. patches its SteamVR driver (see "Driver patches" below; the download is checked by its
   SHA-256 first, and the original is kept as `driver_alvr_server.dll.orig`), and adds PSVR
   and PS Move status icons for SteamVR (`resources\icons`);
4. installs [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) for the microphone,
   unless Virtual Audio Cable or VB-Cable is already installed;
5. writes the PS4 settings (the table below) into ALVR's `session.json`, after backing up
   any existing one;
6. opens the ALVR ports (9943-9944, UDP and TCP) in the Windows firewall;
7. registers the driver with SteamVR (and unregisters any other ALVR version, which would
   fight over the headset), creates an "ALVR (PS4)" desktop shortcut and starts the
   dashboard.

It is safe to run again. To install into another folder, run it from a command prompt:
`ALVR-PS4-Setup.bat D:\VR`. To start over from scratch, run
`ALVR-PS4_PC-Streamer\ALVR-PS4-Reset.bat`: it deletes everything in that folder (ALVR, its
settings, logs) and installs it again with the default PS4 settings (the PS4 then has to be
trusted again). The file is built from `pc-setup/setup.ps1` and `pc-setup/alvr-ps4-session.json`
by `python tools/make_release.py`.

### Turning the virtual audio cable off

The virtual audio cable adds a speaker and a microphone to Windows. To get rid of them when
you do not need the PSVR microphone, run `ALVR-PS4_PC-Streamer\ALVR-PS4-Audio-Cable-Toggle.bat`:
it disables the cable (VB-Cable or Virtual Audio Cable) and turns ALVR's microphone off (ALVR
refuses to connect when its microphone device is missing). Run it again to enable both. It
only changes anything when you run it; the setup and the reset keep the cable as it is.

### Tracker mode (PS Moves with another headset)

`ALVR-PS4_PC-Streamer\ALVR-PS4-Tracker-Mode.bat` switches between the normal mode and a
tracker mode, for using the PS Moves as Vive trackers while wearing another SteamVR headset
(run it again to switch back). In tracker mode:

* the PSVR becomes a SteamVR tracking reference (ALVR's "tracking reference only"; nothing is
  displayed in it), and the PS Moves become Vive trackers (right Move = right foot, left
  Move = left foot; change the roles in SteamVR → Settings → Controllers → Manage Vive
  Trackers);
* all of them report their own tracking system, `PSMoves`: in
  [OpenVR Space Calibrator](https://github.com/hyblocker/OpenVR-SpaceCalibrator), pick it as
  the target space and calibrate with a PS Move held against one of the other headset's
  controllers;
* SteamVR's "Activate multiple drivers" setting is turned on, so that SteamVR loads ALVR
  next to the other headset's driver.

Keep ALVR PS4 running on the PS4 with the PSVR switched on, where the camera sees it. The
play area centre is set where the PSVR is each time SteamVR connects: calibrate again after
SteamVR restarts. The PS Move buttons do
nothing in this mode. The reset .bat or the setup brings the normal mode back as well.

### Manual setup

1. Extract [ALVR streamer 20.14.1](https://github.com/alvr-org/ALVR/releases/tag/v20.14.1)
   (`alvr_streamer_windows.zip`) and start **ALVR Dashboard.exe**. Follow its setup wizard,
   which registers the SteamVR driver and adds the firewall rules.
2. Apply the driver patches: `python tools\alvr_driver_patch.py "C:\path\to\ALVR-PS4_PC-Streamer\bin\win64\driver_alvr_server.dll"`.
3. Apply the PS4 settings: `python tools\alvr_setup.py --mic "C:\path\to\ALVR-PS4_PC-Streamer\session.json"`,
   or set them by hand (table below). The dashboard rewrites its settings while it runs,
   so the script waits until you close it. Leave out `--mic` without a virtual audio cable.
4. Copy `pc-setup\icons` to `ALVR-PS4_PC-Streamer\resources\icons` (the PSVR and PS Move
   status icons the settings refer to).

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
| Video → Preferred FPS | **90** | the client offers only the rate set on the PS4 (90 by default; 60 is reprojected to 120 Hz) |
| Video → Encoder → Quality preset | **Quality** (NVENC: **P4**) | the fastest preset blurs text; the PC's GPU pays for it, not the PS4 |
| Video → Bitrate | **Constant, 80 Mbps** | the PS4 decodes 90 fps up to about 130 Mbps (measured with `tools/video_bench.py`), but 100 Mbps was too much in use; 30 Mbps is too low for readable text |
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

Four behaviours of the ALVR 20.14.1 SteamVR driver cannot be changed by any setting.
`tools/alvr_driver_patch.py` (and the automatic setup) patches `driver_alvr_server.dll`:

* **Menu is not system.** The driver wires every controller's menu button to both the Vive
  **application menu** and **system** inputs (`Paths.cpp`), so the PS Move menu buttons
  (left □, right △) also opened the SteamVR dashboard. One byte per hand makes menu feed
  only the application menu; START stays the system button.
* **Headset "searching".** The driver always reports the headset as tracked. When the PS
  Camera has not seen the headset for 3 seconds, the PS4 client sends a marker height
  (below −500 m). The patched driver then reports the headset as out of range, and SteamVR
  shows it as searching, with its grey screen. The same happens right away while the
  headset tracking is not initialized yet (at start, or back from the PS menu).
* **Controllers "searching".** The driver reports a controller either tracked or
  disconnected, and drops its buttons while it is not tracked. A PS Move the camera has lost
  for 10 seconds (or never seen) is sent with the same marker height: the patched driver
  shows it as searching (hidden) and keeps its buttons working. A PS Move that is switched
  off stays disconnected.
* **Late controller activation.** When the PS4 connects, the driver gives SteamVR only 1
  second to activate each controller. When SteamVR is busy (just started, many drivers,
  another headset), the controllers then appeared in SteamVR but never moved nor received
  any button until SteamVR was restarted. The patched driver keeps a controller that SteamVR
  activates late, and it works as soon as it is activated.

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
   grid with your Moves, your DualShock 4 and the camera. An information panel in front of
   you shows the PS4's IP address, its ALVR hostname (for example `5026.client`) and the
   connection state. The first time, a panel asks for your height: stand straight, set it
   with − / +, then Confirm (the PC is searched for after that).
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

**Play area centre:** each time SteamVR connects, the centre of the play area is placed
where the headset is (direction and floor height unchanged). Afterwards, use SteamVR's own
recenter (hold Start, the system button).

**Fix drift:** open the PS4 menu (PS button), then go back to the app: the tracking is
reset, as in PSVR games.

**Tracking loss:** a Move hidden from the camera keeps its last position. SteamVR shows it
as "searching" after 10 s. A headset the camera has lost is shown as "searching", with
SteamVR's grey screen, after 3 s (needs the driver patch).

### Lobby settings

In the lobby, a short press on **START** (PS Move) or **OPTIONS** (DualShock 4) opens the
settings in front of you. Point at a button with a Move and pull the trigger, or with the
DualShock 4 (the laser comes out of its light bar) and press **✕**. Settings: your height
(places the floor), controller prediction, stream resolution and refresh rate (both at the
next launch), centring on the headset at SteamVR start, and Reset (click twice; the height
panel then comes back).

**Refresh rate:** 90 Hz (default) runs the PSVR at 90 Hz and asks the PC for 90 frames per
second. 60 Hz runs the PSVR at 120 Hz with each frame shown twice by the system, as most
PSVR games do; use it if 90 Hz is not smooth. With 60 Hz, the ALVR dashboard warns that the
preferred FPS (90) is not supported and uses 60: that is expected.

### DualShock 4 in the lobby

The DualShock 4 is tracked by its light bar, like the Moves, and shown in the lobby with
everything it reports: buttons, sticks, L2 / R2, fingers on the touchpad and vibration. In
the lobby, L2 and R2 drive its two motors, to try them. It is not sent to SteamVR.

The PS4 tracks only two controllers in all: the PS Moves come first, so with both Moves on
the DualShock 4 is not tracked. It is then shown still, in grey, in front of the play area
("Not tracked"), with its buttons still working (Options opens the settings). Controllers
of other users logged in on the console are shown with their user number, and tracked
when there is room.

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
| `client/src/main.cpp` | startup, main loop (90 or 60 Hz), lobby/video display, tracking uplink |
| `client/src/alvr_client.*` | ALVR 20.14.1 protocol: discovery, handshake, streams, video reassembly |
| `client/src/video.*` | hardware H.264 decoding (libSceVideodec2), NV12 → RGB |
| `client/src/foveation.*` | foveated encoding math (expansion of the squeezed eye edges), same as the official client |
| `client/src/audio.*` | game audio playback (libSceAudioOut) and microphone capture (libSceAudioIn) |
| `client/src/hmd.*`, `reproj.*`, `screen.*` | PSVR (libSceHmd) and the system reprojection |
| `client/src/tracker.*`, `camera.*`, `move.*`, `wand.*`, `pad.*`, `hid.*` | camera tracking (libSceVrTracker), PS Move, Vive wand emulation, DualShock 4, raw HID reports |
| `client/src/settings.*` | lobby settings panel and first launch height panel |
| `client/src/lobby.*` | software-rendered lobby |
| `docs/alvr-20.14.1-protocol.md` | the ALVR 20.14.1 wire protocol, as implemented |
| `tools/alvr_setup.py` | PC-side ALVR settings for this client |
| `tools/alvr_driver_patch.py` | ALVR 20.14.1 driver patches: menu is not system, headset and controllers "searching", late controller activation |
| `pc-setup/` | sources of the one-file PC setup (`setup.ps1`, settings template, SteamVR icons) |
| `tools/icons/make_icons.py` | draws the PSVR and PS Move SteamVR status icons (`pc-setup/icons`) |
| `tools/make_release.py` | builds `release/` (`ALVR-PS4-v<version>.pkg`, `ALVR-PS4-Setup.bat`) for the GitHub release |
| `tools/re/` | reverse-engineering helpers (headless Ghidra on dumped system modules) |
| `tools/video_bench.py` | video bench: encodes test clips with NVENC (ALVR's settings, one parameter changed per test), plays them on the PS4 (Dev build, TCP 9955) through the real decoder and conversion, and prints the timings |

The PS4 client's settings are stored in `/data/alvr-ps4/config.txt` on the console. Edit
it over GoldHEN's FTP server, then restart the app:

| Key | Default | Meaning |
| --- | --- | --- |
| `hostname` | random `NNNN.client` | the name the PS4 announces to ALVR |
| `resolution_percent` | `130` | resolution per eye, in percent of the PSVR panel (960×1080), 50–160. The PS4 decoder slows down sharply above about 1920×1088 per frame (both eyes): with foveated encoding at the recommended settings, 130% decodes a 1920×1056 frame. Without foveated encoding, use 100 |
| `controller_prediction_ms` | `0` | extra controller prediction on top of SteamVR's, 0–60 |
| `user_height_cm` | `0` (not set) | your height, set in the lobby (first launch wizard, then settings); `0` shows the wizard at the next launch (a settings Reset also shows it at once) |
| `camera_height_cm` | `0` (estimated) | height of the PS Camera above the floor, derived from your height |
| `center_on_steamvr_start` | `1` | `1`: the play area centre is placed on the headset each time SteamVR connects; `0`: only once, at the first tracking |
| `refresh_rate_hz` | `90` | `90`: PSVR at 90 Hz, 90 fps stream; `60`: PSVR at 120 Hz, 60 fps stream (each frame shown twice) |

## Credits

The PS Move status icon is drawn after the one of
[PSMoveSteamVRBridge](https://github.com/HipsterSloth/PSMoveSteamVRBridge) (Apache License 2.0).
