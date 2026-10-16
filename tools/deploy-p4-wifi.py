#!/usr/bin/env python3
"""Put an emu8 build on the PicoCalc P4's SD card over Wi-Fi, through esp32loader's web server.

  1. Send "@uf2menu" on the console UART (COM9): emu8 reboots into esp32loader
     (src/picocalc/loader_picocalc.cpp). Builds older than that command ignore it; press
     Ctrl-Shift-Up on the PicoCalc instead -- the script waits for the loader either way.
  2. Send F4 (ESC O S) on the console: the loader joins the saved network and logs "Got IP".
  3. POST the .bin to /api/upload?path=/<apps>/<name>, creating the folder first.

The loader stays on its web screen; pick the file there (or Esc, then the file list) to flash it.
"""
import argparse, re, sys, time, urllib.parse, urllib.request
sys.stdout.reconfigure(errors='replace')   # the console log has bytes the Windows codepage lacks
import serial

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--bin', default='.picocalc-p4-build/emu8.ino.bin')
ap.add_argument('--port', default='COM9')
ap.add_argument('--apps', default='esp32-p4-apps')
ap.add_argument('--name', default='emu8.bin')
ap.add_argument('--wait', type=float, default=90, help='seconds to wait for the loader / the IP')
args = ap.parse_args()

data = open(args.bin, 'rb').read()
print(f'{args.bin}: {len(data)} bytes')

p = serial.Serial()
p.port, p.baudrate, p.timeout = args.port, 115200, 0.2
# CH343 auto-reset pulls EN low when RTS and DTR differ; opening the port with both low still reset the
# board (twice, losing the loader), so open with both asserted and never change them.
p.dtr = p.rts = True
p.open()

log = ''
def pump():
    global log
    b = p.read(4096)
    if b:
        t = b.decode('latin1')
        log += t
        if 'FB ' not in t:            # skip the pixel dump
            sys.stdout.write(t); sys.stdout.flush()
    return b

def wait_for(pattern, seconds, poke=None, every=3.0):
    t0 = last = time.time()
    while time.time() - t0 < seconds:
        pump()
        m = re.search(pattern, log)
        if m: return m
        if poke and 'FB ' in log: poke = None    # it answered; let the dump finish
        if poke and time.time() - last > every:
            p.write(poke); last = time.time()
    return None

# 1. into the loader. Ctrl+P makes the loader dump its framebuffer ("FB ..." lines ending in "FB end"),
#    which proves it is running; emu8 ignores the byte. Only every 15 s: a dump takes seconds at
#    115200 baud, and back-to-back dumps can reset the loader.
p.write(b'\n@uf2menu\n')
print('\n-- sent @uf2menu; if this emu8 is older, press Ctrl-Shift-Up on the PicoCalc')
if not wait_for(r'FB end|loader: Ready', args.wait, poke=b'\x10', every=15.0):
    sys.exit('esp32loader did not show up on ' + args.port)

# 2. start the web server (F4 = ESC O S on the loader's console)
log = ''
p.write(b'\x1bOS')
m = wait_for(r'Got IP (\d+\.\d+\.\d+\.\d+)', args.wait)
if not m:
    if 'Access point' in log:
        sys.exit('\nthe loader started its own access point (no saved network, or it could not join); '
                 'set the network with F1 in the loader')
    sys.exit('\nno IP from the loader')
ip = m.group(1)
print(f'\n-- loader at http://{ip}/')
p.close()

# 3. upload
def post(api, query, body=b''):
    url = f'http://{ip}/api/{api}?' + urllib.parse.urlencode(query)
    req = urllib.request.Request(url, data=body, method='POST',
                                 headers={'Content-Type': 'application/octet-stream'})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.status, r.read().decode('utf-8', 'replace')

for attempt in range(10):      # the HTTP server can lag the IP by a moment
    try:
        post('mkdir', {'path': '/' + args.apps})
        break
    except urllib.error.HTTPError:
        break                  # already there
    except OSError:
        time.sleep(1)
dest = f'/{args.apps}/{args.name}'
t0 = time.time()
st, body = post('upload', {'path': dest, 'mtime': int(time.time())}, data)
print(f'-- upload {dest}: HTTP {st} {body.strip()} ({time.time() - t0:.1f} s)')
