# Put a .uf2 into the UF2 Loader's app folder on the PicoCalc's SD card, with no cable unplugged.
#
# The UF2 Loader (github.com/pelrun/uf2loader, v2.4.1+ for USB drive mode on Windows) shows the SD
# card to the PC as a USB drive while its menu is open. Getting there from whatever the Pico is
# doing, first thing that works wins:
#
#   1. The loader's drive is already mounted.
#   2. emu8 is running: send "@uf2menu" over its USB serial port (src/picocalc/loader_picocalc.cpp,
#      heard in every mode; Ctrl-Shift-Up on the keyboard does the same). Resent while the port is
#      still there, since a system switch restarts emu8 and the port comes and goes.
#   3. Anything else that answers the 1200-baud touch (a hung or older emu8): reboot into BOOTSEL
#      and reflash the loader itself. That leaves no valid app behind, so the loader boots straight
#      into its menu. The loader image is downloaded from its GitHub release once (GPL-3: not kept
#      in the repo) into tools\.cache.
#   4. Nothing on USB at all: a Windows rescan brings back a loader that was "safely removed"
#      earlier and is still sitting in its menu. Failing that, it asks for Ctrl-Shift-Up.
#
# Then the image is copied into pico1-apps, and the card is handed back to the menu with a "safely
# remove" of the whole USB device -- not an Eject: v2.4.1's SCSI eject handler is a stub, and it
# only leaves its "USB is connected" screen once the USB link is down (usb_msc_is_mounted() ->
# tud_ready(), ui/lib/usb_msc/usb_msc.c). Pick the file in the menu.
#
# With -RomsDir, the system ROMs in that folder (e.g. sdcard\roms) are also copied into \roms on
# the card, and the files the old per-system layout used (\roms\msx\cbios.rom, \roms\apple2\iie.bin,
# ...) are deleted, since the firmware no longer reads them. Nothing else on the card is touched.
#
# Used by the "Deploy RP2040 (SD)" task in .vscode/tasks.json.
param(
  [string]$Uf2       = "$PSScriptRoot\..\build-picocalc-rp2040\emu8.ino.uf2",
  [string]$Name      = "emu8.uf2",           # file name in the apps folder (what the menu lists)
  [string]$AppsDir   = "pico1-apps",         # pico2-apps for a Pico 2
  [string]$Port      = "",                   # emu8's USB serial port; found by USB id when empty
  [string]$LoaderUf2 = "$PSScriptRoot\.cache\uf2loader-v2.4.1\bootloader_pico.uf2",
  [string]$LoaderUrl = "https://github.com/pelrun/uf2loader/releases/download/v2.4.1/bootloader_pico.uf2",
  [string]$RomsDir   = "",                  # also copy these system ROMs into \roms on the card
  [int]$WaitSec      = 300,                 # how long to keep trying to reach the loader's drive
  [switch]$ReleaseOnly                      # copy nothing: just hand a mounted card back to the menu
)
$ErrorActionPreference = "Stop"

Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Uf2LoaderUsb {
  [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)] static extern int CM_Locate_DevNodeW(out uint dn, string id, int flags);
  [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)] static extern int CM_Request_Device_EjectW(uint dn, out int veto, StringBuilder name, int len, int flags);
  [DllImport("cfgmgr32.dll")] static extern int CM_Reenumerate_DevNode(uint dn, int flags);
  public static string Remove(string id) {
    uint dn; int r = CM_Locate_DevNodeW(out dn, id, 0);
    if (r != 0) return "CM_Locate_DevNode " + r;
    int veto; var sb = new StringBuilder(260);
    r = CM_Request_Device_EjectW(dn, out veto, sb, sb.Capacity, 0);
    return (r == 0 && veto == 0) ? null : ("CM_Request_Device_Eject " + r + ", veto " + veto + " " + sb);
  }
  public static void Rescan() {   // Device Manager's "Scan for hardware changes"
    uint dn; if (CM_Locate_DevNodeW(out dn, null, 0) == 0) CM_Reenumerate_DevNode(dn, 1 /* SYNCHRONOUS */);
  }
}
'@

