#!/bin/sh
set -eu

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

CLOUDFLARED="${CLOUDFLARED:-/opt/homebrew/bin/cloudflared}"
TUNNEL_NAME="${SWITCHOCR_CLOUDFLARE_TUNNEL:-switchocr}"

exec "$CLOUDFLARED" tunnel --no-autoupdate run "$TUNNEL_NAME"
