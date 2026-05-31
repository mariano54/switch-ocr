#!/bin/sh
set -eu

cd "$HOME/Projects/SwitchOCR"

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PYTHONUNBUFFERED=1
export GEMINI_MODEL="${GEMINI_MODEL:-gemini-3.1-flash-lite}"

exec python3 server/app.py --host 0.0.0.0 --port 8742
