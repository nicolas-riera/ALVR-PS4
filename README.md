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
> vibration), the lobby, video (H.264 with foveated encoding, 90 Hz by
> default, or 60 Hz shown by the system at 120 Hz through reprojection), game audio and the
> microphone work.
> Mainly tested with VRChat and Beat Saber on a PS4 Pro.

## Documentation

| Page | Contents |
| --- | --- |
| [PC setup](docs/pc-setup.md) | what the setup .bat does, the audio cable toggle, tracker mode, SteamVR add-ons |
| [Manual setup](docs/manual-setup.md) | setting ALVR up by hand: microphone, every dashboard setting, the driver patches |
| [Usage](docs/usage.md) | PS Move controls, lobby settings, refresh rate, DualShock 4 |
| [Troubleshooting](docs/troubleshooting.md) | common problems and their fixes |
| [Development](docs/development.md) | building, deploying, logs, the source layout, the saved settings |
| [ALVR 20.14.1 protocol](docs/alvr-20.14.1-protocol.md) | the wire protocol, as implemented by the client |

## Requirements

| Side | What |
| --- | --- |
| PS4 | PS4 or PS4 Pro with [GoldHEN](https://github.com/GoldHEN/GoldHEN) (tested on a FAT and on a Pro, both firmware 11.00) |
| VR | PSVR (CUH-ZVR1 or ZVR2), PS Camera, *two PS Move controllers (optional)* |
| PC | Windows 10 or 11, SteamVR, **ALVR streamer 20.14.1 exactly** (the setup below downloads it) |
| Network | PC and PS4 on the same local network, PS4 on Ethernet (or at least 5 GHz Wi-Fi) |

The PS4 client implements the ALVR **20.14.1** protocol only. ALVR may change its protocol
between versions, so any other streamer version may not connect.

## 1. Install the app on the PS4

Both files come from the project's **Releases** page: `ALVR-PS4-v0.10.x.pkg` for the PS4 and
`ALVR-PS4-Setup.bat` for the PC.

1. Start GoldHEN on the PS4.
2. Copy `ALVR-PS4-v0.10.x.pkg` to the console, either to a USB drive or over GoldHEN's FTP
   server (port 2121, to `/data/pkg/`).
3. In GoldHEN, open **Package Installer** and install the package. The app **ALVR PS4**
   appears on the home screen.

## 2. Set up the PC

Install SteamVR from Steam first. Then double-click **`ALVR-PS4-Setup.bat`** (from the
Releases page) and accept the administrator prompt. It downloads ALVR streamer 20.14.1 into
`ALVR-PS4_PC-Streamer\` next to the .bat, patches and configures it for the PS4, installs a
virtual audio cable for the microphone, opens the firewall ports and creates an
"ALVR (PS4)" desktop shortcut. It is safe to run again.

Details, and the extra tools it installs, are in [PC setup](docs/pc-setup.md). To set
everything up by hand instead, see [Manual setup](docs/manual-setup.md).

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

The controls and the lobby settings are described in [Usage](docs/usage.md).

## Credits

The PSVR status icons in SteamVR are drawn by [leonmc330](https://github.com/leonmc330/).
The PS Move status icon is drawn after the one of
[PSMoveSteamVRBridge](https://github.com/HipsterSloth/PSMoveSteamVRBridge) (Apache License 2.0).
