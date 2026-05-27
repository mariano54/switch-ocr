#!/bin/sh
set -eu

cd "$HOME/Projects/SwitchOCR"

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PYTHONUNBUFFERED=1
export SWITCH_IP="${SWITCH_IP:-192.168.0.136}"
export SYSDVR_FRAME_SOURCE="${SYSDVR_FRAME_SOURCE:-rtsp}"
export SYSDVR_FRAME_OUTPUT="${SYSDVR_FRAME_OUTPUT:-server/latest_frame.jpg}"

case "$SYSDVR_FRAME_SOURCE" in
  rtsp)
    exec python3 tools/frame_source.py --output "$SYSDVR_FRAME_OUTPUT" rtsp \
      --url "${SYSDVR_RTSP_URL:-rtsp://$SWITCH_IP:6666/}" \
      --interval "${SYSDVR_FRAME_INTERVAL:-0.5}" \
      --restart-delay "${SYSDVR_RTSP_RESTART_DELAY:-2.0}"
    ;;
  bridge)
    export SYSDVR_CAPTURE_RETRIES="${SYSDVR_CAPTURE_RETRIES:-1}"
    export SYSDVR_CONNECT_TIMEOUT_SECONDS="${SYSDVR_CONNECT_TIMEOUT_SECONDS:-3.0}"
    export SYSDVR_RECORD_SECONDS="${SYSDVR_RECORD_SECONDS:-0.85}"
    export SYSDVR_USE_PTY="${SYSDVR_USE_PTY:-1}"
    exec python3 tools/frame_source.py --output "$SYSDVR_FRAME_OUTPUT" bridge \
      --interval "${SYSDVR_CAPTURE_INTERVAL:-2.0}"
    ;;
  *)
    echo "Unknown SYSDVR_FRAME_SOURCE=$SYSDVR_FRAME_SOURCE; expected rtsp or bridge" >&2
    exit 2
    ;;
esac
