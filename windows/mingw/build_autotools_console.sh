#!/bin/bash
# Compatibility wrapper: Autotools MinGW console (no GUI).
exec "$(cd "$(dirname "$0")" && pwd)/build_console.sh" "$@"
