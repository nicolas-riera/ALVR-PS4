# PC setup

[Back to the README](../README.md)

## Automatic setup (recommended, Windows only)

Install SteamVR from Steam first. Then double-click **`ALVR-PS4-Setup.bat`** (from the
Releases page) and accept the administrator prompt. It:

1. closes the ALVR dashboard and SteamVR if they run;
2. downloads ALVR streamer **20.14.1** into `ALVR-PS4_PC-Streamer\`, next to the .bat
   (skipped if already there; a folder named `alvr_streamer_windows` by an earlier setup is
   renamed; any other ALVR driver registered with SteamVR is unregistered);
3. patches its SteamVR driver (see [Driver patches](manual-setup.md#driver-patches); the
   download is checked by its SHA-256 first, and the original is kept as
   `driver_alvr_server.dll.orig`), and adds PSVR and PS Move status icons for SteamVR
   (`resources\icons`);
4. installs [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) for the microphone,
   unless Virtual Audio Cable or VB-Cable is already installed (even turned off: a cable
   turned off keeps ALVR's microphone off, and the setup says how to turn it back on);
5. writes the PS4 settings (see [the settings table](manual-setup.md#by-hand-in-the-dashboard-settings-tab))
   into ALVR's `session.json`, after backing up any existing one (a PS4 already trusted
   stays trusted);
6. opens the ALVR ports (9943-9944, UDP and TCP) in the Windows firewall;
7. registers the driver with SteamVR (and unregisters any other ALVR version, which would
   fight over the headset);
8. puts [ALVR PS4 Tracking Viewer](usage.md#tracking-viewer-on-the-pc) in
   `ALVR-PS4_PC-Streamer\` (no shortcut: start it from there), creates the "ALVR (PS4)"
   desktop shortcut and starts the dashboard (as your user, not as administrator).

It is safe to run again. To install into another folder, run it from a command prompt:
`ALVR-PS4-Setup.bat D:\VR`. To start over from scratch, run
`ALVR-PS4_PC-Streamer\ALVR-PS4-Reset.bat`: it deletes everything in that folder (ALVR, its
settings, logs) and installs it again with the default PS4 settings (the PS4 then has to be
trusted again). The file is built from `pc-setup/setup.ps1` and `pc-setup/alvr-ps4-session.json`
by `python tools/make_release.py`.

## Turning the virtual audio cable off

The virtual audio cable adds a speaker and a microphone to Windows. To get rid of them when
you do not need the PSVR microphone, run `ALVR-PS4_PC-Streamer\ALVR-PS4-Audio-Cable-Toggle.bat`:
it disables the cable (VB-Cable or Virtual Audio Cable) and turns ALVR's microphone off (ALVR
refuses to connect when its microphone device is missing). Run it again to enable both. It
only changes anything when you run it; the setup and the reset keep the cable as it is.

## Tracker mode (PS Moves with another headset)

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
SteamVR restarts, or turn off "Center on the headset at SteamVR start" in the lobby
settings. The PS Move buttons do nothing in this mode. The reset .bat or the setup brings
the normal mode back as well.

## Recommended: disable unused SteamVR add-ons

When the PS4 connects, the ALVR driver gives SteamVR only 1 second to activate each
controller. If SteamVR is still loading other drivers, the activation times out. The PS4
client waits 6 seconds after SteamVR starts before it connects, and the
[driver patch](manual-setup.md#driver-patches) keeps a controller that SteamVR activates
late, so this rarely matters any more. Disabling the drivers you do not use with ALVR still
makes SteamVR start faster and leaves less to go wrong. To do so, open
SteamVR → Settings → Startup / Shutdown → **Manage Add-ons**, then turn off Steam Link
(vrlink), Oculus, OculusTouchLink, SlimeVR, Amethyst, PSMoveServiceEx and similar add-ons.
