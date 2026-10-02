# ALVR PS4 - PC setup (PowerShell part of ALVR-PS4-Setup.bat).
#
# Installs the ALVR streamer 20.14.1 (the only version the PS4 client speaks) as
# ALVR-PS4_PC-Streamer, patches its SteamVR driver, installs a virtual audio cable for the
# PSVR microphone if none is present, applies the PS4 settings, opens the firewall ports
# and registers the driver.
# Safe to run again.
#
# A copy of the .bat is left in ALVR-PS4_PC-Streamer as ALVR-PS4-Reset.bat: run from
# there, it deletes everything in that folder and installs again from scratch (default
# settings, the PS4 has to be trusted again).
# Another copy, ALVR-PS4-Tracker-Mode.bat, switches between the normal mode (the PSVR is
# the SteamVR headset) and the tracker mode (the PSVR becomes a tracking reference and the
# PS Moves Vive trackers, next to another headset, aligned with OpenVR Space Calibrator).
# A third, ALVR-PS4-Audio-Cable-Toggle.bat, disables the virtual audio cable (VB-Cable or
# Virtual Audio Cable: an extra speaker and microphone in Windows) together with ALVR's
# microphone, or enables both again. Only when run by hand: nothing toggles it by itself.
# It also puts ALVR PS4 Tracking Viewer (a window showing what the PS4 tracks) in the
# streamer folder (no shortcut).
#
# Do not run this file directly: tools/make_release.py embeds it, together with the
# settings template (alvr-ps4-session.json), into release/ALVR-PS4-Setup.bat, a single double-clickable file. The batch header sets
# ALVR_PS4_BAT (its own path) and ALVR_PS4_DIR (optional install folder, first argument).

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar

$AlvrUrl = "https://github.com/alvr-org/ALVR/releases/download/v20.14.1/alvr_streamer_windows.zip"
$AlvrZipSize = 82981131
$DriverSha256 = "5B2DC0012254FA3C45268ED655C3F589B2D460A62907C620670CFB5D22A48FA8"
$VbCableUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip"
# Settings template, gzip + base64, and the SteamVR status icons (pc-setup/icons,
# "name:base64|name:base64|...") (filled in by tools/make_release.py).
$TemplateGz = "@@SESSION_TEMPLATE@@"
$IconsData = "@@ICONS@@"
# ALVR PS4 Tracking Viewer (companion/, built by tools/build_companion.sh), gzip + base64.
$ViewerGz = "@@TRACKING_VIEWER@@"
$ViewerName = "ALVR PS4 Tracking Viewer"
$StreamerName = "ALVR-PS4_PC-Streamer"
$ResetBatName = "ALVR-PS4-Reset.bat"
$TrackerBatName = "ALVR-PS4-Tracker-Mode.bat"
$TrackerSystemName = "PSMoves"   # tracking system of every ALVR device in tracker mode
$CableBatName = "ALVR-PS4-Audio-Cable-Toggle.bat"

# Reset mode: the .bat runs from inside an installed streamer folder under the reset name.
# Under the tracker mode or audio cable names, it switches the mode or toggles the cable
# instead. Any other name there (a renamed copy, the setup dropped into the folder) stops:
# the reset deletes the whole folder, so it only runs under its own name.
$BatDir = Split-Path -Parent $env:ALVR_PS4_BAT
$InStreamer = Test-Path (Join-Path $BatDir "ALVR Dashboard.exe")
$BatName = Split-Path -Leaf $env:ALVR_PS4_BAT
$ResetMode = $InStreamer -and ($BatName -ieq $ResetBatName)
$SwitchMode = $InStreamer -and ($BatName -ieq $TrackerBatName)
$CableMode = $InStreamer -and ($BatName -ieq $CableBatName)
if ($InStreamer -and -not ($ResetMode -or $SwitchMode -or $CableMode)) {
    Write-Host "This file is inside an ALVR streamer folder ($BatDir) under an unknown name." -ForegroundColor Red
    Write-Host "Run ALVR-PS4-Setup.bat from another folder, or use $ResetBatName, $TrackerBatName"
    Write-Host "or $CableBatName in this folder."
    Read-Host "Press Enter to close"
    exit
}
# A relative install folder is resolved now: once elevated, the current folder is System32.
$InstallDir = $env:ALVR_PS4_DIR
if ($InstallDir) { $InstallDir = [IO.Path]::GetFullPath($InstallDir) }
# Default: the folder holding the .bat (not the current directory, which is System32 once
# elevated); in reset mode, the folder holding the streamer folder.
if ($ResetMode) { $InstallDir = Split-Path -Parent $BatDir }
elseif (-not $InstallDir) { $InstallDir = $BatDir }

function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Info($text) { Write-Host "   $text" }
function Warn($text) { Write-Host "   $text" -ForegroundColor Yellow }

