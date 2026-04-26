#!/usr/bin/env bash
# Self-contained launcher for ota.py — same shape as run-test.sh.
# Creates host/.venv on first run, installs requirements, builds the
# fixture envelope via sumo-tool, pushes via UDS, prints OK/FAIL.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$SCRIPT_DIR/.venv"
REQ="$SCRIPT_DIR/requirements.txt"
STAMP="$VENV/.installed"

if [ ! -d "$VENV" ]; then
    echo "==> creating venv at $VENV"
    python3 -m venv "$VENV"
fi

PIP="$VENV/bin/pip"
PY="$VENV/bin/python3"

need_install=0
if [ ! -f "$STAMP" ]; then
    need_install=1
elif [ "$REQ" -nt "$STAMP" ]; then
    need_install=1
fi
if [ "$need_install" = "1" ]; then
    echo "==> pip install -r $(basename "$REQ")"
    "$PIP" install -q --upgrade pip
    "$PIP" install -q -r "$REQ"
    touch "$STAMP"
fi

exec "$PY" "$SCRIPT_DIR/ota.py" "$@"
