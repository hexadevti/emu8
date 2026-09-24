# emu8 SD Manager

Browse, upload, download, rename and delete files on an emu8 board's internal microSD card over USB, without taking the card out. Use it for ROMs, BIOS files and disk images.

There are two clients. Both use the same [protocol](PROTOCOL.md) and talk to the server built into the firmware ([src/shared/sdserial.cpp](../../src/shared/sdserial.cpp)):

| | Where it runs | Needs |
|---|---|---|
| **Web app** ([web/](web/)) | Chrome, Edge or Opera on Windows, macOS, Linux, ChromeOS and Android (USB OTG). Installable as an app. | Node.js to serve it (`npm run dev`), or any static HTTPS host for the built `dist/` |
| **CLI** ([emu8sd.py](emu8sd.py)) | Windows, macOS, Linux; also for scripts | Python 3.8+ and `pip install pyserial` |

Firefox and Safari have no Web Serial API. On those browsers, use the CLI.

## Web app

The web app is a [Vite](https://vite.dev) project and needs **Node.js 20.19 or newer**. To run it:

```sh
cd tools/sdmanager/web
npm install        # first time only
npm run dev        # opens http://localhost:5180
```

Web Serial only works on pages served over `https://` or from `localhost`, so open the app via `localhost`, not your PC's LAN IP.

To publish or install it:

```sh
npm run build      # static site in tools/sdmanager/web/dist/
npm run preview    # serves dist/ on http://localhost:5180 to check it
```

`dist/` uses relative paths, so you can copy it to any HTTPS static host (GitHub Pages, Netlify, a NAS…). On the built version, the install icon in the address bar gives you a standalone window that also works offline. `npm run dev` skips the offline cache so edits reload instantly.

To use it:

1. Close any other program that holds the port (Arduino IDE serial monitor, PlatformIO, PuTTY).
2. Click **Connect** and pick the board's port:
   - CYD / JC4827W543: *USB-SERIAL CH340* or *CP210x*
   - PicoCalc, or a P4 in CDC mode: *USB Serial Device*
3. Work with the files:
   - **Browse** by clicking folders or using the breadcrumbs. The sidebar jumps to the ROM folders each emulator reads; a grey entry doesn't exist yet, and clicking it creates it.
   - **Upload** with the buttons, or drag files and whole folders onto the list. An upload is written to `name.part` and only replaces the real file once it has fully arrived.
   - **Download** a file by clicking its name. Selecting several items or a folder downloads one `.zip`.
   - **Rename (F2), delete (Del) and create folders** from the toolbar. Folders are deleted with their contents.
   - **Reboot** the board so the emulator picks up new ROMs.
4. On USB-UART boards (CYD, JC4827W543), **Speed** switches the link to 460800 or 921600 baud. That makes transfers about 4× or 8× faster. If the cable can't keep up, the app falls back to 115200 by itself.

The **Device log** tab shows what the board prints on the port between replies.

Expected throughput, estimated from the wire speed and not yet measured on hardware: under 10 KB/s at 115200 and several tens of KB/s at 921600. Native-USB boards are limited mostly by the SD card.

## CLI

```sh
pip install pyserial
python tools/sdmanager/emu8sd.py --port COM5 info
python tools/sdmanager/emu8sd.py --port COM5 --fast ls /roms
python tools/sdmanager/emu8sd.py --port COM5 --fast put Karateka.dsk /
python tools/sdmanager/emu8sd.py --port COM5 --fast put -r ./msx-roms /roms/msx
python tools/sdmanager/emu8sd.py --port COM5 get /roms/msx ./backup
python tools/sdmanager/emu8sd.py --port COM5 mv /old.dsk /new.dsk
python tools/sdmanager/emu8sd.py --port COM5 rm -r /games/old
python tools/sdmanager/emu8sd.py --port COM5 reboot
```

- On Linux the port is `/dev/ttyUSB0` or `/dev/ttyACM0`; on macOS it's `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`.
- Set `EMU8_PORT` to skip `--port`.
- `--fast` means 921600 baud; `--baud N` picks another rate.
- **Git Bash on Windows** rewrites arguments like `/roms` into Windows paths. Run `export MSYS_NO_PATHCONV=1` first, or use PowerShell or cmd.

## Notes

- The emulator keeps running while you transfer. Don't overwrite the disk image the running machine has mounted; switch images or reboot afterwards.
- On the CYD, opening the port may reset the board. Both clients wait for it to boot. While a session is active the board stops printing its log, and it resumes 10 s after the host goes quiet.
- Names follow FAT rules: no `\ / : * ? " < > |`.

## Tests

The firmware server also builds as a PC program that uses a folder as its "SD card" and stdin/stdout as its "serial port" ([host/sdserial_host.cpp](../../host/sdserial_host.cpp)). The protocol tests drive it on a clean line and on a noisy one, with log lines mixed in and corrupted replies:

```sh
g++ -O2 -std=c++17 -Wall -DSDSERIAL_HOST -Isrc/desktop/arduino_shim \
    -o sdserial_host host/sdserial_host.cpp src/shared/sdserial.cpp
node tools/sdmanager/test/protocol.test.mjs ./sdserial_host
```

The CLI can use the same harness in place of a port: `python tools/sdmanager/emu8sd.py --exec "./sdserial_host /tmp/sd" ls /`.