# --- Administrator rights (virtual cable driver, firewall) ---------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host "Asking for administrator rights..."
    try {
        if ($env:ALVR_PS4_DIR) {
            Start-Process -FilePath $env:ALVR_PS4_BAT -ArgumentList @("`"$InstallDir`"") -Verb RunAs
        } else {
            Start-Process -FilePath $env:ALVR_PS4_BAT -Verb RunAs
        }
    } catch {
        Write-Host "Administrator rights were not given: nothing was changed." -ForegroundColor Red
        Read-Host "Press Enter to close"
    }
    exit
}

# The ALVR dashboard is started as the signed-in user, not elevated like this script:
# SteamVR, which the dashboard launches, must not run as administrator.
function Start-Dashboard($dir) {
    Start-Process -FilePath "explorer.exe" -ArgumentList "`"$(Join-Path $dir 'ALVR Dashboard.exe')`""
}

# Driver folders registered with SteamVR (openvrpaths.vrpath, written by vrpathreg).
function Get-SteamVrDrivers {
    $file = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (-not (Test-Path $file)) { return @() }
    $j = [IO.File]::ReadAllText($file) | ConvertFrom-Json
    return @($j.external_drivers | Where-Object { $_ })
}

function Get-VrPathReg {
    $file = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (Test-Path $file) {
        $j = [IO.File]::ReadAllText($file) | ConvertFrom-Json
        foreach ($rt in @($j.runtime)) {
            $exe = Join-Path $rt "bin\win64\vrpathreg.exe"
            if (Test-Path $exe) { return $exe }
        }
    }
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $exe = Join-Path $steam "steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
        if (Test-Path $exe) { return $exe }
    }
    return $null
}

# Entries left by other tools can be malformed paths: they are simply not ALVR.
function Test-AlvrDriverFolder($dir) {
    try {
        $manifest = [IO.Path]::Combine($dir, "driver.vrdrivermanifest")
        if (-not [IO.File]::Exists($manifest)) { return $false }
        return ([IO.File]::ReadAllText($manifest) | ConvertFrom-Json).name -eq "alvr_server"
    } catch { return $false }
}

function Expand-Gz($b64) {
    $raw = New-Object IO.MemoryStream(, [Convert]::FromBase64String($b64))
    $gz = New-Object IO.Compression.GZipStream($raw, [IO.Compression.CompressionMode]::Decompress)
    $out = New-Object IO.MemoryStream
    $gz.CopyTo($out)
    return $out.ToArray()
}

function HexBytes($hex) { [byte[]]($hex -split '(..)' | Where-Object { $_ } | ForEach-Object { [Convert]::ToByte($_, 16) }) }

function Stop-AlvrAndSteamVr {
    foreach ($name in @("ALVR Dashboard", "vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrwebhelper", $ViewerName)) {
        $p = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($p) {
            Info "Stopping $name"
            $p | Stop-Process -Force
        }
    }
    Start-Sleep -Seconds 2
}

# --- Tracker mode ------------------------------------------------------------------------
function Prop($key, $value) { [pscustomobject]@{ key = [pscustomobject]@{ variant = $key }; value = $value } }

function Set-Field($obj, $name, $value) {
    if ($obj.PSObject.Properties[$name]) { $obj.$name = $value }
    else { $obj | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}

# Copy of a template extra_openvr_props list, with some values replaced.
function Copy-Props($list, $replace) {
    @($list | ForEach-Object {
        $v = $_.value
        if ($replace.ContainsKey($_.key.variant)) { $v = $replace[$_.key.variant] }
        Prop $_.key.variant $v
    })
}

# Tracker mode on: ALVR's tracking_ref_only (the PSVR is a tracking reference, no display)
# and the ViveTracker controller emulation. Every ALVR device reports the tracking system
# $TrackerSystemName, so that Space Calibrator can tell them from the other headset's
# devices (ALVR's trackers would otherwise say "lighthouse", like real Vive trackers).
# Off: the PS4 settings of the template (Vive wands, PSVR headset).
function Set-TrackerMode($session, $template, [bool]$on) {
    $h = $session.session_settings.headset
    $th = $template.session_settings.headset
    $h.tracking_ref_only = $on
    $h.controllers.content.emulation_mode.variant = if ($on) { "ViveTracker" } else { "ViveWand" }
    $headRep = @{}
    if ($on) { $headRep["TrackingSystemNameString"] = $TrackerSystemName }
    $h.extra_openvr_props.content = Copy-Props $th.extra_openvr_props.content $headRep
    if ($on) {
        $t = "{alvr_server}/icons/psmove_status"
        $h.controllers.content.extra_openvr_props.content = @(
            (Prop "TrackingSystemNameString" $TrackerSystemName),
            (Prop "CurrentUniverseIdUint64" "2"),
            (Prop "DeviceProvidesBatteryStatusBool" "true"),
            (Prop "DeviceIsWirelessBool" "true"),
            (Prop "NamedIconPathDeviceOffString" "${t}_off.png"),
            (Prop "NamedIconPathDeviceSearchingString" "${t}_searching.gif"),
            (Prop "NamedIconPathDeviceSearchingAlertString" "${t}_searching_alert.gif"),
            (Prop "NamedIconPathDeviceReadyString" "${t}_ready.png"),
            (Prop "NamedIconPathDeviceReadyAlertString" "${t}_ready_alert.png"),
            (Prop "NamedIconPathDeviceAlertLowString" "${t}_ready_low.png"),
            (Prop "NamedIconPathDeviceStandbyString" "${t}_standby.png"),
            (Prop "NamedIconPathDeviceStandbyAlertString" "${t}_standby_alert.png"),
            (Prop "NamedIconPathDeviceNotReadyString" "${t}_error.png"))
    } else {
        $h.controllers.content.extra_openvr_props.content = Copy-Props $th.controllers.content.extra_openvr_props.content @{}
    }
    # The driver's copy of these settings; ALVR restarts SteamVR once when it is stale.
    $o = $session.openvr_config
    if ($o) {
        Set-Field $o "tracking_ref_only" $on
        Set-Field $o "controller_is_tracker" $on
        Set-Field $o "_controller_profile" $(if ($on) { 41 } else { 40 })
    }
}

# SteamVR loads ALVR next to the other headset's driver only with activateMultipleDrivers.
function Enable-MultipleDrivers {
    $dirs = @()
    $vrpath = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (Test-Path $vrpath) { $dirs += @(([IO.File]::ReadAllText($vrpath) | ConvertFrom-Json).config) }
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) { $dirs += (Join-Path $steam "config") }
    $file = @($dirs | Where-Object { $_ } | ForEach-Object { Join-Path $_ "steamvr.vrsettings" } | Where-Object { Test-Path $_ }) | Select-Object -First 1
    if (-not $file) {
        Warn "steamvr.vrsettings not found: in SteamVR, turn on Settings > Startup / Shutdown > 'Activate multiple drivers'."
        return
    }
    $j = [IO.File]::ReadAllText($file) | ConvertFrom-Json
    if (-not $j.PSObject.Properties["steamvr"]) { $j | Add-Member -NotePropertyName "steamvr" -NotePropertyValue ([pscustomobject]@{}) }
    if ($j.steamvr.PSObject.Properties["activateMultipleDrivers"] -and $j.steamvr.activateMultipleDrivers -eq $true) {
        Info "SteamVR already loads multiple drivers"
        return
    }
    Copy-Item $file "$file.alvr-ps4-bak" -Force
    Set-Field $j.steamvr "activateMultipleDrivers" $true
    [IO.File]::WriteAllText($file, ($j | ConvertTo-Json -Depth 100), (New-Object Text.UTF8Encoding $false))
    Info "Turned on SteamVR's 'Activate multiple drivers' (backup: steamvr.vrsettings.alvr-ps4-bak)"
}

function Switch-TrackerMode {
    $Session = Join-Path $BatDir "session.json"
    if (-not (Test-Path $Session)) { throw "session.json not found in $BatDir. Run $ResetBatName first." }
    $on = -not [bool](([IO.File]::ReadAllText($Session) | ConvertFrom-Json).session_settings.headset.tracking_ref_only)
    Write-Host "ALVR PS4 - tracker mode" -ForegroundColor Green
    Write-Host ""
    if ($on) {
        Write-Host "Current mode: NORMAL (the PSVR is the SteamVR headset)."
        Write-Host ""
        Write-Host "This switches to the TRACKER mode, to use the PS Moves with another headset:"
        Write-Host "  - the PSVR becomes a tracking reference in SteamVR (no image in it);"
        Write-Host "  - the PS Moves become Vive trackers (feet by default, change the roles in"
        Write-Host "    SteamVR > Settings > Controllers > Manage Vive Trackers);"
        Write-Host "  - all of them use the tracking system '$TrackerSystemName': in OpenVR Space"
        Write-Host "    Calibrator, pick it as the target space and calibrate with a PS Move held"
        Write-Host "    against one of the other headset's controllers;"
        Write-Host "  - SteamVR's 'Activate multiple drivers' setting is turned on."
        Write-Host "  Keep ALVR PS4 running on the PS4 with the PSVR on. The play area centre is set"
        Write-Host "  where the PSVR is each time SteamVR connects: calibrate again after a restart."
    } else {
        Write-Host "Current mode: TRACKER (the PS Moves are Vive trackers next to another headset)."
        Write-Host ""
        Write-Host "This switches back to the NORMAL mode: the PSVR is the SteamVR headset again,"
        Write-Host "with the PS Moves as Vive wands."
    }
    Write-Host ""
    Write-Host "The ALVR dashboard and SteamVR will be closed."
    Read-Host "Press Enter to switch, or close this window to cancel"

    Step "Closing the ALVR dashboard and SteamVR if they run"
    Stop-AlvrAndSteamVr

    Step "ALVR settings"
    # Read again: the dashboard saves the settings when it closes.
    $s = [IO.File]::ReadAllText($Session) | ConvertFrom-Json
    $t = [Text.Encoding]::UTF8.GetString((Expand-Gz $TemplateGz)) | ConvertFrom-Json
    Set-TrackerMode $s $t $on
    $backup = "$Session.bak-" + (Get-Date -Format "yyyyMMdd-HHmmss")
    Copy-Item $Session $backup
    [IO.File]::WriteAllText($Session, ($s | ConvertTo-Json -Depth 100), (New-Object Text.UTF8Encoding $false))
    Info "Previous settings saved as $(Split-Path -Leaf $backup)"
    if ($on) {
        Info "Tracker mode on (tracking reference, Vive trackers, tracking system '$TrackerSystemName')"
        Step "SteamVR"
        Enable-MultipleDrivers
    } else {
        Info "Normal mode on (PSVR headset, Vive wands)"
    }

    Write-Host ""
    Write-Host "Done. Run this file again to switch back." -ForegroundColor Green
    Start-Dashboard $BatDir
    Read-Host "Press Enter to close"
}

# --- Virtual audio cable toggle -----------------------------------------------------------
# The cable's Windows endpoints (VB-Cable: "CABLE Input" / "CABLE Output", Virtual Audio
# Cable: "Line 1" speaker and microphone) are listed from the MMDevices registry, which also
# holds the disabled ones, and switched like the Sound control panel's Disable / Enable
# (IPolicyConfig::SetEndpointVisibility; registry DeviceState 0x10000001 = disabled). A PnP
# disable of the endpoint or of the driver device does not work: it did nothing, or failed.
# An endpoint marked disabled whose driver is no longer loaded (DeviceState 0x10000004, seen
# with Virtual Audio Cable turned off by other means) still counts as a cable turned off:
# the setup must not install another cable over it. Its driver device must still be there
# (disabled in Device Manager): uninstalling a cable leaves its endpoints in the registry
# with the same state, and those are ignored.
$CableDescs = @("VB-Audio Virtual Cable", "Virtual Audio Cable")
$MMDevices = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio"

function Get-AudioCables {
    $list = @()
    foreach ($flow in @(@("Render", "0"), @("Capture", "1"))) {
        foreach ($k in @(Get-ChildItem (Join-Path $MMDevices $flow[0]) -ErrorAction SilentlyContinue)) {
            $state = [int64](Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue).DeviceState
            $disabled = ($state -band 0x10000000) -ne 0
            $present = ($state -band 0xF) -eq 1
            # Present endpoints (4: not present, 8: unplugged), and absent ones marked disabled.
            if (-not $present -and -not ($disabled -and ($state -band 0xF) -eq 4)) { continue }
            $props = Get-ItemProperty (Join-Path $k.PSPath "Properties") -ErrorAction SilentlyContinue
            $desc = $props.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
            if ($CableDescs -notcontains $desc) { continue }
            if (-not $present) {
                # "{1}.ROOT\..." holds the driver device's instance id.
                $dev = "$($props.'{b3f8fa53-0004-438e-9003-51a46e139bfc},2')" -replace '^\{\d+\}\.', ''
                if (-not $dev -or -not (Get-PnpDevice -InstanceId $dev -ErrorAction SilentlyContinue | Where-Object { $_.Present })) { continue }
            }
            $list += [pscustomobject]@{
                Id = "{0.0.$($flow[1]).00000000}.$($k.PSChildName)"
                Name = "$($props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2') ($desc, $(if ($flow[1] -eq '0') { 'speaker' } else { 'microphone' }))"
                Preset = if ($desc -like "VB-Audio*") { "VBCable" } else { "VAC" }
                Enabled = $present -and -not $disabled
                Present = $present
            }
        }
    }
    $list
}

# Off: installed, with every endpoint disabled.
function Test-CablesOff($cables) {
    $cables.Count -gt 0 -and -not ($cables | Where-Object { $_.Enabled })
}

$PolicyConfigCs = @"
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("f8679f50-850a-41cf-9c72-430f290290c8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAlvrPs4PolicyConfig {
    [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr format);
    [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, int useDefault, IntPtr format);
    [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id);
    [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr endpointFormat, IntPtr mixFormat);
    [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, int useDefault, IntPtr defaultPeriod, IntPtr minPeriod);
    [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr period);
    [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
    [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
    [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr key, IntPtr value);
    [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr key, IntPtr value);
    [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string id, int role);
    [PreserveSig] int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string id, int visible);
}
[ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")]
class AlvrPs4PolicyConfigClient { }
public static class AlvrPs4Audio {
    public static int SetEndpointVisibility(string id, bool visible) {
        var pc = (IAlvrPs4PolicyConfig)new AlvrPs4PolicyConfigClient();
        return pc.SetEndpointVisibility(id, visible ? 1 : 0);
    }
}
"@

function Set-CableEndpoint($c, [bool]$on) {
    if (-not ("AlvrPs4Audio" -as [type])) { Add-Type -TypeDefinition $PolicyConfigCs }
    $hr = [AlvrPs4Audio]::SetEndpointVisibility($c.Id, $on)
    if ($hr -ne 0) {
        Warn ("Could not {0} {1} (error 0x{2:x8})" -f $(if ($on) { "enable" } else { "disable" }), $c.Name, $hr)
        return $false
    }
    Info "$(if ($on) { 'Enabled' } else { 'Disabled' }) $($c.Name)"
    return $true
}

# ALVR refuses the connection when its microphone device is missing, so the microphone
# follows the cable.
function Set-Microphone($session, [bool]$on, $preset) {
    $m = $session.session_settings.audio.microphone
    $m.enabled = $on
    if ($on -and $preset) { $m.content.devices.variant = $preset }
}

function Switch-AudioCable {
    Write-Host "ALVR PS4 - virtual audio cable" -ForegroundColor Green
    Write-Host ""
    $cables = @(Get-AudioCables)
    if ($cables.Count -eq 0) {
        Write-Host "No virtual audio cable (VB-Cable or Virtual Audio Cable) is installed."
        Read-Host "Press Enter to close"
        return
    }
    $on = Test-CablesOff $cables
    Write-Host "Current state: $(if ($on) { 'DISABLED' } else { 'ENABLED' })"
    foreach ($c in $cables) { Write-Host "  $($c.Name)" }
    Write-Host ""
    if ($on) {
        Write-Host "This enables the cable again (its speaker and microphone come back in Windows)"
        Write-Host "and turns ALVR's microphone back on (the PSVR microphone)."
    } else {
        Write-Host "This disables the cable (its speaker and microphone disappear from Windows)"
        Write-Host "and turns ALVR's microphone off: no PSVR microphone until you run this again."
    }
    Write-Host ""
    Write-Host "The ALVR dashboard and SteamVR will be closed if they run."
    Read-Host "Press Enter to continue, or close this window to cancel"

    Step "Closing the ALVR dashboard and SteamVR if they run"
    Stop-AlvrAndSteamVr

    Step "Virtual audio cable"
    $ok = $true
    $absent = @($cables | Where-Object { -not $_.Present })
    if ($on -and $absent.Count -gt 0) {
        foreach ($c in $absent) { Warn "Not present (its driver is not loaded): $($c.Name)" }
        Warn "Enable the cable's driver in Device Manager (or reinstall it), then run this again."
        $ok = $false
    }
    foreach ($c in @($cables | Where-Object { $_.Present -and $_.Enabled -ne $on })) { $ok = (Set-CableEndpoint $c $on) -and $ok }
    Start-Sleep -Milliseconds 500
    $left = @(Get-AudioCables | Where-Object { $_.Present -and $_.Enabled -ne $on })
    foreach ($c in $left) { Warn "Still $(if ($on) { 'disabled' } else { 'enabled' }): $($c.Name)" }
    if (-not $ok -or $left.Count -gt 0) {
        Warn "Some devices could not be changed: ALVR's microphone is left as it was."
        Read-Host "Press Enter to close"
        return
    }

    Step "ALVR microphone"
    $Session = Join-Path $BatDir "session.json"
    if (Test-Path $Session) {
        $s = [IO.File]::ReadAllText($Session) | ConvertFrom-Json
        # Keep the chosen preset if its cable is there, else take the installed one.
        $preset = $s.session_settings.audio.microphone.content.devices.variant
        if (-not ($cables | Where-Object { $_.Preset -eq $preset })) { $preset = $cables[0].Preset }
        Set-Microphone $s $on $preset
        [IO.File]::WriteAllText($Session, ($s | ConvertTo-Json -Depth 100), (New-Object Text.UTF8Encoding $false))
        Info "$(if ($on) { "Microphone on ($preset)" } else { 'Microphone off' })"
    } else {
        Info "No ALVR settings yet: nothing to change"
    }

    Write-Host ""
    Write-Host "Done. Run this file again to switch back." -ForegroundColor Green
    if ($on) {
        Write-Host "Check that Windows did not make the cable its default speaker: keep your real"
        Write-Host "speakers or headset as the output (ALVR refuses to connect otherwise)."
    }
    Read-Host "Press Enter to close"
}

try {
    if ($SwitchMode) {
        Switch-TrackerMode
        exit
    }
    if ($CableMode) {
        Switch-AudioCable
        exit
    }
    if ($ResetMode) {
        Write-Host "ALVR PS4 - reset" -ForegroundColor Green
        Write-Host ""
        Write-Host "This deletes everything in $BatDir (ALVR, its settings and logs) and installs it"
        Write-Host "again from scratch with the default PS4 settings. The PS4 will have to be trusted"
        Write-Host "again in the ALVR dashboard."
        Read-Host "Press Enter to reset, or close this window to cancel"
    } else {
        Write-Host "ALVR PS4 - PC setup" -ForegroundColor Green
    }

    # --- Close ALVR and SteamVR (their files are in use) -----------------------------
    Step "Closing the ALVR dashboard and SteamVR if they run"
    Stop-AlvrAndSteamVr

    # --- ALVR streamer 20.14.1 -------------------------------------------------------
    Step "ALVR streamer 20.14.1"
    Remove-Item (Join-Path $env:TEMP "alvr_streamer_windows_20.14.1.zip") -Force -ErrorAction SilentlyContinue   # cache of earlier setups
    $registered = @(Get-SteamVrDrivers | Where-Object { Test-AlvrDriverFolder $_ })
    $Streamer = Join-Path $InstallDir $StreamerName
    if ($ResetMode) { $Streamer = $BatDir }
    $oldStreamer = Join-Path $InstallDir "alvr_streamer_windows"   # name used by earlier setups
    if (-not (Test-Path $Streamer) -and (Test-Path (Join-Path $oldStreamer "ALVR Dashboard.exe"))) {
        Move-Item $oldStreamer $Streamer
        Info "Renamed $oldStreamer to $StreamerName"
    }
    if ($ResetMode) {
        # Everything goes, except this .bat (cmd still reads it).
        Get-ChildItem -Force $Streamer | Where-Object { $_.Name -ne $ResetBatName } | Remove-Item -Recurse -Force
        Info "Deleted the previous install"
    }
    # Installed means the dashboard and the driver are there (an interrupted extraction
    # is extracted again).
    if ((Test-Path (Join-Path $Streamer "ALVR Dashboard.exe")) -and
        (Test-Path (Join-Path $Streamer "bin\win64\driver_alvr_server.dll")) -and
        (Test-Path (Join-Path $Streamer "driver.vrdrivermanifest"))) {
        Info "Already installed in $Streamer"
    } else {
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
        # Downloaded every time and deleted once extracted (no cache left in TEMP).
        $zip = Join-Path $env:TEMP "alvr_streamer_windows_20.14.1.zip"
        Info "Downloading $AlvrUrl (83 MB)..."
        Invoke-WebRequest -Uri $AlvrUrl -OutFile $zip -UseBasicParsing
        if ((Get-Item $zip).Length -ne $AlvrZipSize) {
            Remove-Item $zip -Force
            throw "The ALVR download is incomplete, run the setup again."
        }
        Info "Extracting..."
        # The 20.14.1 zip has no top-level folder: extract straight into $Streamer, and
        # flatten a nested alvr_streamer_windows folder should a zip ever have one.
        Expand-Archive -Path $zip -DestinationPath $Streamer -Force
        Remove-Item $zip -Force
        $nested = Join-Path $Streamer "alvr_streamer_windows"
        if (-not (Test-Path (Join-Path $Streamer "ALVR Dashboard.exe")) -and (Test-Path (Join-Path $nested "ALVR Dashboard.exe"))) {
            Get-ChildItem -Force $nested | Move-Item -Destination $Streamer -Force
            Remove-Item $nested -Recurse -Force
        }
        if (-not (Test-Path (Join-Path $Streamer "ALVR Dashboard.exe"))) { throw "ALVR Dashboard.exe not found after extraction." }
        Info "Installed in $Streamer"
    }
    $Dashboard = Join-Path $Streamer "ALVR Dashboard.exe"

    # --- SteamVR driver patch (see tools/alvr_driver_patch.py for the details) -------
    Step "Patching the ALVR SteamVR driver"
    $Dll = Join-Path $Streamer "bin\win64\driver_alvr_server.dll"
    $Orig = "$Dll.orig"
    # (file offset, original bytes, patched bytes)
    $Patches = @(
        @(0xA79D73, "488d45f7", "488d45ff"),   # left menu: no longer also "system"
        @(0xA7AB07, "488d45f7", "488d45ff"),   # right menu
        @(0xAE2140, ("00" * 44), "c7842478010000c8000000448b531c4181fa0000fac37613c7842478010000c9000000c684247c01000000c3"),
        @(0xA77F7A, "c7842470010000c8000000", "e8c1a10600660f1f440000"),   # headset "searching" check
        @(0x220, "3f1dae00", "001eae00"),      # .text VirtualSize covers the checks
        @(0xAE2170, ("00" * 27), "418b461c3d0000fac3760bc74550c9000000c6455400e9182ef9ff"),
        @(0xA74D16, "e988020000", "e955d40600"),   # controller "searching" check (hand y < -500)
        @(0xA75AAA, "4180be2c01000000", "4180be2f01000000"),   # buttons work while searching
        @(0xA6E23F, "83bb3c01000001488bcf0f94c3", "83bb3c01000002488bcf0f95c3")   # controllers kept when SteamVR activates them after 1 s
    )
    $bytes = [IO.File]::ReadAllBytes($Dll)
    $patchedAll = $true
    foreach ($p in $Patches) {
        $want = HexBytes $p[2]
        for ($i = 0; $i -lt $want.Length; $i++) { if ($bytes[$p[0] + $i] -ne $want[$i]) { $patchedAll = $false } }
    }
    if ($patchedAll) {
        Info "Already patched"
    } else {
        $sha = (Get-FileHash -Algorithm SHA256 -Path $Dll).Hash
        if ($sha -ne $DriverSha256) {
            if ((Test-Path $Orig) -and (Get-FileHash -Algorithm SHA256 -Path $Orig).Hash -eq $DriverSha256) {
                $bytes = [IO.File]::ReadAllBytes($Orig)
            } else {
                throw "driver_alvr_server.dll is not the ALVR 20.14.1 one (SHA-256 $sha)."
            }
        } elseif (-not (Test-Path $Orig)) {
            Copy-Item $Dll $Orig
        }
        foreach ($p in $Patches) {
            $want = HexBytes $p[2]
            [Array]::Copy($want, 0, $bytes, $p[0], $want.Length)
        }
        [IO.File]::WriteAllBytes($Dll, $bytes)
        Info "Patched (original kept as driver_alvr_server.dll.orig)"
    }

    # --- SteamVR status icons (PSVR, PS Move) -----------------------------------------
    Step "PSVR and PS Move icons for SteamVR"
    # {alvr_server}/icons/... in the device properties = the driver's resources\icons.
    # SteamVR writes its gradient variants of them in that folder.
    $IconDir = Join-Path $Streamer "resources\icons"
    New-Item -ItemType Directory -Force -Path $IconDir | Out-Null
    # Variants SteamVR generated from earlier icons (name.<8 hex>.png/gif): made again.
    Get-ChildItem $IconDir -File | Where-Object { $_.Name -match '\.[0-9a-f]{8}\.(png|gif)$' } | Remove-Item -Force
    $n = 0
    foreach ($entry in $IconsData.Split("|")) {
        $name, $b64 = $entry.Split(":")
        [IO.File]::WriteAllBytes((Join-Path $IconDir $name), [Convert]::FromBase64String($b64))
        $n++
    }
    Info "$n icons in $IconDir"

    # --- Virtual audio cable for the PSVR microphone ---------------------------------
    Step "Virtual audio cable (PSVR microphone)"
    $endpoints = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq "OK" }
    $cables = @(Get-AudioCables)
    $micPreset = $null
    $micOn = $true
    if (Test-CablesOff $cables) {
        # Turned off with the toggle .bat: left off, and so is ALVR's microphone.
        $micPreset = $cables[0].Preset
        $micOn = $false
        Info "Virtual audio cable found, disabled: ALVR's microphone stays off (turn both on with $CableBatName)"
        if (-not ($cables | Where-Object { $_.Present })) {
            Warn "Its driver is not loaded either: enable it again in Device Manager (or reinstall it) to use the PSVR microphone"
        }
    } elseif ($endpoints | Where-Object { $_.FriendlyName -like "*Line 1*" }) {
        $micPreset = "VAC"
        Info "Virtual Audio Cable found (Line 1)"
    } elseif ($endpoints | Where-Object { $_.FriendlyName -like "*CABLE Output*" -or $_.FriendlyName -like "*CABLE Input*" }) {
        $micPreset = "VBCable"
        Info "VB-Audio Virtual Cable found"
    } else {
        Info "None found: installing VB-Audio Virtual Cable (free, donationware)..."
        $vbZip = Join-Path $env:TEMP "VBCABLE_Driver_Pack45.zip"
        $vbDir = Join-Path $env:TEMP "VBCABLE_Driver_Pack45"
        Invoke-WebRequest -Uri $VbCableUrl -OutFile $vbZip -UseBasicParsing
        Expand-Archive -Path $vbZip -DestinationPath $vbDir -Force
        $setup = Join-Path $vbDir "VBCABLE_Setup_x64.exe"
        if (-not (Test-Path $setup)) { throw "VBCABLE_Setup_x64.exe not found in the VB-Cable package." }
        Start-Process -FilePath $setup -ArgumentList "-i", "-h" -Wait
        Remove-Item $vbZip, $vbDir -Recurse -Force -ErrorAction SilentlyContinue
        $micPreset = "VBCable"
        Warn "VB-Cable installed. Windows may need a restart before the cable appears."
    }

    # --- ALVR settings ---------------------------------------------------------------
    Step "ALVR settings for the PS4"
    $Session = Join-Path $Streamer "session.json"
    if (Test-Path $Session) {
        $backup = "$Session.bak-" + (Get-Date -Format "yyyyMMdd-HHmmss")
        Copy-Item $Session $backup
        Info "Previous settings saved as $(Split-Path -Leaf $backup)"
    }
    $json = [Text.Encoding]::UTF8.GetString((Expand-Gz $TemplateGz))
    $json = $json.Replace('"variant": "VAC"', '"variant": "' + $micPreset + '"')
    # The PCs' trust of the PS4 (client_connections) is kept when the setup runs again;
    # only the reset starts from an empty session.
    $trusted = $null
    if (Test-Path $Session) {
        try { $trusted = ([IO.File]::ReadAllText($Session) | ConvertFrom-Json).client_connections } catch { $trusted = $null }
        if ($trusted -and @($trusted.PSObject.Properties).Count -eq 0) { $trusted = $null }
    }
    if (-not $micOn -or $trusted) {
        $s = $json | ConvertFrom-Json
        if (-not $micOn) { Set-Microphone $s $false $null }
        if ($trusted) {
            $s.client_connections = $trusted
            Info "Kept the trusted PS4 of the previous settings"
        }
        $json = $s | ConvertTo-Json -Depth 100
    }
    [IO.File]::WriteAllText($Session, $json, (New-Object Text.UTF8Encoding $false))
    Info "Applied (H.264, 90 fps, foveated encoding, Vive wands, PS Move buttons, game audio, microphone: $(if ($micOn) { $micPreset } else { 'off' }))"

    # --- Firewall --------------------------------------------------------------------
    Step "Firewall (ALVR ports 9943-9944)"
    foreach ($proto in @("UDP", "TCP")) {
        $rule = "ALVR PS4 ($proto 9943-9944)"
        & netsh advfirewall firewall delete rule name="$rule" | Out-Null
        & netsh advfirewall firewall add rule name="$rule" dir=in action=allow protocol=$proto localport=9943-9944 | Out-Null
        if ($LASTEXITCODE -ne 0) { Warn "Could not add the firewall rule $rule (netsh exit code $LASTEXITCODE)"; continue }
        Info "Rule added: $rule"
    }

    # --- SteamVR driver registration -------------------------------------------------
    Step "Registering the ALVR driver with SteamVR"
    $vrpathreg = Get-VrPathReg
    if ($vrpathreg) {
        # Two ALVR drivers would fight over the headset: other ALVR folders are unregistered.
        $mine = (Resolve-Path $Streamer).Path.TrimEnd('\')
        foreach ($dir in $registered) {
            if ($dir.TrimEnd('\') -ne $mine) {
                & $vrpathreg removedriver $dir | Out-Null
                Warn "Unregistered another ALVR driver: $dir"
            }
        }
        & $vrpathreg adddriver $Streamer | Out-Null
        if ($LASTEXITCODE -eq 0) { Info "Registered $Streamer" }
        else { Warn "vrpathreg could not register $Streamer (exit code $LASTEXITCODE): SteamVR will not load the driver" }
    } else {
        Warn "SteamVR not found: install SteamVR from Steam, then run this setup again."
    }

    # --- Reset and tracker mode .bat files (copies of this one) -----------------------
    Step "Reset and tracker mode tools"
    foreach ($name in @($ResetBatName, $TrackerBatName, $CableBatName)) {
        $dest = Join-Path $Streamer $name
        if ((Resolve-Path $env:ALVR_PS4_BAT).Path -ne $dest) { Copy-Item $env:ALVR_PS4_BAT $dest -Force }
    }
    Info "$ResetBatName reinstalls everything from scratch"
    Info "$TrackerBatName switches the PS Moves to Vive trackers for another headset (and back)"
    Info "$CableBatName disables the virtual audio cable and ALVR's microphone (and back)"

    # --- ALVR PS4 Tracking Viewer ------------------------------------------------------
    Step "ALVR PS4 Tracking Viewer"
    $Viewer = Join-Path $Streamer "$ViewerName.exe"
    [IO.File]::WriteAllBytes($Viewer, [byte[]](Expand-Gz $ViewerGz))
    Info "$Viewer shows what the PS4 tracks (headset, PS Moves, DualShock 4), and records it"

    # --- Shortcut and launch ---------------------------------------------------------
    Step "Desktop shortcut"
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "ALVR (PS4).lnk"
    $shell = New-Object -ComObject WScript.Shell
    $s = $shell.CreateShortcut($lnk)
    $s.TargetPath = $Dashboard
    $s.WorkingDirectory = $Streamer
    $s.Save()
    Info "Created $lnk"

    Write-Host ""
    Write-Host "Done. Next steps:" -ForegroundColor Green
    Write-Host "  1. Start ALVR PS4 on the PS4 (lobby with the PC connection state)."
    Write-Host "  2. In the ALVR dashboard, click Trust next to the PS4 (NNNN.client), then launch SteamVR."
    Write-Host "  3. In Windows sound settings, pick the cable's output as the microphone"
    Write-Host "     ('Line 1' or 'CABLE Output'), and keep your real speakers/headset as the output."
    Write-Host ""
    Start-Dashboard $Streamer
    Read-Host "Press Enter to close"
} catch {
    Write-Host ""
    Write-Host "Setup failed: $($_.Exception.Message)" -ForegroundColor Red
    # Line numbers are those of the .bat (Invoke-Expression runs the whole file).
    Write-Host "   at line $($_.InvocationInfo.ScriptLineNumber): $($_.InvocationInfo.Line.Trim())" -ForegroundColor DarkGray
    Write-Host "   setup file: $env:ALVR_PS4_BAT" -ForegroundColor DarkGray
    Read-Host "Press Enter to close"
    exit 1
}
