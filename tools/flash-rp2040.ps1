# Flash a .uf2 to an RP2040 through its BOOTSEL mass-storage drive (RPI-RP2).
#
# If the drive is not already mounted, the running firmware is asked to reboot into BOOTSEL by
# opening its USB CDC port at 1200 baud (the arduino-pico "touch" reset, same as arduino-cli
# upload does). Then we wait for the RPI-RP2 volume and copy the image onto it; the chip reboots
# into the new firmware by itself once the copy completes.
#
# Used by the "Flash RP2040" task in .vscode/tasks.json.
param(
  [string]$Uf2  = "$PSScriptRoot\..\build-picocalc-rp2040\emu8.ino.uf2",
  [string]$Port = "COM5",
  [int]$TimeoutSec = 20
)
$ErrorActionPreference = "Stop"

function Get-BootDrive {
  Get-Volume -ErrorAction SilentlyContinue |
    Where-Object { $_.FileSystemLabel -eq "RPI-RP2" -and $_.DriveLetter } |
    Select-Object -First 1
}

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2 (build it first)" }
$Uf2 = (Resolve-Path $Uf2).Path

$vol = Get-BootDrive
if (-not $vol) {
  Write-Host "Resetting $Port into BOOTSEL (1200-baud touch)..."
  try {
    $sp = New-Object System.IO.Ports.SerialPort $Port, 1200
    $sp.DtrEnable = $true
    $sp.Open(); Start-Sleep -Milliseconds 100; $sp.Close()
  } catch {
    Write-Host "Could not open ${Port}: $($_.Exception.Message)"
    Write-Host "Hold BOOTSEL while plugging the board in, and this will pick the drive up."
  }
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while (-not ($vol = Get-BootDrive) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
  if (-not $vol) { throw "RPI-RP2 drive did not appear within $TimeoutSec s" }
}

$dest = "$($vol.DriveLetter):\"
Write-Host "Copying $Uf2 -> $dest"
Copy-Item -LiteralPath $Uf2 -Destination $dest
Write-Host "Done. The RP2040 reboots into the new firmware."
