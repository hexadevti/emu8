#!/usr/bin/env python3
"""Build the emu8 browser version (WebAssembly) into web/dist/.

    python web/build.py            # incremental build (objects cached in build-web/)
    python web/build.py --clean    # from scratch

Needs the Emscripten SDK (EMSDK env var, or ~/emsdk). Compiles the SAME core/shared sources the
desktop build does (BOARD_DESKTOP), with the browser backends from web/src/ (EMU_WEB) in place of
the SDL/ImGui ones, then copies the page (web/shell/) next to the .js/.wasm.
"""
import concurrent.futures as cf
import glob
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH = os.path.join(REPO, "emu8")
WEB = os.path.join(REPO, "web")
OBJ = os.path.join(REPO, "build-web")
DIST = os.path.join(WEB, "dist")

EMSDK = os.environ.get("EMSDK") or os.path.join(os.path.expanduser("~"), "emsdk")
EXE = ".exe" if os.name == "nt" else ""
EMXX = os.path.join(EMSDK, "upstream", "emscripten", "em++" + EXE)

EXCLUDE = {os.path.normpath(os.path.join(SKETCH, "src", "shared", f)) for f in
           ("audio_amp.cpp", "speaker.cpp", "display_gfx.cpp", "sd.cpp", "usbgamepad.cpp", "keyboardPs2.cpp")}


def sources():
    src = []
    for d in ("apple2", "c64", "nes", "atari", "msx", "sms", "z80", "coleco", "zx", "shared"):
        src += glob.glob(os.path.join(SKETCH, "src", d, "*.cpp"))
    src = [s for s in src if os.path.normpath(s) not in EXCLUDE]
    src += glob.glob(os.path.join(SKETCH, "src", "pcxt", "*.cpp"))
    src += glob.glob(os.path.join(SKETCH, "src", "pcxt", "fabgl", "*.cpp"))
    src += [os.path.join(SKETCH, "globals.cpp")]
    src += [os.path.join(SKETCH, "src", "desktop", f) for f in ("hal.cpp", "sd_host.cpp", "debug_bridge.cpp")]
    src += glob.glob(os.path.join(WEB, "src", "*.cpp"))
    ino = os.path.join(OBJ, "emu8_ino.cpp")
    os.makedirs(OBJ, exist_ok=True)
    with open(os.path.join(SKETCH, "emu8.ino"), "rb") as f:
        data = f.read()
    if not os.path.exists(ino) or open(ino, "rb").read() != data:
        with open(ino, "wb") as f:
            f.write(data)
    src.append(ino)
    return src


CFLAGS = [
    "-std=c++17", "-O2", "-pthread", "-funsigned-char",
    "-Wno-write-strings", "-Wno-narrowing", "-Wno-c++11-narrowing", "-w",
    "-DBOARD_DESKTOP", "-DEMU_WEB", '-DEMU_DEFAULT_SD_DIR="/sdcard"', "-DPANEL_NATIVE_W=320", "-DPANEL_NATIVE_H=240",
    "-I" + os.path.join(SKETCH, "src", "desktop", "arduino_shim"),
    "-I" + SKETCH,
]

LDFLAGS = [
    "-O2", "-pthread",
    "-sPROXY_TO_PTHREAD=1",
    "-sPTHREAD_POOL_SIZE=10",
    "-sDEFAULT_PTHREAD_STACK_SIZE=1048576",
    "-sSTACK_SIZE=2097152",
    "-sINITIAL_MEMORY=268435456",
    "-sALLOW_MEMORY_GROWTH=0",
    "-sMODULARIZE=1", "-sEXPORT_NAME=createEmu8",
    "-sENVIRONMENT=web,worker",
    "-sFORCE_FILESYSTEM=1", "-lidbfs.js",
    "-sEXIT_RUNTIME=0",
    "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,HEAPU8,HEAP32,addRunDependency,removeRunDependency",
    "-sEXPORTED_FUNCTIONS=_main,_malloc,_free",
]


def obj_for(src):
    rel = os.path.relpath(src, REPO).replace(os.sep, "_").replace(":", "")
    return os.path.join(OBJ, rel + ".o")


def compile_one(src):
    obj = obj_for(src)
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and not os.environ.get("FORCE"):
        return obj, None
    cmd = [EMXX, "-x", "c++", "-c", src, "-o", obj] + CFLAGS
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return obj, f"--- {src}\n{r.stdout}{r.stderr}"
    return obj, None


def main():
    if "--clean" in sys.argv and os.path.isdir(OBJ):
        shutil.rmtree(OBJ)
    if not os.path.exists(EMXX):
        sys.exit(f"em++ not found at {EMXX} (set EMSDK)")
    srcs = sources()
    # headers changed? (cheap heuristic: any .h newer than an object forces that object)
    newest_h = max(os.path.getmtime(h) for h in glob.glob(os.path.join(SKETCH, "**", "*.h"), recursive=True))
    for s in srcs:
        o = obj_for(s)
        if os.path.exists(o) and os.path.getmtime(o) < newest_h:
            os.remove(o)
    errors, objs = [], []
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for obj, err in ex.map(compile_one, srcs):
            objs.append(obj)
            if err:
                errors.append(err)
    if errors:
        print("\n".join(errors[:8]))
        sys.exit(f"{len(errors)} file(s) failed to compile")
    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, "emu8.js")
    r = subprocess.run([EMXX] + objs + ["-o", out] + LDFLAGS, capture_output=True, text=True)
    sys.stdout.write(r.stdout + r.stderr)
    if r.returncode != 0:
        sys.exit("link failed")
    for f in glob.glob(os.path.join(WEB, "shell", "*")):
        shutil.copy2(f, DIST) if os.path.isfile(f) else None
    print("OK ->", DIST)


if __name__ == "__main__":
    main()
