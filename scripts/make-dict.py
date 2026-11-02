#!/usr/bin/env python3
"""Builds the sorted lower-case word list that Pocket Write searches.

Usage: make-dict.py word-list out.txt
"""
import sys

words = set()
with open(sys.argv[1], encoding="utf-8", errors="ignore") as f:
    for line in f:
        word = line.strip().lower()
        if word.endswith("'s") or not word.isascii():
            continue
        words.add(word)
with open(sys.argv[2], "w", encoding="ascii", newline="\n") as f:
    f.write("\n".join(sorted(words, key=lambda w: w.encode())) + "\n")
print(len(words), "words")
