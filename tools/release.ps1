# release.ps1 — build every emu8 deliverable and (optionally) publish them as GitHub releases.
#
# One release per component, each with its own tag so they can be versioned independently:
#   cyd-v<ver>          ESP32 CYD (ESP32-2432S024/028)            firmware .zip
#   jc4827w543-v<ver>   Guition JC4827W543 (ESP32-S3)             firmware .zip
#   jc1060p470-v<ver>   Guition JC1060P470 (ESP32-P4)             firmware .zip
#   picocalc-v<ver>     ClockworkPi PicoCalc (Pico 2 RP2350 + Pico RP2040)  .uf2 files
#   desktop-v<ver>      Windows desktop (SDL2) build              portable .zip
#   sdmanager-v<ver>    USB-serial SD card manager (web app + CLI) .zip
#
#   pwsh tools/release.ps1 -Version 1.0.0                         # build everything into dist/v1.0.0
#   pwsh tools/release.ps1 -Version 1.0.0 -Targets picocalc,desktop
#   pwsh tools/release.ps1 -Version 1.0.0 -Publish                # build + create the GitHub releases
#   pwsh tools/release.ps1 -Version 1.0.0 -Publish -SkipBuild     # publish what is already in dist/
#
# Needs the same one-time toolchain setup as the VS Code tasks (see README "Software prerequisites",
# src/shared/p4/README.md for the isolated P4 core, and MSYS2 MINGW32 with SDL2 for the desktop).
# -Publish needs `gh` logged in and HEAD pushed to origin (the tags are created on the HEAD commit).
param(
  [Parameter(Mandatory)] [string]$Version,
  [string[]]$Targets = @('cyd','jc4827w543','jc1060p470','picocalc','desktop','sdmanager'),
  [switch]$Publish,
  [switch]$SkipBuild,
  [switch]$Draft,
  [string]$Msys = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$Version = $Version.TrimStart('v')
$ALL = 'cyd','jc4827w543','jc1060p470','picocalc','desktop','sdmanager'
$Targets = @($Targets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # -File passes "a,b" as one string
foreach ($t in $Targets) { if ($ALL -notcontains $t) { throw "unknown target '$t' (valid: $($ALL -join ', '))" } }
$repo    = (Resolve-Path "$PSScriptRoot\..").Path
$out     = Join-Path $repo "dist\v$Version"
$work    = Join-Path $env:LOCALAPPDATA 'emu8-release'     # build trees, outside the sketch folder
New-Item -ItemType Directory -Force $out, $work | Out-Null

$ESP_BOARDS = @{
  cyd = @{
    Name = 'ESP32 CYD (ESP32-2432S024 / 2432S028)'
    Fqbn = 'esp32:esp32:esp32:PSRAM=disabled,PartitionScheme=huge_app,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,LoopCore=1,EventsCore=1,DebugLevel=none'
    Define = ''; Chip = 'esp32'; P4 = $false; BootAddr = '0x1000'
  }
  jc4827w543 = @{
    Name = 'Guition JC4827W543 (ESP32-S3)'
    Fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,CPUFreq=240,FlashMode=qio,FlashSize=4M,UploadSpeed=921600,USBMode=default,CDCOnBoot=default,DebugLevel=none'
    Define = '-DBOARD_JC4827W543'; Chip = 'esp32s3'; P4 = $false; BootAddr = '0x0'
  }
  jc1060p470 = @{
    Name = 'Guition JC1060P470 (ESP32-P4)'
    Fqbn = 'esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,PartitionScheme=huge_app,DebugLevel=none'
    Define = '-DBOARD_JC1060P470'; Chip = 'esp32p4'; P4 = $true
  }
}
$PICO_BOARDS = @(
  @{ Tag = 'rp2350'; Name = 'Pico 2 (RP2350)'
     Fqbn = 'rp2040:rp2040:rpipico2:arch=arm,os=freertos,freq=200,flash=4194304_65536,usbstack=picosdk,opt=Optimize2,exceptions=Enabled' }
  @{ Tag = 'rp2040'; Name = 'Pico (RP2040)'
     Fqbn = 'rp2040:rp2040:rpipico:os=freertos,freq=200,flash=2097152_65536,usbstack=picosdk,opt=Optimize2,exceptions=Enabled' }
)

function Invoke-Arduino([string]$Fqbn, [string]$Flags, [string]$BuildDir, [string]$OutDir, [hashtable]$Env = @{}) {
  $saved = @{}
  foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $Env[$k]) }
  try {
    if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
    $cmd = @('compile', '--fqbn', $Fqbn, '--build-path', $BuildDir, '--output-dir', $OutDir,
             '--build-property', 'compiler.optimization_flags=-O2')
    if ($Flags) {
      $cmd += @('--build-property', "compiler.cpp.extra_flags=$Flags", '--build-property', "compiler.c.extra_flags=$Flags")
    }
    & arduino-cli @cmd $repo
    if ($LASTEXITCODE) { throw "arduino-cli compile failed for $Fqbn" }
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
  }
}

