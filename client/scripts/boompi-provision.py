#!/usr/bin/env python3
"""Static setup QR and one-shot Wi-Fi form; credentials are never logged."""

import fcntl
import os
import struct
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

# WIFI:T:WPA;S:boomPI-Setup;P:boompi-setup;;
# Generated once with python-qrcode 7.4.2 (MIT); the runtime needs no QR library.
QR = (
    "11111110011010001001101111111", "10000010001110111000101000001",
    "10111010111100100010101011101", "10111010101011111100001011101",
    "10111010100010101001101011101", "10000010100010111000101000001",
    "11111110101010101010101111111", "00000000110100011001000000000",
    "10111110011001100100101111100", "00111101010010100000101010110",
    "11110010101000111010111101000", "01000101010000110010111010011",
    "10101010101011100111000111100", "10110100100000101101011110110",
    "11110010101110010110110100100", "01100000110000010000011001000",
    "11101111010101011001000101011", "11011101000001101000111011010",
    "10110111011111011010001110000", "10101000100110111010001010001",
    "10011010010101000110111111100", "00000000110001101101100010100",
    "11111110000101011110101010100", "10000010110010101011100011011",
    "10111010101100000100111111011", "10111010111111001011000011011",
    "10111010101010011111111111110", "10000010001111101010101011010",
    "11111110111001100101001011000",
)
FORM = b"""<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>
<title>boomPI Wi-Fi</title><style>body{font:18px sans-serif;max-width:28em;margin:3em auto;padding:1em}input,button{font-size:1em;width:100%;padding:.7em;margin:.4em 0;box-sizing:border-box}</style>
<h2>boomPI Wi-Fi setup</h2><p>Enter the 2.4 GHz Wi-Fi name and password.</p>
<form method=post><input name=ssid maxlength=32 placeholder='Wi-Fi name' required>
<input name=password type=password minlength=8 maxlength=63 placeholder='Password' required>
<button>Save and connect</button></form>"""


def write_gpio(number, value=None):
    path = "/sys/class/gpio/gpio{}".format(number)
    if not os.path.isdir(path):
        with open("/sys/class/gpio/export", "w", encoding="ascii") as output:
            output.write(str(number))
        for _ in range(50):
            if os.path.isdir(path):
                break
            time.sleep(0.01)
    if value is None:
        with open(path + "/direction", "w", encoding="ascii") as output:
            output.write("out")
    else:
        with open(path + "/value", "w", encoding="ascii") as output:
            output.write("1" if value else "0")


def draw_panel(frame):
    """Own the panel while the voice client is stopped and draw one QR frame."""
    for number in (53, 66, 67):
        write_gpio(number)
        write_gpio(number, False)
    panel = os.open("/dev/spidev0.0", os.O_RDWR)
    try:
        fcntl.ioctl(panel, 0x40016B01, struct.pack("B", 0))
        fcntl.ioctl(panel, 0x40016B03, struct.pack("B", 8))
        fcntl.ioctl(panel, 0x40046B04, struct.pack("I", 8_000_000))

        def command(value, *data):
            write_gpio(66, False)
            os.write(panel, bytes((value,)))
            if data:
                write_gpio(66, True)
                os.write(panel, bytes(data))

        write_gpio(67, False)
        time.sleep(0.1)
        write_gpio(67, True)
        time.sleep(0.1)
        commands = (
            (0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33), (0x36, 0x00), (0x3A, 0x55),
            (0xB7, 0x55), (0xBB, 0x1A), (0xC0, 0x2C), (0xC2, 0x01),
            (0xC3, 0x19), (0xC6, 0x0F), (0xD0, 0xA7), (0xD0, 0xA4, 0xA1),
            (0xD6, 0xA1),
            (0xE0, 0xF0, 0x03, 0x09, 0x0B, 0x0A, 0x16, 0x2B, 0x33, 0x41, 0x38, 0x14, 0x14, 0x29, 0x2F),
            (0xE1, 0xF0, 0x04, 0x06, 0x09, 0x08, 0x04, 0x2B, 0x32, 0x41, 0x36, 0x12, 0x12, 0x2A, 0x30),
            (0x21,), (0x2A, 0x00, 0x00, 0x00, 0xEF),
            (0x2B, 0x00, 0x00, 0x01, 0x3F), (0x11,),
        )
        for item in commands:
            command(*item)
        time.sleep(0.12)
        command(0x29)
        command(0x2C)
        write_gpio(66, True)
        for offset in range(0, len(frame), 4096):
            os.write(panel, frame[offset:offset + 4096])
        write_gpio(53, True)
    finally:
        os.close(panel)


def show_qr():
    width, height, scale, border = 240, 320, 6, 4
    frame = bytearray(b"\xff\xff" * width * height)
    origin_x, origin_y = 9, 24
    for row, bits in enumerate(QR):
        for column, dark in enumerate(bits):
            if dark == "1":
                x0 = origin_x + (column + border) * scale
                y0 = origin_y + (row + border) * scale
                for y in range(y0, y0 + scale):
                    start = 2 * (y * width + x0)
                    frame[start:start + 2 * scale] = b"\0\0" * scale
    try:
        draw_panel(frame)
    except OSError:
        # The HTTP fallback remains usable on headless development images.
        pass


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body, content_type="text/html; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.reply(200, FORM)

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            values = parse_qs(self.rfile.read(min(length, 4096)).decode("utf-8"))
            ssid = values.get("ssid", [""])[0]
            password = values.get("password", [""])[0]
            if not (1 <= len(ssid.encode()) <= 32 and 8 <= len(password.encode()) <= 63):
                return self.reply(400, b"Invalid Wi-Fi name or password", "text/plain")
            result = subprocess.run(["/userdata/boompi/bin/boompi-client", "--save-wifi"],
                                    input=ssid + "\n" + password + "\n", text=True,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if result.returncode:
                return self.reply(500, b"Could not save Wi-Fi", "text/plain")
            self.reply(200, b"Wi-Fi saved. boomPI is reconnecting.", "text/plain")
            threading.Thread(target=self.server.shutdown, daemon=True).start()
        except (UnicodeError, ValueError):
            self.reply(400, b"Invalid request", "text/plain")

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    if os.environ.get("BOOMPI_PROVISION_NO_PANEL") != "1":
        show_qr()
    HTTPServer(("0.0.0.0", 80), Handler).serve_forever()