function Get-LoaderDrive {   # the loader's card: the drive with its menu image in the root
  Get-Volume -ErrorAction SilentlyContinue |
    Where-Object { $_.DriveLetter -and ((Test-Path "$($_.DriveLetter):\BOOT2040.uf2") -or (Test-Path "$($_.DriveLetter):\BOOT2350.uf2")) } |
    Select-Object -First 1
}
function Get-BootselDrive {
  Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -eq "RPI-RP2" } | Select-Object -First 1
}
function Get-PicoPort {      # emu8 (arduino-pico): VID 2E8A
  if ($Port) { if ([System.IO.Ports.SerialPort]::GetPortNames() -contains $Port) { return $Port } else { return $null } }
  $d = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -match 'VID_2E8A' -and $_.FriendlyName -match '\(COM\d+\)' } | Select-Object -First 1
  if ($d -and $d.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
  return $null
}
function Wait-For([scriptblock]$what, [int]$sec) {
  $end = (Get-Date).AddSeconds($sec)
  do { $r = & $what; if ($r) { return $r }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end)
  return $null
}
function Send-Serial([string]$p, [int]$baud, [string]$text) {
  try {
    $sp = New-Object System.IO.Ports.SerialPort $p, $baud
    $sp.DtrEnable = $true
    $sp.Open()
    if ($text) { $sp.Write($text) }
    Start-Sleep -Milliseconds 200
    try { $sp.Close() } catch {}    # the device may already be gone
    return $true
  } catch { return $false }
}

function Release-Card {
  # the loader's USBSTOR disk -> the USB device above it, which is what gets removed
  $stor = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USBSTOR*' -and $_.InstanceId -match 'UF2LOADER' } | Select-Object -First 1
  if (-not $stor) { Write-Host "The loader's card is not mounted on this PC - nothing to release."; return }
  $usb = (Get-PnpDeviceProperty -InstanceId $stor.InstanceId -KeyName DEVPKEY_Device_Parent).Data
  Write-Host "Handing the card back to the loader"
  $err = $null
  for ($i = 0; $i -lt 5; $i++) {    # Explorer can hold the new drive open for a moment
    $err = [Uf2LoaderUsb]::Remove($usb)
    if (-not $err) { break }
    Start-Sleep -Seconds 1
  }
  if ($err) { throw "Could not release the card ($err). Close anything open on it and unplug the micro-USB cable instead." }
}

if ($ReleaseOnly) { Release-Card; Write-Host "Done. The loader menu on the PicoCalc takes keys again."; return }

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2 (build it first)" }
$Uf2 = (Resolve-Path $Uf2).Path

# --- get to the loader's drive -------------------------------------------------------------------
# One loop that watches for every state the Pico can be in, instead of one attempt at each in turn.
# The old sequence gave up on the first surprise: a port held open by a serial monitor, a hung emu8
# whose stale COM port Windows still lists, or a PicoCalc power-cycled after the script had
# already moved past "ask emu8". Now whatever shows up while we wait is acted on:
#   loader drive  -> done
#   BOOTSEL drive -> flash the UF2 Loader onto it (it then boots straight into its menu)
#   emu8's port   -> send "@uf2menu" (again every few seconds; retried if the port is busy)
#   port that never reacts -> 1200-baud touch into BOOTSEL, once
#   nothing       -> rescan USB now and then (a loader Windows "safely removed" earlier)
function Flash-Loader($boot) {
  if (-not (Test-Path $LoaderUf2)) {
    Write-Host "Downloading $LoaderUrl"
    New-Item -ItemType Directory -Force -Path (Split-Path $LoaderUf2) | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri $LoaderUrl -OutFile $LoaderUf2
  }
  Write-Host "Flashing the UF2 Loader ($($boot.DriveLetter):)"
  Copy-Item -LiteralPath $LoaderUf2 -Destination "$($boot.DriveLetter):\" -Force
}