function New-Zip([string]$Dir, [string]$Zip) {
  if (Test-Path $Zip) { Remove-Item -Force $Zip }
  # Windows' bsdtar, not Compress-Archive: PowerShell 5.1 stores entry names with backslashes, which
  # unzip on macOS/Linux turns into literal "dir\file" names instead of folders.
  & "$env:SystemRoot\System32\tar.exe" -a -cf $Zip -C $Dir *
  if ($LASTEXITCODE) { throw "zip failed: $Zip" }
  Write-Host "  -> $Zip"
}

# ---------------------------------------------------------------------------------------------
# Builders. Each returns the list of asset files to attach to its release.
# ---------------------------------------------------------------------------------------------
function Build-Esp([string]$key) {
  $b   = $ESP_BOARDS[$key]
  $bin = Join-Path $work "$key-out"
  $isoEnv = @{}
  if ($b.P4) {
    $isoEnv = @{ ARDUINO_DIRECTORIES_DATA = "$HOME\.emu6502-p4\data"; ARDUINO_DIRECTORIES_USER = "$HOME\.emu6502-p4\user" }
  }
  Invoke-Arduino $b.Fqbn $b.Define (Join-Path $work "$key-build") $bin $isoEnv

  $pkg = Join-Path $work "$key-pkg"
  if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
  New-Item -ItemType Directory $pkg | Out-Null
  $merged = Get-ChildItem $bin -Filter '*.merged.bin' | Select-Object -First 1
  if (-not $merged) {
    # Arduino-ESP32 2.0.x does not emit a merged image (3.x does), so build one with the core's own
    # esptool from the same four pieces, offsets and flash settings that `arduino-cli upload` writes.
    $core    = "$env:LOCALAPPDATA\Arduino15\packages\esp32"
    $esptool = Get-ChildItem "$core\tools\esptool_py" -Recurse -Filter 'esptool.exe' | Select-Object -First 1
    $bootApp = Get-ChildItem "$core\hardware\esp32" -Recurse -Filter 'boot_app0.bin' | Select-Object -First 1
    if (-not $esptool -or -not $bootApp) { throw "esptool.exe / boot_app0.bin not found under $core" }
    $mergedPath = Join-Path $bin 'emu8.ino.merged.bin'
    & $esptool.FullName --chip $b.Chip merge_bin -o $mergedPath --flash_mode dio --flash_freq 80m --flash_size 4MB `
        $b.BootAddr (Join-Path $bin 'emu8.ino.bootloader.bin') 0x8000 (Join-Path $bin 'emu8.ino.partitions.bin') `
        0xe000 $bootApp.FullName 0x10000 (Join-Path $bin 'emu8.ino.bin')
    if ($LASTEXITCODE) { throw "esptool merge_bin failed for $key" }
    $merged = Get-Item $mergedPath
  }
  foreach ($part in 'bootloader', 'partitions') {
    $f = Get-ChildItem $bin -Filter "*.$part.bin" | Select-Object -First 1
    if ($f) { Copy-Item $f.FullName (Join-Path $pkg "$part.bin") }
  }
  Copy-Item (Join-Path $bin 'emu8.ino.bin') (Join-Path $pkg 'emu8.ino.bin')
  @"
emu8 v$Version for $($b.Name)

Easiest: flash emu8-$key-v$Version-merged.bin (a separate download on the release page) at 0x0.
  esptool.py --chip $($b.Chip) --baud 921600 write_flash 0x0 emu8-$key-v$Version-merged.bin
or open https://espressif.github.io/esptool-js/ in Chrome/Edge, connect, add the merged .bin at
address 0x0 and click Program.

This zip holds the separate pieces: emu8.ino.bin is the application alone (for OTA, or for
flashing at 0x10000 over an existing huge_app layout); bootloader.bin / partitions.bin are the
matching bootloader and partition table.
"@ | Set-Content -Encoding utf8 (Join-Path $pkg 'FLASHING.txt')
  $zip = Join-Path $out "emu8-$key-v$Version-parts.zip"
  New-Zip $pkg $zip
  $mergedOut = Join-Path $out "emu8-$key-v$Version-merged.bin"
  Copy-Item $merged.FullName $mergedOut -Force
  return @($zip, $mergedOut)
}

