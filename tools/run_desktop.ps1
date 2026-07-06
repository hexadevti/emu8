# run_desktop.ps1 — launch the desktop (SDL+ImGui) build and optionally screenshot the composited
# window for offline verification. Env vars must be set in-process (the Git Bash "VAR=val ./exe"
# prefix does NOT reach the native .exe — only $env: from PowerShell does).
#
#   pwsh tools/run_desktop.ps1 -Platform apple2                      # just run (Ctrl-C to stop)
#   pwsh tools/run_desktop.ps1 -Platform c64 -Capture cap.png -At 120 -Quit   # screenshot frame 120 then exit
#   pwsh tools/run_desktop.ps1 -Platform pcxt    -Floppy dos-622-disk1.img    # boot PC-XT from a DOS floppy
#   pwsh tools/run_desktop.ps1 -Platform tiny386 -Hd DOSHDD.IMG               # boot tiny386 (i386) from a hard disk
# Valid -Platform values: apple2 c64 nes atari iigs msx sms pcxt tiny386
param(
  [string]$Platform = 'apple2',
  [string]$Sd       = 'C:/Users/lucia/repos/emu8/build/sdcard',
  [string]$Floppy   = '',          # boot floppy image (A:)  — PC-XT / tiny386 (else generic EMU_DISK)
  [string]$Hd       = '',          # boot hard-disk image (C:) — PC-XT / tiny386
  [string]$Capture  = '',          # output PNG path (relative to build/) — empty = no capture
  [int]   $At       = 120,         # frame to capture at
  [switch]$Quit,                   # exit right after the capture
  [int]   $TimeoutMs = 25000
)

$build = 'C:\Users\lucia\repos\emu8\build'
Set-Location $build

$env:EMU_PLATFORM = $Platform
$env:EMU_SD_DIR   = $Sd

# Route the boot disk(s) to the right per-platform env var so PC-XT / tiny386 boot INTO a disk on
# launch (they have no persisted EEPROM selection on a fresh run, so without this they boot blank).
# Paths are SD-relative names (mapped under -Sd) or absolute host paths.
switch ($Platform) {
  'pcxt'    { if ($Floppy) { $env:EMU_PCXT_A   = $Floppy }; if ($Hd) { $env:EMU_PCXT_C   = $Hd } }
  'tiny386' { if ($Floppy) { $env:EMU_T386_FDA = $Floppy }; if ($Hd) { $env:EMU_T386_HDA = $Hd } }
  default   { if ($Floppy) { $env:EMU_DISK     = $Floppy } }
}
if ($Capture) {
  $bmp = [System.IO.Path]::ChangeExtension($Capture, '.bmp')
  $env:EMU_UI_DUMP    = $bmp
  $env:EMU_UI_DUMP_AT = "$At"
  if ($Quit) { $env:EMU_UI_QUIT = '1' } else { Remove-Item Env:EMU_UI_QUIT -ErrorAction SilentlyContinue }
  if (Test-Path $bmp) { Remove-Item $bmp }
} else {
  Remove-Item Env:EMU_UI_DUMP -ErrorAction SilentlyContinue
}

$p = Start-Process -FilePath .\emu8.exe -PassThru -NoNewWindow `
       -RedirectStandardOutput run.log -RedirectStandardError run.err.log
if (-not $p.WaitForExit($TimeoutMs)) { $p.Kill(); Write-Host "TIMEOUT (killed)" }
else { Write-Host "exited code $($p.ExitCode)" }

if ($Capture -and (Test-Path $bmp)) {
  Add-Type -AssemblyName System.Drawing
  $img = [System.Drawing.Image]::FromFile((Resolve-Path $bmp))
  $img.Save((Join-Path $build $Capture), [System.Drawing.Imaging.ImageFormat]::Png)
  $img.Dispose()
  Remove-Item $bmp
  Write-Host "capture -> $build\$Capture"
}
