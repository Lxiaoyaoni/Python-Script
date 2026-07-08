#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${1:-"$SCRIPT_DIR/rk_bridge_config.sh"}"

if [[ -f "$CONFIG_FILE" ]]; then
  # shellcheck source=/dev/null
  source "$CONFIG_FILE"
else
  LOG_DIR="./logs"
fi

LOG_DIR="${LOG_DIR:-./logs}"

if [[ ! -d "$LOG_DIR" ]]; then
  echo "Log dir not found: $LOG_DIR"
  exit 0
fi

found=0

for pid_file in "$LOG_DIR"/rk_bridge_instance_*.pid; do
  [[ -f "$pid_file" ]] || continue
  found=1
  pid="$(cat "$pid_file" 2>/dev/null || true)"

  if [[ -z "$pid" ]]; then
    rm -f "$pid_file"
    continue
  fi

  if kill -0 "$pid" 2>/dev/null; then
    echo "Stopping pid $pid from $pid_file"
    kill "$pid" 2>/dev/null || true
  else
    echo "Pid $pid is not running"
  fi

  rm -f "$pid_file"
done

if [[ "$found" == "0" ]]; then
  echo "No RK bridge pid files found in $LOG_DIR"
fi
