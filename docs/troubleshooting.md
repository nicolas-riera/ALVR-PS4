# Troubleshooting

[Back to the README](../README.md)

| Symptom | Fix |
| --- | --- |
| Lobby says "Waiting for the PC" | Is the dashboard running and the PS4 trusted? Are the PC and PS4 on the same network? Check the ALVR firewall rules (the dashboard's setup wizard adds them) |
| "SteamVR is restarting" at every connection | Normal once after changing the resolution or the foveated encoding settings. At every connection: the PS4 client is older than v0.9.3 (it did not support foveated encoding); update it or turn foveated encoding off |
| Stream stutters in busy scenes (log: `decode` above 10 ms) | Foveated encoding off, or a resolution too high for it: the decoded frame (log line `video: ... decoded frame WxH`) should stay within 1920×1088 |
| Stutters in one game only (performance overlay: frame rate below 90, many Repeats) | The PC does not render that game at 90 fps. Set Refresh rate to 60 Hz in the lobby settings (restart the app): every frame is then shown exactly twice |
| Controllers lag behind the hands | Settings > Controller prediction adds prediction on top of SteamVR's (try 10-20 ms). A stable network (Ethernet for the PS4 and the PC) lowers the jitter margin the PS4 waits for |
| The play area faces the wrong way after a reconnection | In the ALVR dashboard, Headset > Position recentering mode and Rotation recentering mode: Disabled (the setup sets them; run it again, the PS4 stays trusted) |
| Controllers greyed out / "standby" in SteamVR | Controller activation timed out: [disable unused SteamVR add-ons](pc-setup.md#recommended-disable-unused-steamvr-add-ons), then restart SteamVR |
| SteamVR shows Oculus Touch controllers or wrong bindings | Missing `InputProfilePathString` extra OpenVR prop (see [Manual setup](manual-setup.md#by-hand-in-the-dashboard-settings-tab)) |
| START does nothing | Button mappings not applied (run `tools/alvr_setup.py`) |
| Menu (□ / △) also opens the SteamVR dashboard | [Driver patch](manual-setup.md#driver-patches) not applied (run `tools/alvr_driver_patch.py`) |
| Wand drawn ahead of the Move | Controller position offset not set to 0, 0, 0 |
| The lobby stays although "Connected" | The codec must be H264 (HEVC works too, but decodes more slowly; AV1 does not). Look at the PS4 logs (development build, see [Logs](development.md#logs)) for `video:` lines |
| Height calibration: "The camera cannot see it there" | The PS Camera does not see the floor where the controller is: step further back from the camera, or use the arm span (T-pose) instead |
| Stuck in the lobby while the PC is connected ("the game is paused in the lobby") | Hold ✕ (left Move) or ○ (right Move) for 1 s to go back to the game |
| No sound in the headset | Game audio must be on, and the Windows output device must be at 48 kHz (the PS4 logs say "game audio at … Hz is not supported" otherwise) |
| "Game audio and microphone cannot point to the same device" | The virtual cable is the default Windows output: pick your real speakers/headset as default |
