#!/usr/bin/env python3
"""emu8sd - command-line client for the emu8 SD file manager (firmware: src/shared/sdserial.cpp).

Works on Windows, macOS and Linux with Python 3.8+ and pyserial (pip install pyserial).
Same protocol as the web app in web/; see PROTOCOL.md.

  emu8sd.py --port COM5 info
  emu8sd.py --port /dev/ttyUSB0 --fast ls /roms
  emu8sd.py --port COM5 put Karateka.dsk /                 # into a folder
  emu8sd.py --port COM5 put -r ./roms/msx /roms            # a whole folder
  emu8sd.py --port COM5 get /roms/msx .                    # folders download recursively
  emu8sd.py --port COM5 mkdir /games/nes
  emu8sd.py --port COM5 mv /old.dsk /new.dsk
  emu8sd.py --port COM5 rm -r /games/old
  emu8sd.py --port COM5 reboot
"""
import argparse
import os
import queue
import struct
import subprocess
import sys
import threading
import time

CMD = dict(HELLO=0x01, FSINFO=0x02, LIST=0x10, STAT=0x11, RDOPEN=0x20, READ=0x21, RDCLOSE=0x22,
           WROPEN=0x30, WRITE=0x31, WRCLOSE=0x32, MKDIR=0x40, REMOVE=0x41, RENAME=0x42,
           BAUD=0x50, REBOOT=0x60, BYE=0x7F)
STATUS_TEXT = ['OK', 'Operation failed', 'Not found', 'SD card I/O error', 'Bad request',
               'No SD card mounted', 'No file open', 'Unknown command', 'Not supported on this board']
NOT_FOUND = 2
DEFAULT_BAUD = 115200
FAST_BAUD = 921600


class DeviceError(Exception):
    def __init__(self, status, what):
        text = STATUS_TEXT[status] if status < len(STATUS_TEXT) else f'status {status}'
        super().__init__(f'{what}: {text}')
        self.status = status


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def join_path(d, name):
    return (d if d.endswith('/') else d + '/') + name


# ---------------------------------------------------------------------------------------------
# Transports: write(bytes), read(timeout_s) -> bytes (may be empty), set_baud(baud), close()
# ---------------------------------------------------------------------------------------------
class SerialTransport:
    def __init__(self, port, baud):
        try:
            import serial  # imported lazily so --help and --exec work without pyserial
        except ImportError:
            sys.exit('pyserial is required: pip install pyserial')
        # dsrdtr/rtscts off and DTR/RTS released so opening the port does not hold the ESP32 in reset.
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = baud
        self.s.timeout = 0.05
        self.s.dtr = False
        self.s.rts = False
        self.s.open()

    def write(self, b):
        self.s.write(b)

    def read(self, timeout):
        self.s.timeout = timeout
        n = max(1, self.s.in_waiting)
        return self.s.read(n)

    def set_baud(self, baud):
        self.s.flush()
        self.s.baudrate = baud
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()


