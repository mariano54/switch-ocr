#!/bin/sh
set -eu

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

CLOUDFLARED="${CLOUDFLARED:-/opt/homebrew/bin/cloudflared}"
TARGET_URL="${SWITCHOCR_TUNNEL_TARGET:-http://127.0.0.1:8742}"

exec "$CLOUDFLARED" tunnel --no-autoupdate --url "$TARGET_URL"
