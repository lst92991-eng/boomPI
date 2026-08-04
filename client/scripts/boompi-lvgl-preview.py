#!/usr/bin/env python3
"""Serve the live 320x240 simulator framebuffer with no browser cache."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HTML = b"""<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>
<title>boomPI LVGL live preview</title><style>
body{margin:0;background:#090c12;color:#eaf0ff;font:16px system-ui;display:grid;place-items:center;min-height:100vh}
main{text-align:center}img{width:640px;max-width:94vw;image-rendering:auto;border:1px solid #34405a;box-shadow:0 18px 60px #000;border-radius:8px}
p{color:#95a3bd}</style><main><h2>boomPI · LVGL 320x240</h2><img id=f><p>Live simulator · refreshes 12 times/s</p></main>
<script>const f=document.querySelector('#f');setInterval(()=>f.src='/frame.bmp?t='+Date.now(),83)</script>"""


class Handler(BaseHTTPRequestHandler):
    root = Path(".")

    def do_GET(self):
        if self.path.startswith("/frame.bmp"):
            try:
                body = (self.root / "frame.bmp").read_bytes()
            except FileNotFoundError:
                self.send_error(503, "Simulator frame is not ready")
                return
            content_type = "image/bmp"
        else:
            body, content_type = HTML, "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--port", type=int, default=17830)
    args = parser.parse_args()
    Handler.root = args.root
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
