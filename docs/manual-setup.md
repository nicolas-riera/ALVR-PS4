# Manual setup

[Back to the README](../README.md)

Everything on this page is done by `ALVR-PS4-Setup.bat` (see [PC setup](pc-setup.md)). It
is only needed to set ALVR up by hand, or to check what the setup changed.

## Steps

1. Extract [ALVR streamer 20.14.1](https://github.com/alvr-org/ALVR/releases/tag/v20.14.1)
   (`alvr_streamer_windows.zip`) and start **ALVR Dashboard.exe**. Follow its setup wizard,
   which registers the SteamVR driver and adds the firewall rules.
2. Apply the [driver patches](#driver-patches): `python tools\alvr_driver_patch.py "C:\path\to\ALVR-PS4_PC-Streamer\bin\win64\driver_alvr_server.dll"`.
3. Apply the PS4 settings: `python tools\alvr_setup.py --mic "C:\path\to\ALVR-PS4_PC-Streamer\session.json"`,
   or set them by hand ([table below](#by-hand-in-the-dashboard-settings-tab)). The
   dashboard rewrites its settings while it runs, so the script waits until you close it.
   Leave out `--mic` without a virtual audio cable.
4. Copy `pc-setup\icons` to `ALVR-PS4_PC-Streamer\resources\icons` (the PSVR and PS Move
   status icons the settings refer to).

## Microphone

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

## By hand, in the dashboard (Settings tab)

| Setting | Value | Why |
| --- | --- | --- |
| Video → Preferred codec | **H264** | the PS4's decoder handles HEVC too, but more slowly |
| Video → Foveated encoding | **on**: center region width 0.5, height 0.5, center shift X 0, Y 0, horizontal and vertical edge ratio 2 | keeps the center of each eye at full resolution and squeezes the edges 2:1 (the lenses blur them anyway): at the default 130% resolution the PS4 decodes a 1920×1056 frame instead of 2496×1408, the size its decoder handles quickly. The client reads these values from the streamer, so other values work too; a smaller center or a higher edge ratio decodes faster but blurs more of the view |
| Video → Preferred FPS | **90** | the client offers only the rate set on the PS4 (90 by default; 60 is reprojected to 120 Hz) |
| Video → Encoder → Quality preset | **Quality** (NVENC: **P4**) | the fastest preset blurs text; the PC's GPU pays for it, not the PS4 |
| Video → Bitrate | **Constant, 80 Mbps** | the PS4 Pro decodes 90 fps up to about 130 Mbps (measured with `tools/video_bench.py`), but 100 Mbps was too much in use; 30 Mbps is too low for readable text |
| Headset → Extra OpenVR props | `TrackingSystemNameString` = `htc`, `ModelNumberString` = `PlayStation VR`, `ManufacturerNameString` = `Sony Interactive Entertainment`, `RenderModelNameString` = `generic_hmd`, `RegisteredDeviceTypeString` = `sony/psvr`, `DriverVersionString` = `20.14.1` | the "Custom" headset mode declares no identity at all; VRChat left the head at the origin |
| Headset → Controllers → Extra OpenVR props | also `CurrentUniverseIdUint64` = `2` | same tracking universe as the headset |
| Headset → Position recentering mode, Rotation recentering mode | **Disabled** (both) | the PS4 places the play area centre itself; the streamer recenters on its last head pose, which it keeps across reconnections, so the play area took the direction the headset faced at the previous disconnection |
| Audio → Game audio | **on** | played in the PSVR headphones |
| Audio → Microphone | **on**, devices **VAC**, only with a virtual audio cable (see [Microphone](#microphone)) | the PSVR microphone becomes a Windows microphone |
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

## Driver patches

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
