# Usage

[Back to the README](../README.md)

## Controls (PS Move as a Vive wand)

| Vive wand | Left Move | Right Move |
| --- | --- | --- |
| Trigger (analog) | T | T |
| Trackpad touch | hold **MOVE** and tilt the controller (roll = left/right, pitch = up/down); the touch starts at the pad centre | same |
| Trackpad click | **△** (+ tilt for the position) | **□** (+ tilt) |

These are the default trackpad buttons and behaviour; the settings can swap the touch and
click buttons of each hand and switch each button to the "Alternate" way (see
[Trackpad behavior](#lobby-settings) below).
| Grip | **○** | **✕** |
| Menu | **□** | **△** |
| System (SteamVR dashboard) | **START** | **START** |
| Back to the lobby (hold 1 s) | **✕** | **○** |

**SELECT** is never bound: the PS4 takes it for screenshots. **PS** opens the PS4 menu.
The Moves vibrate with the game's haptics (strength in the settings). In the lobby, they also
tick on button presses while the game is not shown.

**Back to the lobby while playing:** hold **✕** on the left Move or **○** on the right Move
(the buttons the Vive wand mapping leaves free; either one alone) for 1 second to switch
to the lobby, for example to open the settings; hold it again to go back to the game. In
the lobby the ALVR session stays open: SteamVR keeps the headset and the controllers
tracked, game audio and the microphone keep working, but the buttons are not sent to
SteamVR (it sees them released), the game's vibrations are ignored and the video is not
decoded. Back in the game, the picture comes back within a moment (the PC is asked for a
full new frame). Both ways the picture fades to black and back (0.3 s each). Coming back
from the game, the settings open by themselves (turn it off in the settings: "Open the
settings when back in the lobby"); closing them keeps you in the lobby.

**Play area centre:** each time SteamVR connects, the centre of the play area is placed
where the headset is; its direction stays the camera's (forward is towards the PS Camera)
and the floor height is unchanged. This needs ALVR's "Position recentering mode" and
"Rotation recentering mode" set to Disabled, as the setup does (otherwise ALVR turns the
play area to where the headset faced before a reconnection). Afterwards, use SteamVR's own
recenter (hold Start, the system button).

**Fix drift:** open the PS4 menu (PS button), then go back to the app, or click Reset
tracking at the bottom of the settings: the tracking is reset, as in PSVR games. A reset
makes the tracker estimate the camera's tilt again, which used to move the floor by a few
centimetres; the floor is now kept under you: your neck height just before the reset is
compared with the second after it, and the floor follows the difference (when you stayed
in place: less than 25 cm sideways and 12 cm up or down).

**Tracking loss:** a Move hidden from the camera is predicted, like a Quest 2 controller. For
the first moments it keeps the tracker's own estimate (fast swings out of view behave as
before). Lost within arm's reach of the head (1 m) with the headset on and the
controller in hand, it keeps that estimate as long as it is likely right: once your head has
moved, your body turned or the controller turned enough to have moved the hand by about
20 cm, an arm model takes over: the elbow stays fixed to your body (it follows your head's
position, and its turns beyond 35 degrees) and the controller pivots around it with its own
rotation; it never goes "searching". A controller put down (its motion sensors go still: a hand
always shakes it a little; or it stays quiet while you walk away from it) goes back to where the tracker's estimate last placed it and never goes
"searching"; picked up again, the above starts over. Lost further away, or with the headset off, it keeps
the tracker's position, and SteamVR shows it as "searching" only after 10 s and once it has
drifted more than 80 cm from where it was lost. Seen again, it glides to its tracked
position instead of jumping (within a few frames when it moves fast). A headset the camera has lost is shown as "searching", with
SteamVR's grey screen, after 3 s (needs the [driver patch](manual-setup.md#driver-patches)).
In the lobby, the surroundings (grid, texts, settings) then fade to black; the PS Camera and
the controllers stay shown where they are.

**Headset tracking not started** (at launch, or after the PS menu, until the camera sees the
headset): the lobby shows only the PS Camera, 1.8 m ahead, blinking blue and red. If it lasts
more than 10 seconds while the headset is worn, the PS4's own "confirm your position"
screen opens, once per launch. Worn comes from the headset's own sensor once it has said
"worn" in this launch (until then, from the headset moving).

## Lobby settings

In the lobby, a short press on **START** (PS Move) or **OPTIONS** (DualShock 4) opens the
settings in front of you (the information panel says which, for the controllers that are
on). Point at a button with a Move and pull the trigger, or with the
DualShock 4 (the laser comes out of its light bar) and press **✕**. Settings: your height
(places the floor), headset prediction, controller prediction, stream resolution and
refresh rate (both marked "restart required", in orange once changed), centring on the
headset at SteamVR start, vibration strength, performance overlay, opening the settings
when back in the lobby from the game, Reset tracking, and Reset settings (click twice; the
height panel then comes back). The rows scroll: pull the trigger on an empty spot of
the panel and drag up or down, grab the scroll bar on the right edge (or click its track to
jump there), or push the DualShock 4's right stick up or down. Close stays at the bottom.

**Your height** shows the height your headset is at now above the floor (it follows your
head: stand straight to read it). - / + change it by 1 cm from that value, which moves the
floor under the headset. At first launch the grid only appears once the height is set with
- / + or measured; Confirm without touching it places the floor for the height shown from
where the headset is at that moment. Nothing else is shown behind the first launch panel,
and while it is open the game stays paused in the lobby (for example after a settings Reset
while connected).

**Vibration:** a slider for the strength of the PS Move (and DualShock 4) vibrations, the
game's and the lobby's alike: fully right is the full strength (as before), fully left
("Off") turns them off. Releasing the slider gives a sample buzz at the new strength.

**Trackpad behavior (left and right):** for each hand, the Move button and the other
trackpad button (Triangle on the left, Square on the right) each have two settings:

* **Touch / Click:** which of the two touches the trackpad and which clicks it (by default
  the Move button touches and Triangle / Square clicks). They always differ: changing one
  changes the other. A click also touches.
* **Default / Alternate:** how that button places the point on the trackpad. Default: the
  point starts at the centre and follows the controller's tilt since the press. Alternate
  (like Rec Room's locomotion): the point is where the controller points, relative to the
  headset: straight ahead is the top of the pad, left is left, behind is the bottom. As
  games move you relative to the headset, turning your head while pointing the same way
  keeps you walking the same way. The point stays at the edge of the pad, except when
  pointing up: within 40 degrees of straight up it comes in towards the centre (reached
  pointing straight up). Pointing straight down keeps the last direction.

With both buttons held, the click button's way places the point.

**Performance overlay:** shows, below the centre of the view while playing, the console
(PS4 or PS4 Pro), the refresh rate and the bitrate received; the frame rate and the latency
(from the head position the frame was rendered for to its display); the decoding time (from
the frame received to the decoder's picture) and the wait after it (conversion, then until
the frame's turn for display); and, over the last
second, the frames never shown (Drops), shown twice (Repeats), arrived late (Late) and lost
on the network (Lost), in orange when there are some. It is off by default; go back to the
lobby while playing to turn it on or off.

**Height calibration:** instead of setting your height by hand, press **Calibrate** (in the
first launch panel or in the settings) and do one of the two things it shows:

* touch the floor with the ball of a PS Move and pull its trigger: the floor is placed
  exactly there (the DualShock 4's tracking is not precise enough for this).
  This needs the PS Camera to see the floor where the controller is, which is easier a bit
  further from the camera. If your camera does not see the floor at all (placed high, or
  tilted up), use the arm span below instead;
* stand straight, arms straight out to the sides at shoulder height with a PS Move in each
  hand, both pointing straight up (balls on top), and pull both triggers: your height is
  measured from your arm span. The Moves must point up: pointed sideways, the balls are
  ~10 cm further out each and the height comes out wrong.

The panel says under each picture whether the camera sees what it needs, and why an
attempt was refused. It then goes back to the height panel with the height found, which
can still be adjusted with - / +. Cancel goes back without changing anything.

*Surface test (development build only):* the calibration panel has a "Surface test" button.
Then each trigger pull logs the tracked height of that PS Move and the surface found under
it with the same offsets, and shows it on the panel, without changing anything: put the
Move's ball on a table the camera sees, then the Move lying on its side. Both must give the
same surface; with the table and the camera lens heights measured from the floor, camera
height - table height should match the "below the camera" value.

The settings are kept in the PS4's own save data, per user: Settings > Application Saved
Data Management shows them as "ALVR PS4 - Settings", where they can be copied, backed up or
deleted like a game save. Deleting them brings back the first launch height panel and a new
client name, which the PC must trust again.

**Refresh rate:** 90 Hz (default) runs the PSVR at 90 Hz and asks the PC for 90 frames per
second. 60 Hz runs the PSVR at 120 Hz with each frame shown twice by the system, as most
PSVR games do; use it if 90 Hz is not smooth, typically in a game the PC cannot run at 90
fps (the performance overlay's frame rate stays below 90, with many Repeats): at 90 Hz some
frames are then shown twice and others once, at 60 Hz every frame is shown exactly twice.
The decoder also holds frames for less time at 60 Hz. With 60 Hz, the ALVR dashboard warns
that the preferred FPS (90) is not supported and uses 60: that is expected.

## DualShock 4 in the lobby

The DualShock 4 is tracked by its light bar, like the Moves, and shown in the lobby with
everything it reports: buttons, sticks, L2 / R2, fingers on the touchpad and vibration. In
the lobby, L2 and R2 drive its two motors, to try them. It is not sent to SteamVR.

The PS4 tracks only two controllers in all: the PS Moves come first, so with both Moves on
the DualShock 4 is not tracked. It is then shown still, in grey, in front of the play area
("Not tracked"), with its buttons still working (Options opens the settings). Controllers
of other users logged in on the console are shown with their user number, and tracked
when there is room.

## Tracking Viewer on the PC

**ALVR PS4 Tracking Viewer** is a small Windows program, a single portable
`ALVR PS4 Tracking Viewer.exe` with nothing to install. The PC setup puts it in
`ALVR-PS4_PC-Streamer\` (start it from there, there is no shortcut); the .exe can also be
copied anywhere on its own.

It shows what the PS4 tracks, live, on the lobby's floor grid: the headset, the PS Moves
and the DualShock 4 with their pressed buttons, and the PS Camera. Something the camera
does not see turns grey, the headset's blue lights included. It works in the lobby and
during a game, so someone next to you can watch the tracking, or you can check the
camera's coverage of your play area.

At start it asks for the PS4's IP address, shown in the lobby's info panel. It remembers the
address in an `.ini` file next to the .exe, and it can also be given on the command line
(for a shortcut). The title bar says whether the PS4 answers. Without a PS4 (Cancel), it can
still play recordings.

| Mouse / key | Action |
| --- | --- |
| Left drag | turn around |
| Right drag (or Shift + left drag) | move |
| Wheel | zoom |
| Double-click or R | reset the camera |
| F2 | change the PS4 IP address |

The bar at the bottom records what you see and plays it back:

| Button | Key | Action |
| --- | --- | --- |
| Record / Stop | Ctrl+R | record the live tracking, stop |
| Play / Pause | Space | play the recording, pause |
| Live | L | back to the live tracking |
| Load... | Ctrl+O | open a recording (or drop a `.psvrdata` file on the window, or give it on the command line) |
| Save... | Ctrl+S | save the recording, where you choose |
| slider | Left / Right, Home | move through the recording (5 s back / forward, to the start) |

Recordings are `.psvrdata` files: every tracking state as it came from the PS4, about
20 KB a second. The camera stays free while a recording plays. Closing the window or
starting another recording asks first if the last one was not saved.

It talks to the ALVR PS4 app over UDP port 9955 and only receives while its window is
open. The two must be the same version: the title bar says so if they are not.
