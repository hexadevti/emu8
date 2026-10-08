#!/usr/bin/env python3
"""Put a build on the PicoCalc's SD card over emu8's USB serial port -- no USB drive, no eject.

The mass-storage route (deploy-picocalc.ps1) mounts and "safely removes" the loader's drive and
rescans USB, which leaves Windows and USB hubs wedged often enough to need replugging. This only
ever talks to the serial port, the same one emu8 always has:

  1. Reach the SD file server (sdserial, tools/sdmanager/PROTOCOL.md). If emu8 runs an emulator,
     "@sdmgr" makes it restart in SD Manager mode (src/picocalc/loader_picocalc.cpp). Builds older
     than that command ignore it: then pick SD MGR on the PicoCalc's system menu (Ctrl-Shift-F1).
  2. Upload to <apps>/<name>.new, then swap it in, so a broken transfer never leaves a bad .uf2.
  3. Send "@uf2menu": the UF2 Loader menu opens; press Enter on the file to flash it. (The loader
     has no autoload: its UI only flashes on a key press, github.com/pelrun/uf2loader ui/main.c.)

Usage: deploy-serial.py [--uf2 FILE] [--port COM5] [--name emu8.uf2] [--apps pico1-apps]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sdmanager'))
import emu8sd  # noqa: E402

import serial  # noqa: E402
from serial.tools import list_ports  # noqa: E402

PICO_VID = 0x2E8A
STALE = ('emu8-dev.uf2', 'emu8.ino.uf2')


def find_port(wanted):
    if wanted:
        return wanted if any(p.device == wanted for p in list_ports.comports()) else None
    for p in list_ports.comports():
        if p.vid == PICO_VID:
            return p.device
    return None


def say(msg):
    print(msg, flush=True)


def try_link(port, retries):
    """Open the port and say hello. Returns (transport, link) or None."""
    try:
        t = emu8sd.SerialTransport(port, emu8sd.DEFAULT_BAUD)
    except Exception as e:
        return None, str(e)
    # The Pico's USB serial (TinyUSB CDC) only sends while DTR is up; emu8sd keeps it down for the
    # ESP32, where DTR is wired to reset.
    t.s.dtr = True
    t.s.rts = True
    link = emu8sd.Link(t, emu8sd.DEFAULT_BAUD)
    try:
        link.hello(retries=retries)
        return (t, link), None
    except Exception as e:
        t.close()
        return None, str(e)


def send_raw(port, data):
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
        s.write(data)
        s.flush()
        time.sleep(0.2)
        s.close()
        return True
    except Exception:
        return False


def connect(want_port, wait_sec):
    t0 = time.time()
    last_sdmgr = 0
    told = set()
    while time.time() - t0 < wait_sec:
        port = find_port(want_port)
        if not port:
            if 'noport' not in told:
                say('Esperando a porta serial do emu8... (se o PicoCalc estiver no menu do loader, escolha o emu8 lá)')
                told.add('noport')
            time.sleep(1)
            continue
        got, err = try_link(port, retries=2)
        if got:
            return got
        if err and ('PermissionError' in err or 'Access is denied' in err or 'could not open' in err):
            if 'busy' not in told:
                say(f'{port} está ocupada (monitor serial aberto?). Feche e eu continuo.')
                told.add('busy')
            time.sleep(2)
            continue
        if time.time() - last_sdmgr > 8:
            say(f'Pedindo ao emu8 em {port} para reiniciar no SD Manager...')
            send_raw(port, b'\n@sdmgr\n')
            last_sdmgr = time.time()
            if time.time() - t0 > 20 and 'old' not in told:
                say('Sem resposta. Se o firmware for antigo: no PicoCalc, Ctrl-Shift-F1 e escolha SD MGR.')
                told.add('old')
        time.sleep(1.5)
    raise SystemExit(f'Não consegui falar com o emu8 em {wait_sec}s.')


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--uf2', default=os.path.join(here, '..', 'build-picocalc-rp2040', 'emu8.ino.uf2'))
    ap.add_argument('--port', default=os.environ.get('EMU8_PORT', ''))
    ap.add_argument('--name', default='emu8.uf2')
    ap.add_argument('--apps', default='pico1-apps')
    ap.add_argument('--wait', type=int, default=300)
    ap.add_argument('--no-menu', action='store_true', help='leave the PicoCalc in SD Manager mode')
    a = ap.parse_args()

    data = open(a.uf2, 'rb').read()
    say(f'{os.path.basename(a.uf2)}: {len(data)} bytes')
    (t, link) = connect(a.port or None, a.wait)
    try:
        if not link.info['sd_mounted']:
            raise SystemExit('O emu8 respondeu, mas diz que não tem cartão SD montado.')
        apps = '/' + a.apps.strip('/')
        if not link.stat(apps):
            link.mkdirs(apps)
        final, tmp = f'{apps}/{a.name}', f'{apps}/{a.name}.new'
        link.remove(tmp)
        t_start = time.time()
        prog = emu8sd.Progress(f'Enviando {final}', False)
        link.upload(tmp, data, prog)
        prog.end()
        st = link.stat(tmp)
        if not st or st['size'] != len(data):
            raise SystemExit(f'Tamanho errado no cartão: {st}')
        link.remove(final)
        link.rename(tmp, final)
        for n in STALE:
            if n != a.name:
                link.remove(f'{apps}/{n}')
        say(f'OK: {final} ({len(data)} bytes em {time.time() - t_start:.1f}s)')
        link.bye()
    finally:
        t.close()

    if a.no_menu:
        return
    time.sleep(0.3)
    port = find_port(a.port or None)
    if port and send_raw(port, b'\n@uf2menu\n'):
        say(f'Abrindo o menu do UF2 Loader. No PicoCalc: Enter em {a.name}.')
    else:
        say(f'Não consegui abrir o menu; no PicoCalc: Ctrl-Shift-Up e Enter em {a.name}.')


if __name__ == '__main__':
    main()