$vol = Get-LoaderDrive
$end = (Get-Date).AddSeconds($WaitSec)
$portSince = $null; $lastAsk = [datetime]::MinValue; $lastScan = Get-Date; $touched = $false
$saidBusy = $false; $saidHelp = $false; $t0 = Get-Date
while (-not $vol -and (Get-Date) -lt $end) {
  $boot = Get-BootselDrive
  if ($boot) { Flash-Loader $boot; $vol = Wait-For { Get-LoaderDrive } 30; continue }

  $p = Get-PicoPort
  if ($p) {
    if (-not $portSince) { $portSince = Get-Date; Write-Host "emu8 is on $p - asking it to open the UF2 Loader menu..." }
    if (((Get-Date) - $lastAsk).TotalSeconds -ge 4) {
      $lastAsk = Get-Date
      if (-not (Send-Serial $p 115200 "`n@uf2menu`n")) {
        if (-not $saidBusy) { Write-Host "$p is busy (a serial monitor?) - close it; still retrying."; $saidBusy = $true }
      } elseif (Wait-For { -not (Get-PicoPort) } 3) {
        Write-Host "emu8 left - waiting for the loader to show the card..."
        $vol = Wait-For { Get-LoaderDrive } 30
        $portSince = $null
        continue
      }
    }
    # Listed but deaf for 15s: a hung emu8. The touch works while its USB stack still runs.
    if (-not $touched -and ((Get-Date) - $portSince).TotalSeconds -ge 15) {
      Write-Host "emu8 on $p does not answer - trying a reset into BOOTSEL..."
      [void](Send-Serial $p 1200 $null)
      $touched = $true
    }
  } else {
    $portSince = $null
  }

  if (-not $saidHelp -and ((Get-Date) - $t0).TotalSeconds -ge 20) {
    Write-Host "Still waiting. If the PicoCalc is frozen, switch it off and on (or press Ctrl-Shift-Up"
    Write-Host "in emu8); it is picked up as soon as it comes back. Waiting up to $WaitSec s."
    $saidHelp = $true
  }
  if (((Get-Date) - $lastScan).TotalSeconds -ge 10) { [Uf2LoaderUsb]::Rescan(); $lastScan = Get-Date }
  Start-Sleep -Milliseconds 300
  if (-not $vol) { $vol = Get-LoaderDrive }
}
if (-not $vol) { throw "The PicoCalc's SD card never showed up as a drive in $WaitSec s (is the micro-USB cable connected?)" }

# --- copy and hand the card back ------------------------------------------------------------------
$drive = "$($vol.DriveLetter):"
$destDir = "$drive\$AppsDir"
if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir | Out-Null }
Write-Host "Copying $Uf2 -> $destDir\$Name"
Copy-Item -LiteralPath $Uf2 -Destination "$destDir\$Name" -Force
# One name only: earlier deploys used emu8-dev.uf2, which left two emu8 entries in the menu.
foreach ($stale in 'emu8-dev.uf2','emu8.ino.uf2') {
  $path = "$destDir\$stale"
  if ($stale -ne $Name -and (Test-Path -LiteralPath $path)) { Write-Host "Deleting old $path"; Remove-Item -LiteralPath $path -Force }
}
if ($RomsDir) {
  $romDest = "$drive\roms"
  if (-not (Test-Path $romDest)) { New-Item -ItemType Directory -Path $romDest | Out-Null }
  Get-ChildItem -LiteralPath $RomsDir -File | ForEach-Object {
    Write-Host "Copying $($_.Name) -> $romDest"
    Copy-Item -LiteralPath $_.FullName -Destination $romDest -Force
  }
  # the pre-flat layout: system ROMs in per-system sub-folders under the old names
  $old = 'apple2\iiplus.bin','apple2\iie.bin','apple2\diskii.bin','apple2\hd.bin','apple2\mouse.bin',
         'c64\basic.bin','c64\kernal.bin','c64\chargen.bin',
         'msx\cbios.rom','msx\diskrom.rom','msx\diskrom.bin','diskrom.rom','cbios_main_msx1.rom',
         'coleco\coleco.rom','zxspectrum\spec48.rom','pcxt\bios.bin'
  foreach ($f in $old) {
    $path = "$romDest\$f"
    if (Test-Path -LiteralPath $path) { Write-Host "Deleting old $path"; Remove-Item -LiteralPath $path -Force }
  }
  foreach ($d in 'apple2','c64','msx','coleco','zxspectrum','pcxt') {
    $path = "$romDest\$d"
    if ((Test-Path -LiteralPath $path) -and -not (Get-ChildItem -LiteralPath $path -Force)) { Remove-Item -LiteralPath $path -Force }
  }
  Write-Host "Card \roms now holds:"; Get-ChildItem -LiteralPath $romDest | ForEach-Object { Write-Host "  $($_.Name)" }
}
Write-VolumeCache -DriveLetter $vol.DriveLetter

Release-Card
Write-Host "Done. Pick $Name in the loader menu on the PicoCalc."
