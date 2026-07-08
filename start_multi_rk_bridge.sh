#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${1:-"$SCRIPT_DIR/rk_bridge_config.sh"}"

if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "Config file not found: $CONFIG_FILE" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$CONFIG_FILE"

mkdir -p "$LOG_DIR"

RUN_ID="$(date +%Y%m%d_%H%M%S)"
SUMMARY_LOG="$LOG_DIR/rk_bridge_${RUN_ID}.summary.log"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$SUMMARY_LOG"
}

require_file() {
  if [[ ! -x "$1" ]]; then
    echo "Executable not found or not executable: $1" >&2
    echo "Try: chmod +x $1" >&2
    exit 1
  fi
}

require_file "$RK_BRIDGE_BIN"

log "RK multi bridge start"
log "config: $CONFIG_FILE"
log "windows_ip: $WINDOWS_IP"
log "phone_count: $PHONE_COUNT"
log "base_video_port: $BASE_VIDEO_PORT"
log "base_control_port: $BASE_CONTROL_PORT"
log "port_step: $PORT_STEP"
log "device_bind_mode: $DEVICE_BIND_MODE"
log "log_dir: $LOG_DIR"

log "Current USB device list:"
"$RK_BRIDGE_BIN" --list 2>&1 | tee -a "$SUMMARY_LOG"

if [[ "${START_AOA_ALL:-0}" == "1" ]]; then
  log "Starting all known starter devices into AOA mode"
  "$RK_BRIDGE_BIN" --start-all 2>&1 | tee -a "$SUMMARY_LOG" || true
  log "Waiting ${AOA_WAIT_SECONDS}s for USB re-enumeration"
  sleep "$AOA_WAIT_SECONDS"
  log "USB device list after AOA start:"
  "$RK_BRIDGE_BIN" --list 2>&1 | tee -a "$SUMMARY_LOG"
fi

for ((i = 0; i < PHONE_COUNT; i++)); do
  video_port=$((BASE_VIDEO_PORT + i * PORT_STEP))
  control_port=$((BASE_CONTROL_PORT + i * PORT_STEP))
  worker_log="$LOG_DIR/rk_bridge_${RUN_ID}_instance_${i}.log"
  pid_file="$LOG_DIR/rk_bridge_instance_${i}.pid"

  selector=()
  case "$DEVICE_BIND_MODE" in
    index)
      selector=(--device-index "$i")
      ;;
    usb)
      usb_value="${USB_SELECTORS[$i]:-}"
      if [[ -z "$usb_value" ]]; then
        log "ERROR: USB_SELECTORS[$i] is empty"
        exit 1
      fi
      selector=(--usb "$usb_value")
      ;;
    *)
      log "ERROR: invalid DEVICE_BIND_MODE: $DEVICE_BIND_MODE"
      exit 1
      ;;
  esac

  log "Starting instance $i"
  log "  command: $RK_BRIDGE_BIN $WINDOWS_IP $video_port $control_port ${selector[*]}"
  log "  log: $worker_log"

  nohup "$RK_BRIDGE_BIN" "$WINDOWS_IP" "$video_port" "$control_port" "${selector[@]}" \
    >"$worker_log" 2>&1 &

  pid=$!
  echo "$pid" > "$pid_file"
  log "  pid: $pid"

  sleep 0.3
done

log "All bridge instances launched"
log "Check logs:"
log "  tail -f $LOG_DIR/rk_bridge_${RUN_ID}_instance_0.log"
log "Stop one instance:"
log "  kill \$(cat $LOG_DIR/rk_bridge_instance_0.pid)"
