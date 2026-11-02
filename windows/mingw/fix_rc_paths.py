#!/usr/bin/env python3
"""Rewrite windows/pvengine.rc resource paths to case-correct POSIX form for windres.

Only quoted strings that look like file paths are rewritten. Menu accelerators
and other escapes (\\t, \\n, ...) are left alone.
"""
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

_PATH_EXTS = (
    ".bmp", ".ico", ".cur", ".png", ".jpg", ".jpeg", ".gif", ".avi", ".wav",
    ".rc", ".h", ".hpp", ".inl", ".txt", ".bin", ".manifest",
)


def looks_like_path(s: str) -> bool:
    if not s:
        return False
    low = s.lower().replace("\\\\", "/").replace("\\", "/")
    # Resource paths first (note: "\toolbar\..." contains the two chars "\"+"t").
    if "/" in low or any(low.endswith(ext) for ext in _PATH_EXTS):
        return True
    # Menu/LTEXT captions with accelerator or newline escapes - not paths.
    if re.search(r"\\[tnr]", s):
        return False
    return False


def fix_quoted(m):
    raw = m.group(1)
    if not looks_like_path(raw):
        return m.group(0)
    s = raw.replace("\\\\", "/").replace("\\", "/")
    return '"' + index.get(s.lower(), s) + '"'


dst.write_text(re.sub(r'"([^"\n]*)"', fix_quoted, text))
