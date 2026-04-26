#!/usr/bin/env bash
# Self-contained launcher for tester.py.
#
# Creates an isolated venv at host/.venv on first run, installs
# requirements.txt (re-installs if requirements.txt is newer than the
# last successful install marker), then exec's tester.py inside the
# venv. Args are passed through, so:
#
#   ./run-test.sh                # auto-pick canX
#   ./run-test.sh can1           # explicit interface
#
# To wipe and start fresh:  rm -rf .venv

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$SCRIPT_DIR/.venv"
REQ="$SCRIPT_DIR/requirements.txt"
STAMP="$VENV/.installed"

# ── Ensure venv exists ─────────────────────────────────────────────
if [ ! -d "$VENV" ]; then
    echo "==> creating venv at $VENV"
    python3 -m venv "$VENV"
fi

PIP="$VENV/bin/pip"
PY="$VENV/bin/python3"

# ── Install / refresh deps ────────────────────────────────────────
# Re-install if the stamp doesn't exist OR requirements.txt is newer.
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

# ── Run the tester ────────────────────────────────────────────────
exec "$PY" "$SCRIPT_DIR/tester.py" "$@"
