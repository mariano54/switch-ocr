#!/bin/sh
set -eu

cd "$HOME/Projects/SwitchOCR"

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PYTHONUNBUFFERED=1
export GEMINI_MODEL="${GEMINI_MODEL:-gemini-3.1-flash-lite}"
PORT="${SWITCHOCR_PORT:-8742}"

# Free the port from any leftover instance so we don't crash-loop on bind.
stale_pids="$(lsof -ti "tcp:${PORT}" 2>/dev/null || true)"
if [ -n "$stale_pids" ]; then
    echo "run_server: killing stale listeners on ${PORT}: $stale_pids"
    # shellcheck disable=SC2086
    kill $stale_pids 2>/dev/null || true
    sleep 1
fi

exec python3 server/app.py --host 0.0.0.0 --port "$PORT"
