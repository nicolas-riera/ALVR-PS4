# Usage

[Back to the README](../README.md)

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
SteamVR's grey screen, after 3 s (needs the [driver patch](manual-setup.md#driver-patches)).

## Lobby settings

In the lobby, a short press on **START** (PS Move) or **OPTIONS** (DualShock 4) opens the
settings in front of you. Point at a button with a Move and pull the trigger, or with the
DualShock 4 (the laser comes out of its light bar) and press **✕**. Settings: your height
(places the floor), headset prediction, controller prediction, stream resolution and
refresh rate (both marked "restart required", in orange once changed), centring on the
headset at SteamVR start, and Reset (click twice; the height panel then comes back).

The settings are kept in the PS4's own save data, per user: Settings > Application Saved
Data Management shows them as "ALVR PS4 - Settings", where they can be copied, backed up or
deleted like a game save. Deleting them brings back the first launch height panel and a new
client name, which the PC must trust again.

**Refresh rate:** 90 Hz (default) runs the PSVR at 90 Hz and asks the PC for 90 frames per
second. 60 Hz runs the PSVR at 120 Hz with each frame shown twice by the system, as most
PSVR games do; use it if 90 Hz is not smooth. With 60 Hz, the ALVR dashboard warns that the
preferred FPS (90) is not supported and uses 60: that is expected.

## DualShock 4 in the lobby

The DualShock 4 is tracked by its light bar, like the Moves, and shown in the lobby with
everything it reports: buttons, sticks, L2 / R2, fingers on the touchpad and vibration. In
the lobby, L2 and R2 drive its two motors, to try them. It is not sent to SteamVR.

The PS4 tracks only two controllers in all: the PS Moves come first, so with both Moves on
the DualShock 4 is not tracked. It is then shown still, in grey, in front of the play area
("Not tracked"), with its buttons still working (Options opens the settings). Controllers
of other users logged in on the console are shown with their user number, and tracked
when there is room.