function Build-PicoCalc {
  $assets = @()
  foreach ($p in $PICO_BOARDS) {
    $bin = Join-Path $work "picocalc-$($p.Tag)-out"
    $flags = "-DBOARD_PICOCALC -I$repo/src/picocalc/pico_shim"
    Invoke-Arduino $p.Fqbn $flags (Join-Path $work "picocalc-$($p.Tag)-build") $bin
    $uf2 = Join-Path $out "emu8-picocalc-$($p.Tag)-v$Version.uf2"
    Copy-Item (Join-Path $bin 'emu8.ino.uf2') $uf2 -Force
    Write-Host "  -> $uf2"
    $assets += $uf2
  }
  return $assets
}

function Build-Desktop {
  $bash  = Join-Path $Msys 'usr\bin\bash.exe'
  $bdir  = (Join-Path $work 'desktop-build') -replace '\\', '/'
  $src   = $repo -replace '\\', '/'
  $env:MSYSTEM = 'MINGW32'; $env:CHERE_INVOKING = '1'
  & $bash -lc "cmake -G 'Unix Makefiles' -S '$src' -B '$bdir' -DCMAKE_BUILD_TYPE=Release -DEMU_M32=OFF -DEMU_PORTABLE=ON && make -C '$bdir' -j8"
  if ($LASTEXITCODE) { throw 'desktop build failed' }

  $pkg = Join-Path $work 'desktop-pkg\emu8'
  if (Test-Path (Split-Path $pkg)) { Remove-Item -Recurse -Force (Split-Path $pkg) }
  New-Item -ItemType Directory -Force $pkg | Out-Null
  $bwin = Join-Path $work 'desktop-build'
  Copy-Item (Join-Path $bwin 'emu8.exe') $pkg
  Get-ChildItem $bwin -Filter '*.dll' | Copy-Item -Destination $pkg
  foreach ($d in 'roms\apple2', 'roms\c64', 'roms\coleco', 'roms\msx', 'roms\zxspectrum', 'roms\pcxt') {
    New-Item -ItemType Directory -Force (Join-Path $pkg "sdcard\$d") | Out-Null
  }
  @"
emu8 v$Version -- Windows desktop build (SDL2)

Run emu8.exe. The emulated SD card is the sdcard\ folder next to it: put disk/cartridge images
there (any sub-folder), and copy the system ROMs from the source tree's roms/ folder
(https://github.com/hexadevti/emu8/tree/main/roms) into sdcard\roms\ exactly as on a real card --
the Apple II, C64, ColecoVision, ZX Spectrum and PC-XT cores need them. Set EMU_SD_DIR to use a different folder, and
EMU_PLATFORM=<apple2|c64|nes|atari|msx|sms|coleco|zx|pcxt> to boot a system directly.

Settings persist to eeprom.bin / imgui.ini next to the .exe.
Keys: F10 settings + file browser, F11 / F12 platform reset / menu.

This build is primarily a debug/development target; the ESP32 / PicoCalc firmware is the main product.
"@ | Set-Content -Encoding utf8 (Join-Path $pkg 'README.txt')
  $zip = Join-Path $out "emu8-desktop-windows-v$Version.zip"
  New-Zip (Split-Path $pkg) $zip
  return @($zip)
}

function Build-SdManager {
  $web = Join-Path $repo 'tools\sdmanager\web'
  Push-Location $web
  try {
    & npm ci --no-audit --no-fund; if ($LASTEXITCODE) { throw 'npm ci failed' }
    & npm run build;               if ($LASTEXITCODE) { throw 'vite build failed' }
  } finally { Pop-Location }

  $pkg = Join-Path $work 'sdmanager-pkg\emu8-sdmanager'
  if (Test-Path (Split-Path $pkg)) { Remove-Item -Recurse -Force (Split-Path $pkg) }
  New-Item -ItemType Directory -Force $pkg | Out-Null
  Copy-Item -Recurse (Join-Path $web 'dist') (Join-Path $pkg 'web')
  foreach ($f in 'emu8sd.py', 'README.md', 'PROTOCOL.md') { Copy-Item (Join-Path $repo "tools\sdmanager\$f") $pkg }
  @"
emu8 SD Manager v$Version -- browse/upload/download files on the board's microSD over USB serial.

On the board: pick SD MGR on the system menu (boot splash); it reboots into the file server.

Web app (Chrome / Edge / Opera): web\ is a static site. Web Serial needs https:// or localhost, so
serve it locally, e.g.   python -m http.server 5180 --directory web   then open http://localhost:5180
(or host web\ on any HTTPS static host). Click Connect and pick the board's serial port.

CLI (any OS, Python 3.8+):   pip install pyserial
  python emu8sd.py --port COM5 info
  python emu8sd.py --port COM5 --fast put -r ./roms/msx /roms/msx

Full docs: README.md; wire format: PROTOCOL.md.
"@ | Set-Content -Encoding utf8 (Join-Path $pkg 'START-HERE.txt')
  $zip = Join-Path $out "emu8-sdmanager-v$Version.zip"
  New-Zip (Split-Path $pkg) $zip
  return @($zip)
}

# ---------------------------------------------------------------------------------------------
# Release notes
# ---------------------------------------------------------------------------------------------
$commit = (git -C $repo rev-parse HEAD).Trim()
$short  = $commit.Substring(0, 7)
$SDCARD = "**SD card:** FAT32. Copy the [``roms/``](https://github.com/hexadevti/emu8/tree/main/roms) folder from the source tree to the card root (the Apple II, C64, ColecoVision, ZX Spectrum and PC-XT cores load their system ROMs from ``/roms/<system>/``), then add your disk/cartridge images — see [microSD card preparation](https://github.com/hexadevti/emu8#microsd-card-preparation)."
$NOTES = @{
  cyd = @{ Title = "emu8 v$Version — ESP32 CYD"; Body = @"
Firmware for the **ESP32 Cheap Yellow Display** (ESP32-2432S024 2.4″ / ESP32-2432S028 2.8″, ILI9341, no PSRAM).

**Flash:** ``esptool.py --chip esp32 --baud 921600 write_flash 0x0 emu8-cyd-v$Version-merged.bin``, or load the merged ``.bin`` at address ``0x0`` in [esptool-js](https://espressif.github.io/esptool-js/) (Chrome/Edge).
The ``.zip`` also has the app-only ``emu8.ino.bin``, ``bootloader.bin`` and ``partitions.bin``.

Input: PS/2 keyboard + analog joystick; touch on-screen keyboard.

$SDCARD
"@ }
  jc4827w543 = @{ Title = "emu8 v$Version — Guition JC4827W543 (ESP32-S3)"; Body = @"
Firmware for the **Guition JC4827W543** (ESP32-S3, NV3041A 480×272 QSPI, OPI PSRAM).

**Flash:** hold **BOOT**, tap **RST**, release **BOOT**, then ``esptool.py --chip esp32s3 --baud 921600 write_flash 0x0 emu8-jc4827w543-v$Version-merged.bin`` (or use [esptool-js](https://espressif.github.io/esptool-js/) at address ``0x0``). Tap **RST** to run.

Input: USB SNES gamepad / USB keyboard on the native USB port, plus the touch on-screen keyboard.

$SDCARD
"@ }
  jc1060p470 = @{ Title = "emu8 v$Version — Guition JC1060P470 (ESP32-P4)"; Body = @"
Firmware for the **Guition JC1060P470** (ESP32-P4, JD9165 1024×600 MIPI-DSI, 32 MB PSRAM). Built on Arduino-ESP32 core 3.x.

**Flash:** ``esptool.py --chip esp32p4 --baud 921600 write_flash 0x0 emu8-jc1060p470-v$Version-merged.bin`` (or [esptool-js](https://espressif.github.io/esptool-js/) at address ``0x0``).

Input: GT911 touch + on-screen keyboard; USB keyboard/gamepad on the OTG USB-C port.

$SDCARD
"@ }
  picocalc = @{ Title = "emu8 v$Version — ClockworkPi PicoCalc"; Body = @"
Firmware for the **ClockworkPi PicoCalc**. Pick the file that matches the Pico module on your mainboard:

| File | Module |
| --- | --- |
| ``emu8-picocalc-rp2350-v$Version.uf2`` | Raspberry Pi **Pico 2** (RP2350) — all systems, including the experimental PC-XT |
| ``emu8-picocalc-rp2040-v$Version.uf2`` | Raspberry Pi **Pico** (RP2040) — no PC-XT (not enough RAM) |

**Flash:** hold **BOOTSEL** while powering on / plugging USB in, then copy the ``.uf2`` onto the ``RPI-RP2`` / ``RP2350`` drive. The board reboots into emu8 by itself.

Controls: ``Ctrl``+``F1`` settings, ``Ctrl``+``F6`` system menu, ``Ctrl``+``Shift``+``F1`` reboot — see the [README](https://github.com/hexadevti/emu8#controls).

$SDCARD
"@ }
  desktop = @{ Title = "emu8 v$Version — Windows desktop"; Body = @"
Portable **Windows** build of emu8 (SDL2 + Dear ImGui). The same emulator cores as the firmware, with the hardware swapped for a desktop window — mainly a development/debug target.

Unzip anywhere and run ``emu8\emu8.exe``. The emulated SD card is the ``sdcard\`` folder next to the exe (override with ``EMU_SD_DIR``); copy the source tree's [``roms/``](https://github.com/hexadevti/emu8/tree/main/roms) folder into it as ``sdcard\roms\`` (the Apple II, C64, ColecoVision, ZX Spectrum and PC-XT cores load their system ROMs from there). ``F10`` opens the settings / file browser.
"@ }
  sdmanager = @{ Title = "emu8 SD Manager v$Version"; Body = @"
Manage the board's **microSD card over USB serial** without removing it — browse, upload (drag & drop, folders), download (zip), rename, delete, reboot.

1. On the board pick **SD MGR** on the system menu; it reboots into the file server.
2. Either client from ``emu8-sdmanager-v$Version.zip``:
   - **Web app** (``web/``, Chrome / Edge / Opera): serve it on localhost, e.g. ``python -m http.server 5180 --directory web``, open http://localhost:5180 and click **Connect**. Installable as an app.
   - **CLI** (``emu8sd.py``, Python 3.8+ with ``pyserial``): ``python emu8sd.py --port COM5 --fast put -r ./roms/msx /roms/msx``

Works with every emu8 board (CYD, JC4827W543, JC1060P470, PicoCalc). Protocol: ``PROTOCOL.md``.
"@ }
}

# ---------------------------------------------------------------------------------------------
$assets = @{}
foreach ($t in $Targets) {
  Write-Host "=== $t ===" -ForegroundColor Cyan
  if ($SkipBuild) {
    $assets[$t] = @(Get-ChildItem $out | Where-Object { $_.Name -like "emu8-$t-*" -or ($t -eq 'sdmanager' -and $_.Name -like 'emu8-sdmanager-*') } |
                    ForEach-Object FullName)
    if (-not $assets[$t]) { throw "no built assets for $t in $out" }
    continue
  }
  switch ($t) {
    'picocalc'  { $assets[$t] = Build-PicoCalc }
    'desktop'   { $assets[$t] = Build-Desktop }
    'sdmanager' { $assets[$t] = Build-SdManager }
    default     { $assets[$t] = Build-Esp $t }
  }
}

if ($Publish) {
  git -C $repo fetch -q origin
  if (-not (git -C $repo branch -r --contains $commit)) { throw "HEAD $short is not pushed to origin; push first" }
  foreach ($t in $Targets) {
    $tag = "$t-v$Version"
    $n = $NOTES[$t]
    $notesFile = Join-Path $work "$t-notes.md"
    [IO.File]::WriteAllText($notesFile, $n.Body + "`n`n---`nBuilt from $short.")   # no BOM
    $ghArgs = @('release', 'create', $tag) + $assets[$t] + @('--title', $n.Title, '--notes-file', $notesFile, '--target', $commit)
    if ($Draft) { $ghArgs += '--draft' }
    Write-Host "Publishing $tag" -ForegroundColor Green
    & gh @ghArgs
    if ($LASTEXITCODE) { throw "gh release create $tag failed" }
  }
}
Write-Host "Done. Artifacts in $out"
