# emu8 SD manager: serial protocol (version 1)

The firmware side is [src/shared/sdserial.cpp](../../src/shared/sdserial.cpp). The clients are [web/protocol.js](web/protocol.js) and [emu8sd.py](emu8sd.py).

The server runs as a low-priority FreeRTOS task on the board's normal serial port, next to whichever emulator is active. Until a host sends a valid frame, the port behaves as before: the boot log is printed and nothing else changes.

## Link

| Board | Link | Default speed | `BAUD` |
|---|---|---|---|
| CYD (ESP32) | USB-UART bridge | 115200 | yes (up to 2 Mbaud, 921600 recommended) |
| JC4827W543 (ESP32-S3) | USB-UART bridge¹ | 115200 | yes |
| JC1060P470 (ESP32-P4) | native USB CDC or bridge¹ | n/a | CDC: `UNSUPPORTED` |
| PicoCalc (RP2350) | native USB CDC | n/a | `UNSUPPORTED` |

¹ With `CDCOnBoot=enabled` the board uses native USB CDC. HELLO reports this in flag bit 1.

The link runs at 8N1 with no flow control. The host should keep DTR and RTS de-asserted: on ESP32 boards with an auto-reset circuit, toggling them resets the chip. Opening the port may still restart some boards, so a host should retry HELLO for several seconds.

## Frames

The same format is used in both directions. All integers are little-endian.

```
A5 5A | cmd u8 | seq u8 | len u16 | payload[len] | crc16 u16
```

- **crc16** is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR), computed over `cmd` through the end of `payload`.
- **Replies** echo the request `seq`, set `cmd | 0x80`, and put a **status byte** at `payload[0]`.
- **len** is at most the `maxPayload` reported by HELLO: 4096 on JC4827W543 and JC1060P470, 1024 on CYD and PicoCalc. Before HELLO, a client should assume 1024 for requests.
- **Log text between frames:** the device may print log lines between frames, for example from a core that calls `Serial.printf` directly. Clients skip every byte that is not part of a valid frame. Each reply leaves in a single write, so log text never lands inside a frame. While a session is live, `printLog()` is muted.
- **Device-side resync:** a partial frame followed by 200 ms of silence is discarded.

### Reliability

Only one request is in flight at a time. If no valid reply arrives within `1.2 s + wire time`, the client sends **the identical frame again** (same seq), up to 5 times. Every command is designed so that a resend is harmless:

- Reads and writes carry explicit offsets.
- `WRITE` at an earlier offset seeks back and overwrites.
- `MKDIR` succeeds if the folder already exists.
- `REMOVE` returns `NOT_FOUND` for something already gone, and clients treat that as success.
- A `WRCLOSE` that arrives after the file was already closed returns `OK`.
- `RENAME` returns `OK` when `from` is gone and `to` exists.

A corrupted length field can make a client wait for bytes that will never come. So on timeout the client also drops the first buffered byte and rescans the buffer.

### Session

- The first valid frame starts a session. 10 s without a valid frame ends it. `BYE` ends it at once.
- When a session ends, open files are closed and an unfinished upload is discarded. On a UART link the speed also returns to 115200.

### Status codes

| | | |
|---|---|---|
| 0 | OK | |
| 1 | FAIL | operation refused (e.g. rename target exists, folder not empty) |
| 2 | NOT_FOUND | |
| 3 | IO | SD read/write error |
| 4 | BAD_ARG | malformed payload, bad path (`..`, over 512 bytes), offset past end |
| 5 | NO_SD | no card mounted (every command except HELLO/BYE/REBOOT/BAUD) |
| 6 | NOT_OPEN | READ/WRITE with no open file |
| 7 | UNKNOWN_CMD | |
| 8 | UNSUPPORTED | not available on this board |

## Commands

Paths are absolute UTF-8 strings such as `/roms/msx/cbios.rom`, without a trailing NUL unless noted. `..` segments are rejected.

| cmd | name | request payload | reply payload (after status) |
|---|---|---|---|
| 0x01 | HELLO | — | `version u8`, `flags u8`, `maxPayload u16`, `board name` (rest) |
| 0x02 | FSINFO | — | `totalBytes u64`, `usedBytes u64` |
| 0x10 | LIST | `start u16`, `path` | `count u8`, `more u8`, then `count` × entry |
| 0x11 | STAT | `path` | `isDir u8`, `size u32` |
| 0x20 | RDOPEN | `path` | `size u32` |
| 0x21 | READ | `offset u32`, `len u16` | `data` (shorter than `len` at end of file) |
| 0x22 | RDCLOSE | — | — |
| 0x30 | WROPEN | `path` | — |
| 0x31 | WRITE | `offset u32`, `data` | `position u32` (file position after the write) |
| 0x32 | WRCLOSE | `commit u8` | — |
| 0x40 | MKDIR | `path` | — (OK if it already exists) |
| 0x41 | REMOVE | `path` | — (file, or empty folder) |
| 0x42 | RENAME | `from` `00` `to` | — (FAIL if `to` exists; OK if already done) |
| 0x50 | BAUD | `baud u32` | — |
| 0x60 | REBOOT | — | — (board restarts right after the reply) |
| 0x7F | BYE | — | — |

**HELLO flags:**
- bit 0: SD card mounted.
- bit 1: native USB link (the speed setting has no effect).
- bit 2: `BAUD` is supported.

**LIST entry:** `flags u8` (bit 0 = folder), `size u32`, `nameLen u8`, `name`.
- `start` is the number of entries the client already has. The device keeps the folder open between pages, so asking for `start = previous count` continues cheaply. Any other `start`, including a resend, reopens the folder and skips ahead.
- Keep requesting while `more` is 1.
- The order is whatever the SD library returns (unsorted). `.` and `..` are never listed.

**Upload:** `WROPEN` creates `<path>.part`. `WRITE` chunks carry offsets, and the reply's `position` confirms them. `WRCLOSE 1` renames `.part` over `<path>`, replacing any old file. `WRCLOSE 0` deletes the `.part`. Only one upload and one download can be open at a time. Clients hide leftover `*.part` files from listings.

**Download:** `RDOPEN` returns the size. `READ` any `offset`/`len` (len ≤ maxPayload − 1). `RDCLOSE` ends the download.

**BAUD** (UART boards only):
1. The device acknowledges at the **old** rate, waits about 20 ms and switches.
2. The client reopens its port at the new rate and sends HELLO.
3. If no valid frame arrives within 4 s, the device falls back to 115200. The client can then reopen at 115200, wait more than 4 s and send HELLO again.

## Firmware integration

- `sdSerialSetup()` is called at the end of `setup()` in `emu8.ino`. It starts the task (core 0, priority 1, 4 KB stack). The frame buffers (about 2 × maxPayload) are allocated from the heap on first use.
- SD access goes through `busTake()`/`busGive()`, the same lock the emulator cores use.
- The emulator keeps running during a session. Avoid overwriting a disk image that the running machine has mounted.
