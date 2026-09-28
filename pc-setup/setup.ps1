# ALVR PS4 - PC setup.
# Installs the ALVR streamer 20.14.1 (the only version the PS4 client speaks), applies the
# PS4 settings, patches its SteamVR driver, installs a virtual audio cable for the PSVR
# microphone if none is present, opens the firewall ports and registers the driver.
# Run it through "Setup ALVR for PS4.bat". Safe to run again (it keeps a backup of the
# existing settings).

param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\ALVR-PS4")
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar

$AlvrUrl = "https://github.com/alvr-org/ALVR/releases/download/v20.14.1/alvr_streamer_windows.zip"
$AlvrZipSize = 82981131
$DriverSha256 = "5B2DC0012254FA3C45268ED655C3F589B2D460A62907C620670CFB5D22A48FA8"
$VbCableUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Template = Join-Path $Here "alvr-ps4-session.json"

function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Info($text) { Write-Host "   $text" }
function Warn($text) { Write-Host "   $text" -ForegroundColor Yellow }

# --- Administrator rights (virtual cable driver, firewall) ---------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host "Asking for administrator rights..."
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`"",
        "-InstallDir", "`"$InstallDir`"")
    exit
}

Write-Host "ALVR PS4 - PC setup" -ForegroundColor Green
Info "Install folder: $InstallDir"

# --- Close ALVR and SteamVR (their files are in use) ---------------------------------
Step "Closing the ALVR dashboard and SteamVR if they run"
foreach ($name in @("ALVR Dashboard", "vrmonitor", "vrserver", "vrcompositor")) {
    $p = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($p) {
        Info "Stopping $name"
        $p | Stop-Process -Force
    }
}
Start-Sleep -Seconds 2

# --- ALVR streamer 20.14.1 -----------------------------------------------------------
$Streamer = Join-Path $InstallDir "alvr_streamer_windows"
$Dashboard = Join-Path $Streamer "ALVR Dashboard.exe"
Step "ALVR streamer 20.14.1"
if (Test-Path $Dashboard) {
    Info "Already installed in $Streamer"
} else {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $zip = Join-Path $env:TEMP "alvr_streamer_windows_20.14.1.zip"
    if (-not (Test-Path $zip) -or (Get-Item $zip).Length -ne $AlvrZipSize) {
        Info "Downloading $AlvrUrl (83 MB)..."
        Invoke-WebRequest -Uri $AlvrUrl -OutFile $zip -UseBasicParsing
    }
    if ((Get-Item $zip).Length -ne $AlvrZipSize) { throw "The ALVR download is incomplete, run the setup again." }
    Info "Extracting..."
    Expand-Archive -Path $zip -DestinationPath $InstallDir -Force
    if (-not (Test-Path $Dashboard)) { throw "ALVR Dashboard.exe not found after extraction." }
    Info "Installed in $Streamer"
}

# --- SteamVR driver patch (see tools/alvr_driver_patch.py for the details) -----------
Step "Patching the ALVR SteamVR driver"
$Dll = Join-Path $Streamer "bin\win64\driver_alvr_server.dll"
$Orig = "$Dll.orig"
# (file offset, original bytes, patched bytes)
$Patches = @(
    @(0xA79D73, "488d45f7", "488d45ff"),   # left menu: no longer also "system"
    @(0xA7AB07, "488d45f7", "488d45ff"),   # right menu
    @(0xAE2140, ("00" * 44), "c7842478010000c8000000448b531c4181fa0000fac37613c7842478010000c9000000c684247c01000000c3"),
    @(0xA77F7A, "c7842470010000c8000000", "e8c1a10600660f1f440000"),   # headset "searching" check
    @(0x220, "3f1dae00", "001eae00")       # .text VirtualSize covers the check
)
function HexBytes($hex) { [byte[]]($hex -split '(..)' | Where-Object { $_ } | ForEach-Object { [Convert]::ToByte($_, 16) }) }
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

# --- Virtual audio cable for the PSVR microphone -------------------------------------
Step "Virtual audio cable (PSVR microphone)"
$endpoints = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq "OK" }
$micPreset = $null
if ($endpoints | Where-Object { $_.FriendlyName -like "*Line 1*" }) {
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
    $micPreset = "VBCable"
    Warn "VB-Cable installed. Windows may need a restart before the cable appears."
}

# --- ALVR settings -------------------------------------------------------------------
Step "ALVR settings for the PS4"
$Session = Join-Path $Streamer "session.json"
if (Test-Path $Session) {
    $backup = "$Session.bak-" + (Get-Date -Format "yyyyMMdd-HHmmss")
    Copy-Item $Session $backup
    Info "Previous settings saved as $(Split-Path -Leaf $backup)"
}
$json = [IO.File]::ReadAllText($Template)
$json = $json.Replace('"variant": "VAC"', '"variant": "' + $micPreset + '"')
[IO.File]::WriteAllText($Session, $json, (New-Object Text.UTF8Encoding $false))
Info "Applied (H.264, 60 fps, Vive wands, PS Move buttons, game audio, microphone: $micPreset)"

# --- Firewall ------------------------------------------------------------------------
Step "Firewall (ALVR ports 9943-9944)"
foreach ($proto in @("UDP", "TCP")) {
    $rule = "ALVR PS4 ($proto 9943-9944)"
    & netsh advfirewall firewall delete rule name="$rule" | Out-Null
    & netsh advfirewall firewall add rule name="$rule" dir=in action=allow protocol=$proto localport=9943-9944 | Out-Null
    Info "Rule added: $rule"
}

# --- SteamVR driver registration -----------------------------------------------------
Step "Registering the ALVR driver with SteamVR"
$steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
$vrpathreg = $null
if ($steam) { $vrpathreg = Join-Path $steam "steamapps\common\SteamVR\bin\win64\vrpathreg.exe" }
if ($vrpathreg -and (Test-Path $vrpathreg)) {
    & $vrpathreg adddriver $Streamer | Out-Null
    Info "Registered $Streamer"
} else {
    Warn "SteamVR not found: install SteamVR from Steam, then run this setup again."
}

# --- Shortcut and launch -------------------------------------------------------------
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
Start-Process -FilePath $Dashboard -WorkingDirectory $Streamer
Read-Host "Press Enter to close"
