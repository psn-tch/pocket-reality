#!/usr/bin/env python3
"""Converts a Pocket Word 2 file with no password into plain ASCII-friendly text.

These files are RTF with a {\\pwd2 header. Password-protected files are encrypted
and are refused. Formatting is dropped, because Pocket Write stores plain text.

Usage: pwd2txt.py story.pwd story.txt
"""
import sys

# Characters Pocket Write's font lacks, mapped to what a keyboard would type.
PLAIN = {
    "‘": "'", "’": "'", "“": '"', "”": '"',
    "–": "-", "—": "--", "…": "...", " ": " ",
}

SKIP_GROUPS = {"fonttbl", "colortbl", "stylesheet", "info", "pwdcomment"}


def convert(data):
    out = []
    i = 0
    depth = 0
    skip_at = None
    n = len(data)

    while i < n:
        c = data[i]
        if c == "{":
            depth += 1
            i += 1
            continue
        if c == "}":
            if skip_at == depth:
                skip_at = None
            depth -= 1
            i += 1
            continue
        if c in "\r\n":
            i += 1
            continue
        if c != "\\":
            if skip_at is None:
                out.append(c)
            i += 1
            continue

        i += 1
        c = data[i]
        if c in "\\{}":
            if skip_at is None:
                out.append(c)
            i += 1
        elif c == "'":
            if skip_at is None:
                out.append(bytes([int(data[i + 1:i + 3], 16)]).decode("cp1252", "replace"))
            i += 3
        elif c == "~":
            if skip_at is None:
                out.append(" ")
            i += 1
        elif c == "*":
            i += 1
        elif c.isalpha():
            start = i
            while i < n and data[i].isalpha():
                i += 1
            word = data[start:i]
            if i < n and (data[i].isdigit() or data[i] == "-"):
                i += 1
                while i < n and data[i].isdigit():
                    i += 1
            if i < n and data[i] == " ":
                i += 1
            if word in SKIP_GROUPS and skip_at is None:
                skip_at = depth
            elif skip_at is None and word in ("par", "line"):
                out.append("\n")
            elif skip_at is None and word == "tab":
                out.append("    ")
        else:
            i += 1
    return "".join(out)


def main():
    with open(sys.argv[1], "rb") as f:
        raw = f.read()
    if not raw.startswith(b"{\\pwd2\\ansi"):
        sys.exit(f"{sys.argv[1]}: still has a password, or is not Pocket Word 2")
    text = convert(raw.decode("latin-1"))
    for wide, plain in PLAIN.items():
        text = text.replace(wide, plain)
    text = text.encode("ascii", "replace").decode("ascii").rstrip("\n") + "\n"
    with open(sys.argv[2], "w", encoding="ascii", newline="\n") as f:
        f.write(text)


main()
