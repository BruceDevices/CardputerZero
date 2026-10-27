#!/bin/sh
# Launch the simulator in a self-contained venv (your distro blocks pip into
# system Python — PEP 668). Creates sim/.venv on first run, then reuses it.
#   ./sim/run.sh            simulation
#   ./sim/run.sh --real     also subprocess the plain tool rows
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VENV="$DIR/.venv"
if [ ! -x "$VENV/bin/python" ]; then
    echo "Creating venv + installing pygame (first run only)..."
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q --upgrade pip
    "$VENV/bin/pip" install -q pygame
fi
exec "$VENV/bin/python" "$DIR/bruce_sim.py" "$@"
