@echo off
rem ALVR PS4 - PC setup: installs and configures ALVR streamer 20.14.1 for the PS4 client.
rem Keep this file next to setup.ps1 and alvr-ps4-session.json.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
if errorlevel 1 pause
