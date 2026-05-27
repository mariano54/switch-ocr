#!/bin/sh
set -eu

cd "$HOME/Projects/SwitchOCR"

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PYTHONUNBUFFERED=1
export SWITCH_IP="${SWITCH_IP:-192.168.0.136}"
export SYSDVR_CAPTURE_ON_REQUEST="${SYSDVR_CAPTURE_ON_REQUEST:-1}"
export SYSDVR_BACKGROUND_CAPTURE="${SYSDVR_BACKGROUND_CAPTURE:-0}"
export SYSDVR_USE_PTY="${SYSDVR_USE_PTY:-1}"

exec python3 server/app.py --host 0.0.0.0 --port 8000
