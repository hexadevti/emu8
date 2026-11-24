#!/usr/bin/env python3
"""Local test server for the browser build.

    python web/serve.py [port]          -> http://localhost:8765/

Serves web/dist with the COOP/COEP headers SharedArrayBuffer needs, plus (local only) the repo's
sdcard/ folder: /__sdcard_list returns its file list and /__sdcard/<path> the files. On localhost the
page copies them into its /sdcard (IndexedDB) at startup, so the system ROMs and games are there
without uploading. Nothing of this exists on the published site.
"""
import functools
import http.server
import json
import os
import sys
import urllib.parse

WEB = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(WEB, "dist")
SD = os.path.join(os.path.dirname(WEB), "sdcard")
MAX = 64 * 1024 * 1024
PER_DIR = 40


class H(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        if p.startswith("/__sdcard/"):
            full = os.path.normpath(os.path.join(SD, p[len("/__sdcard/"):]))
            return full if full.startswith(SD) else SD
        return super().translate_path(path)

    def do_GET(self):
        if self.path.startswith("/__sdcard_list"):
            files = []
            for root, _, names in os.walk(SD):
                # IDBFS keeps every file in the tab's memory: a sample of each folder is enough
                # (the repo's nes/ alone has thousands of ROMs)
                for n in sorted(names)[:PER_DIR]:
                    full = os.path.join(root, n)
                    size = os.path.getsize(full)
                    if size <= MAX:
                        files.append({"path": os.path.relpath(full, SD).replace(os.sep, "/"), "size": size})
            body = json.dumps(files).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"emu8 local: http://localhost:{port}/  (sdcard: {SD})", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=DIST)).serve_forever()
