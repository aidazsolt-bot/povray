#!/usr/bin/env python3
"""Deprecated wrapper — use transforms_to_pov_camera.py instead."""
from __future__ import annotations

import sys
from pathlib import Path

# Re-export new tool as drop-in for old argv: transforms.json ply > scene.pov
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from transforms_to_pov_camera import main  # noqa: E402


if __name__ == "__main__":
    if len(sys.argv) >= 3 and not sys.argv[1].startswith("-"):
        # Old: gen_splat_scene.py transforms.json models/Rose.ply
        transforms, ply = sys.argv[1], sys.argv[2]
        raise SystemExit(main([transforms, "--scene", "--ply", ply, "-o", "/dev/stdout"]))
    raise SystemExit(main())
