#!/usr/bin/env python3
"""Converts your own art into the simple .img format the desktop loads.

The output is private too. It goes in build/, which git ignores, and on the card.

Usage: make-art.py private-folder out-folder
"""
import os
import struct
import subprocess
import sys


def rgba(path):
    size = subprocess.run(["magick", "identify", "-format", "%w %h", path + "[0]"],
                          capture_output=True, check=True, text=True).stdout
    w, h = map(int, size.split())
    raw = subprocess.run(["magick", path + "[0]", "-depth", "8", "rgba:-"],
                         capture_output=True, check=True).stdout
    return w, h, raw


def write_img(path, w, h, raw):
    pixels = bytearray()
    mask = bytearray()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        pixels += struct.pack("<H", (r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3)
        mask.append(1 if a >= 128 else 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<HH", w, h) + pixels + mask)


def main():
    private, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    art = {
        "wallpaper": "wallpaper/wallpaper.bmp",
        "happy-mac": "macos9/happy-mac.png",
        "folder": "macos9/icons/folder.png",
        "folder-16": "macos9/icons/folder-16.png",
        "hard-disk": "macos9/icons/hard-disk.png",
        "trash": "macos9/icons/trash.png",
        "document-16": "macos9/icons/document-16.png",
        "application-16": "macos9/icons/application-16.png",
    }
    for name, source in art.items():
        w, h, raw = rgba(os.path.join(private, source))
        write_img(os.path.join(out, name + ".img"), w, h, raw)
        print(f"{name}: {w}x{h}")


main()