class ExecTransport:
    """Talks to a program over its stdin/stdout (e.g. the host test harness sdserial_host)."""

    def __init__(self, command):
        self.p = subprocess.Popen(command, shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            b = self.p.stdout.read1(65536) if hasattr(self.p.stdout, 'read1') else self.p.stdout.read(1)
            if not b:
                break
            self.q.put(b)

    def write(self, b):
        self.p.stdin.write(b)
        self.p.stdin.flush()

    def read(self, timeout):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return b''

    def set_baud(self, baud):
        pass

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


# ---------------------------------------------------------------------------------------------
# Protocol client (mirror of web/protocol.js)
# ---------------------------------------------------------------------------------------------
class Link:
    def __init__(self, transport, baud=DEFAULT_BAUD, log=None):
        self.t = transport
        self.baud = baud
        self.log = log
        self.max_payload = 1024
        self.info = None
        self.seq = 0
        self.rx = bytearray()
        self.log_text = bytearray()
        self.frames = []

    # ---- receive ----
    def _log_byte(self, c):
        if c == 10:
            if self.log and self.log_text.strip():
                self.log(self.log_text.decode('ascii', 'replace'))
            self.log_text.clear()
        elif c != 13 and 32 <= c < 127:
            self.log_text.append(c)

    def _scan(self):
        b = self.rx
        i = 0
        cap = self.max_payload if self.info else 8192
        while i < len(b):
            if b[i] != 0xA5 or (i + 1 < len(b) and b[i + 1] != 0x5A):
                self._log_byte(b[i]); i += 1; continue
            if len(b) - i < 6:
                break
            n = b[i + 4] | (b[i + 5] << 8)
            if n > cap:
                self._log_byte(b[i]); i += 1; continue
            if len(b) - i < 8 + n:
                break
            body = bytes(b[i + 2:i + 6 + n])
            if crc16(body) != (b[i + 6 + n] | (b[i + 7 + n] << 8)):
                self._log_byte(b[i]); i += 1; continue
            self.frames.append((b[i + 2], b[i + 3], bytes(b[i + 6:i + 6 + n])))
            i += 8 + n
        del b[:i]

    def _resync(self):
        if self.rx:
            self._log_byte(self.rx[0])
            del self.rx[0]
            self._scan()

    # ---- request ----
    def request(self, cmd, payload=b'', expect=16, retries=5):
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        body = bytes([cmd, seq]) + struct.pack('<H', len(payload)) + payload
        frame = b'\xA5\x5A' + body + struct.pack('<H', crc16(body))
        timeout = 1.2 + (len(frame) + expect + 16) * 10 / self.baud
        for _ in range(retries + 1):
            self.frames.clear()
            self.t.write(frame)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                chunk = self.t.read(min(0.05, max(0.001, deadline - time.monotonic())))
                if chunk:
                    self.rx += chunk
                    self._scan()
                for c, s, p in self.frames:
                    if c == (cmd | 0x80) and s == seq:
                        return p
                self.frames.clear()
            self._resync()
        raise IOError(f'device did not answer (command 0x{cmd:02x}). Is emu8 running on that port?')

    def ok(self, cmd, payload=b'', what='', **kw):
        r = self.request(cmd, payload, **kw)
        if r[0] != 0:
            raise DeviceError(r[0], what)
        return r

    # ---- commands ----
    def hello(self, retries=5):
        r = self.ok(CMD['HELLO'], what='Hello', retries=retries)
        self.max_payload = struct.unpack_from('<H', r, 3)[0]
        self.info = dict(version=r[1], sd_mounted=bool(r[2] & 1), usb_link=bool(r[2] & 2),
                         can_set_baud=bool(r[2] & 4), max_payload=self.max_payload,
                         board=r[5:].decode('utf-8', 'replace'))
        return self.info

    def fs_info(self):
        r = self.request(CMD['FSINFO'])
        if r[0] != 0:
            return None
        total = struct.unpack_from('<Q', r, 1)[0]
        used = struct.unpack_from('<Q', r, 9)[0]
        return dict(total=total, used=used, free=total - used) if total else None

    def list(self, path):
        out = []
        pb = path.encode()
        while True:
            r = self.ok(CMD['LIST'], struct.pack('<H', len(out)) + pb, f'List {path}', expect=self.max_payload)
            o = 3
            for _ in range(r[1]):
                flags, size, n = r[o], struct.unpack_from('<I', r, o + 1)[0], r[o + 5]
                name = r[o + 6:o + 6 + n].decode('utf-8', 'replace')
                out.append(dict(name=name, dir=bool(flags & 1), size=size, path=join_path(path, name)))
                o += 6 + n
            if not r[2]:
                return out

    def stat(self, path):
        r = self.request(CMD['STAT'], path.encode())
        if r[0] == NOT_FOUND:
            return None
        if r[0] != 0:
            raise DeviceError(r[0], f'Stat {path}')
        return dict(dir=bool(r[1]), size=struct.unpack_from('<I', r, 2)[0])

    def download(self, path, progress=None):
        size = struct.unpack_from('<I', self.ok(CMD['RDOPEN'], path.encode(), f'Open {path}'), 1)[0]
        out = bytearray()
        chunk = self.max_payload - 1
        try:
            while len(out) < size:
                want = min(chunk, size - len(out))
                d = self.ok(CMD['READ'], struct.pack('<IH', len(out), want), f'Read {path}', expect=want)
                if len(d) == 1:
                    raise IOError(f'Read {path}: file ended early at {len(out)} of {size} bytes')
                out += d[1:]
                if progress:
                    progress(len(out), size)
        finally:
            try:
                self.request(CMD['RDCLOSE'])
            except IOError:
                pass
        return bytes(out)

    def upload(self, path, data, progress=None):
        self.ok(CMD['WROPEN'], path.encode(), f'Create {path}')
        chunk = self.max_payload - 4
        off = 0
        try:
            while off < len(data):
                piece = data[off:off + chunk]
                r = self.ok(CMD['WRITE'], struct.pack('<I', off) + piece, f'Write {path}')
                pos = struct.unpack_from('<I', r, 1)[0]
                if pos != off + len(piece):
                    raise IOError(f'Write {path}: device at {pos}, expected {off + len(piece)}')
                off = pos
                if progress:
                    progress(off, len(data))
        except BaseException:
            try:
                self.request(CMD['WRCLOSE'], b'\x00')
            except IOError:
                pass
            raise
        self.ok(CMD['WRCLOSE'], b'\x01', f'Save {path}')

    def mkdir(self, path):
        self.ok(CMD['MKDIR'], path.encode(), f'Create folder {path}')

    def mkdirs(self, path):
        cur = ''
        for part in [p for p in path.split('/') if p]:
            cur += '/' + part
            self.mkdir(cur)

    def remove(self, path):
        r = self.request(CMD['REMOVE'], path.encode())
        if r[0] not in (0, NOT_FOUND):
            raise DeviceError(r[0], f'Delete {path}')

    def remove_tree(self, path, on_step=None):
        st = self.stat(path)
        if not st:
            return
        if st['dir']:
            for e in self.list(path):
                self.remove_tree(e['path'], on_step)
        if on_step:
            on_step(path)
        self.remove(path)

    def rename(self, a, b):
        self.ok(CMD['RENAME'], a.encode() + b'\x00' + b.encode(), f'Rename {a}')

    def set_baud(self, baud):
        self.ok(CMD['BAUD'], struct.pack('<I', baud), 'Change speed')
        self.baud = baud

    def reboot(self):
        self.ok(CMD['REBOOT'], what='Reboot', retries=0)

    def bye(self):
        try:
            self.request(CMD['BYE'], retries=0)
        except IOError:
            pass


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------
def human(n):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return f'{n} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024


class Progress:
    def __init__(self, label, quiet):
        self.label, self.quiet, self.t0 = label, quiet or not sys.stderr.isatty(), time.monotonic()

    def __call__(self, done, total):
        if self.quiet:
            return
        rate = done / max(1e-3, time.monotonic() - self.t0)
        pct = 100 * done / total if total else 100
        sys.stderr.write(f'\r  {self.label}  {pct:5.1f}%  {human(done)}/{human(total)}  {human(rate)}/s   ')
        sys.stderr.flush()

    def end(self):
        if not self.quiet:
            sys.stderr.write('\n')


def norm_remote(p):
    p = '/' + p.replace('\\', '/').strip('/')
    return p


def cmd_info(link, a):
    i = link.info
    print(f"board:        {i['board']}")
    print(f"protocol:     v{i['version']}, frames up to {i['max_payload']} bytes")
    print(f"link:         {'native USB' if i['usb_link'] else f'UART {link.baud} baud'}")
    print(f"SD card:      {'mounted' if i['sd_mounted'] else 'NOT mounted'}")
    fs = link.fs_info()
    if fs:
        print(f"space:        {human(fs['free'])} free of {human(fs['total'])}")


def cmd_ls(link, a):
    path = norm_remote(a.path)
    st = link.stat(path) if path != '/' else {'dir': True, 'size': 0}
    if st is None:
        raise DeviceError(NOT_FOUND, path)
    entries = link.list(path) if st['dir'] else [dict(name=path.rsplit('/', 1)[-1], dir=False, size=st['size'])]
    entries.sort(key=lambda e: (not e['dir'], e['name'].lower()))
    for e in entries:
        size = '<DIR>' if e['dir'] else (str(e['size']) if a.bytes else human(e['size']))
        print(f"{size:>12}  {e['name']}{'/' if e['dir'] else ''}")


def cmd_get(link, a):
    src = norm_remote(a.remote)
    st = link.stat(src)
    if st is None:
        raise DeviceError(NOT_FOUND, src)
    name = src.rsplit('/', 1)[-1] or 'sdcard'
    dest = a.local
    if os.path.isdir(dest):
        dest = os.path.join(dest, name)

    def fetch(rpath, lpath):
        p = Progress(rpath, a.quiet)
        data = link.download(rpath, p)
        p.end()
        with open(lpath, 'wb') as f:
            f.write(data)

    def walk(rpath, lpath):
        os.makedirs(lpath, exist_ok=True)
        for e in link.list(rpath):
            if e['name'].endswith('.part'):
                continue
            target = os.path.join(lpath, e['name'])
            walk(e['path'], target) if e['dir'] else fetch(e['path'], target)

    walk(src, dest) if st['dir'] else fetch(src, dest)


def cmd_put(link, a):
    dest = norm_remote(a.remote)
    dst = link.stat(dest)
    for local in a.local:
        local = local.rstrip('/\\') or local
        base = os.path.basename(os.path.abspath(local))
        # Into an existing folder, or to an explicit new name when uploading one item.
        target = join_path(dest, base) if (dst and dst['dir']) or len(a.local) > 1 else dest
        if os.path.isdir(local):
            if not a.recursive:
                sys.exit(f'{local} is a folder: use put -r')
            link.mkdirs(target)
            for root, dirs, files in os.walk(local):
                rel = os.path.relpath(root, local).replace('\\', '/')
                rdir = target if rel == '.' else join_path(target, rel)
                for d in dirs:
                    link.mkdir(join_path(rdir, d))
                for f in files:
                    put_file(link, os.path.join(root, f), join_path(rdir, f), a.quiet)
        else:
            parent = target.rsplit('/', 1)[0] or '/'
            if parent != '/':
                link.mkdirs(parent)
            put_file(link, local, target, a.quiet)


def put_file(link, local, remote, quiet):
    with open(local, 'rb') as f:
        data = f.read()
    p = Progress(remote, quiet)
    link.upload(remote, data, p)
    if not data:
        p(0, 0)
    p.end()


def cmd_mkdir(link, a):
    for p in a.path:
        link.mkdirs(norm_remote(p))


def cmd_rm(link, a):
    for p in a.path:
        p = norm_remote(p)
        if p == '/':
            sys.exit('refusing to delete the card root')
        st = link.stat(p)
        if st is None:
            print(f'{p}: not found', file=sys.stderr)
            continue
        if st['dir'] and not a.recursive:
            try:
                link.ok(CMD['REMOVE'], p.encode(), f'Delete {p}')
            except DeviceError:
                sys.exit(f'{p} is a folder that is not empty: use rm -r')
        else:
            link.remove_tree(p, None if a.quiet else (lambda x: print(f'deleted {x}', file=sys.stderr)))


def cmd_mv(link, a):
    link.rename(norm_remote(a.src), norm_remote(a.dst))


def cmd_reboot(link, a):
    link.reboot()
    print('board is restarting')


def main():
    ap = argparse.ArgumentParser(description='Manage the SD card of an emu8 board over serial.',
                                 epilog='The board must run emu8 firmware with the SD manager (any recent build).')
    ap.add_argument('--port', '-p', default=os.environ.get('EMU8_PORT'), help='serial port, e.g. COM5 or /dev/ttyUSB0 (or env EMU8_PORT)')
    ap.add_argument('--baud', type=int, default=None, help='transfer speed for USB-UART boards (default: stay at 115200)')
    ap.add_argument('--fast', action='store_true', help=f'same as --baud {FAST_BAUD}')
    ap.add_argument('--exec', dest='exec_cmd', help=argparse.SUPPRESS)  # talk to a test program over stdio
    ap.add_argument('--log', action='store_true', help='print the board log lines seen while working')
    ap.add_argument('--quiet', '-q', action='store_true', help='no progress output')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('info', help='board and card information').set_defaults(fn=cmd_info)
    s = sub.add_parser('ls', help='list a folder'); s.add_argument('path', nargs='?', default='/')
    s.add_argument('-b', '--bytes', action='store_true', help='exact sizes'); s.set_defaults(fn=cmd_ls)
    s = sub.add_parser('get', help='download a file or folder'); s.add_argument('remote'); s.add_argument('local', nargs='?', default='.')
    s.set_defaults(fn=cmd_get)
    s = sub.add_parser('put', help='upload files or folders'); s.add_argument('local', nargs='+'); s.add_argument('remote')
    s.add_argument('-r', '--recursive', action='store_true'); s.set_defaults(fn=cmd_put)
    s = sub.add_parser('mkdir', help='create folders (with parents)'); s.add_argument('path', nargs='+'); s.set_defaults(fn=cmd_mkdir)
    s = sub.add_parser('rm', help='delete files or folders'); s.add_argument('path', nargs='+')
    s.add_argument('-r', '--recursive', action='store_true'); s.set_defaults(fn=cmd_rm)
    s = sub.add_parser('mv', help='rename or move'); s.add_argument('src'); s.add_argument('dst'); s.set_defaults(fn=cmd_mv)
    sub.add_parser('reboot', help='restart the board').set_defaults(fn=cmd_reboot)
    a = ap.parse_args()

    if a.exec_cmd:
        t = ExecTransport(a.exec_cmd)
    elif a.port:
        t = SerialTransport(a.port, DEFAULT_BAUD)
    else:
        ap.error('--port is required (or set EMU8_PORT)')
    link = Link(t, DEFAULT_BAUD, log=(lambda s: print(f'[board] {s}', file=sys.stderr)) if a.log else None)
    try:
        # Opening the port may restart the board: give it time to boot before giving up.
        link.hello(retries=15)
        want = FAST_BAUD if a.fast else a.baud
        if want and want != link.baud and link.info['can_set_baud']:
            link.set_baud(want)
            t.set_baud(want)
            try:
                link.hello(retries=3)
            except IOError:
                print(f'{want} baud does not work here; continuing at {DEFAULT_BAUD}', file=sys.stderr)
                link.baud = DEFAULT_BAUD
                t.set_baud(DEFAULT_BAUD)
                time.sleep(4.5)   # the board falls back on its own after 4 s without a frame
                link.hello(retries=10)
        if not link.info['sd_mounted'] and a.cmd != 'info':
            print('warning: board reports no SD card mounted', file=sys.stderr)
        a.fn(link, a)
        if a.cmd != 'reboot':
            link.bye()
    except (DeviceError, IOError, OSError) as e:
        sys.exit(f'error: {e}')
    except KeyboardInterrupt:
        link.bye()
        sys.exit('interrupted')
    finally:
        t.close()


if __name__ == '__main__':
    main()
