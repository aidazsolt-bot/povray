#!/usr/bin/env python3
"""Rewrite windows/pvengine.rc quoted paths to case-correct POSIX form for windres."""
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    sys.stderr.write("usage: fix_rc_paths.py <windows_dir> <out_rc>\n")
    sys.exit(2)

win = Path(sys.argv[1])
dst = Path(sys.argv[2])
text = (win / "pvengine.rc").read_text(errors="replace")
index = {}
for p in win.rglob("*"):
    if p.is_file():
        rel = p.relative_to(win).as_posix()
        index[rel.lower()] = rel


def fix_quoted(m):
    s = m.group(1).replace("\\\\", "/").replace("\\", "/")
    return '"' + index.get(s.lower(), s) + '"'


dst.write_text(re.sub(r'"([^"\n]*)"', fix_quoted, text))
