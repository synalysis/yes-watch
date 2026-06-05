#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_JSON="${ROOT_DIR}/package.json"
ALL_MODELS=()

load_target_platforms() {
  if [ ! -f "$PACKAGE_JSON" ]; then
    echo "Could not find package.json at ${PACKAGE_JSON}" >&2
    exit 1
  fi

  mapfile -t ALL_MODELS < <(python3 -c "
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    for platform in json.load(f)['pebble']['targetPlatforms']:
        print(platform)
" "$PACKAGE_JSON")

  if [ "${#ALL_MODELS[@]}" -eq 0 ]; then
    echo "No target platforms found in ${PACKAGE_JSON}" >&2
    exit 1
  fi
}

usage() {
  cat <<'USAGE'
Usage: scripts/screenshot.sh [model] [output] [--gif]

Starts a Pebble emulator, installs the watchface, waits for app startup, and
writes a screenshot or GIF under screenshots/<model>.

Arguments:
  model   Pebble model/platform to run (default: basalt)
          Use "all" to capture every supported model in one run.
  output  Output path (default: screenshots/<model>/app-start.png or .gif)
  --gif   Record a GIF instead of a PNG, starting after app startup delay

Environment:
  STARTUP_DELAY_SECONDS  Seconds to wait after install before capture (default: 5)
  GIF_CYCLE_SECONDS      Corner alternation period to align with (default: 5)
  GIF_CAPTURE_CYCLES     Complication changes to include in GIF (default: 5)
  GIF_DURATION_SECONDS   Override GIF length; default is cycles * cycle seconds
  GIF_FPS                GIF capture frame rate (default: 15)
  PEBBLE_RETRIES         Attempts for emulator commands (default: 6)
  PEBBLE_RETRY_DELAY     Seconds between emulator command attempts (default: 5)
  SCREENSHOT_WIPE_EMULATOR
                         Wipe running emulator data before launch (default: 1)
  SCREENSHOT_CLEAN_EMULATOR
                         Kill stale emulator processes before launch (default: 1)
  SCREENSHOT_KILL_ON_EXIT
                         Kill emulator when the script exits (default: 1)
  PEBBLE_BZ2_COMPAT      Create temporary libbz2.so.1.0 shim if needed (default: 1)
  SEED_EMULATOR_DATA     Send deterministic location/sun/moon data (default: 1)
  SCREENSHOT_LAT_E6      Seed latitude in degrees * 1e6 (default: 37774900)
  SCREENSHOT_LON_E6      Seed longitude in degrees * 1e6 (default: -122419400)
  SCREENSHOT_TZ_MIN      Seed timezone offset minutes (default: -480)
USAGE
}

capture_gif=0
positional=()
for arg in "$@"; do
  case "$arg" in
    --gif)
      capture_gif=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      positional+=("$arg")
      ;;
  esac
done

model="${positional[0]:-basalt}"
output="${positional[1]:-}"
startup_delay_seconds="${STARTUP_DELAY_SECONDS:-5}"
gif_cycle_seconds="${GIF_CYCLE_SECONDS:-5}"
gif_capture_cycles="${GIF_CAPTURE_CYCLES:-5}"
gif_fps="${GIF_FPS:-15}"
if [ -n "${GIF_DURATION_SECONDS:-}" ]; then
  gif_duration_seconds="$GIF_DURATION_SECONDS"
else
  gif_duration_seconds=$((gif_capture_cycles * gif_cycle_seconds))
fi
pebble_retries="${PEBBLE_RETRIES:-6}"
kill_emulator_on_exit="${SCREENSHOT_KILL_ON_EXIT:-1}"
pebble_retry_delay="${PEBBLE_RETRY_DELAY:-5}"
wipe_emulator="${SCREENSHOT_WIPE_EMULATOR:-1}"
clean_emulator="${SCREENSHOT_CLEAN_EMULATOR:-1}"
pebble_bz2_compat="${PEBBLE_BZ2_COMPAT:-1}"
seed_emulator_data="${SEED_EMULATOR_DATA:-1}"
seed_lat_e6="${SCREENSHOT_LAT_E6:-37774900}"
seed_lon_e6="${SCREENSHOT_LON_E6:--122419400}"
seed_tz_min="${SCREENSHOT_TZ_MIN:--480}"
bundle="${ROOT_DIR}/build/yes-watch.pbw"
message_keys_file="${ROOT_DIR}/build/src/message_keys.auto.c"
compat_dir=""

default_output_for_model() {
  local target_model="$1"
  if [ "$capture_gif" = "1" ]; then
    printf 'screenshots/%s/app-start.gif' "$target_model"
  else
    printf 'screenshots/%s/app-start.png' "$target_model"
  fi
}

cleanup() {
  if [ -n "$compat_dir" ] && [ -d "$compat_dir" ]; then
    rm -rf "$compat_dir"
  fi
  if [ "$kill_emulator_on_exit" != "0" ]; then
    pebble kill >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

run_with_retries() {
  local description="$1"
  shift

  local attempt=1
  while true; do
    if "$@"; then
      return 0
    fi

    if [ "$attempt" -ge "$pebble_retries" ]; then
      echo "${description} failed after ${attempt} attempts." >&2
      return 1
    fi

    echo "${description} failed; retrying in ${pebble_retry_delay}s (${attempt}/${pebble_retries})..." >&2
    sleep "$pebble_retry_delay"
    attempt=$((attempt + 1))
  done
}

install_watchface() {
  local attempt=1
  while true; do
    if [ "$attempt" -eq 1 ]; then
      if pebble install "$bundle" --emulator "$model"; then
        return 0
      fi
    elif pebble install "$bundle"; then
      return 0
    fi

    if [ "$attempt" -ge "$pebble_retries" ]; then
      echo "Install on ${model} failed after ${attempt} attempts." >&2
      return 1
    fi

    echo "Install on ${model} failed; retrying in ${pebble_retry_delay}s (${attempt}/${pebble_retries})..." >&2
    sleep "$pebble_retry_delay"
    attempt=$((attempt + 1))
  done
}

message_key() {
  local symbol="$1"
  local key
  key="$(awk -v symbol="$symbol" '$2 == symbol { gsub(/;/, "", $4); print $4 }' "$message_keys_file")"
  if [ -z "$key" ]; then
    echo "Could not find generated message key: ${symbol}" >&2
    exit 1
  fi
  printf '%s\n' "$key"
}

setup_pebble_emulator_compat() {
  if [ "$pebble_bz2_compat" = "0" ]; then
    return
  fi

  local ldconfig_output
  ldconfig_output="$(ldconfig -p 2>/dev/null || true)"
  if [ -z "$ldconfig_output" ]; then
    return
  fi

  if awk '/libbz2[.]so[.]1[.]0[[:space:]]/ { found = 1 } END { exit(found ? 0 : 1) }' <<<"$ldconfig_output"; then
    return
  fi

  local bz2_lib
  bz2_lib="$(awk '/libbz2[.]so[.]1[[:space:]]/ { print $NF; exit }' <<<"$ldconfig_output")"
  if [ -z "$bz2_lib" ] || [ ! -e "$bz2_lib" ]; then
    return
  fi

  compat_dir="$(mktemp -d)"
  ln -s "$bz2_lib" "${compat_dir}/libbz2.so.1.0"
  export LD_LIBRARY_PATH="${compat_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
  echo "Using temporary Pebble emulator libbz2 compatibility shim (${bz2_lib} -> libbz2.so.1.0)."
}

clean_stale_emulator() {
  if [ "$clean_emulator" = "0" ]; then
    return
  fi

  echo "Stopping any stale Pebble emulator processes..."
  pebble kill >/dev/null 2>&1 || true
  sleep 1
}

wipe_running_emulator() {
  if [ "$wipe_emulator" = "0" ]; then
    return
  fi

  echo "Wiping any running Pebble emulator data..."
  pebble wipe >/dev/null 2>&1 || true
}

seed_watchface_data() {
  local now_unix
  now_unix="$(date +%s)"

  run_with_retries "Seed watchface data on ${model}" \
    pebble send-app-message --emulator "$model" --int \
      "$(message_key MESSAGE_KEY_KEY_LAT_E6)=${seed_lat_e6}" \
      "$(message_key MESSAGE_KEY_KEY_LON_E6)=${seed_lon_e6}" \
      "$(message_key MESSAGE_KEY_KEY_TZ_OFFSET_MIN)=${seed_tz_min}" \
      "$(message_key MESSAGE_KEY_KEY_LOC_UNIX)=${now_unix}" \
      "$(message_key MESSAGE_KEY_KEY_HOME_SUN_STATE)=0" \
      "$(message_key MESSAGE_KEY_KEY_HOME_SUNRISE_MIN)=420" \
      "$(message_key MESSAGE_KEY_KEY_HOME_SUNSET_MIN)=1140" \
      "$(message_key MESSAGE_KEY_KEY_HOME_MOON_STATE)=0" \
      "$(message_key MESSAGE_KEY_KEY_HOME_MOONRISE_MIN)=600" \
      "$(message_key MESSAGE_KEY_KEY_HOME_MOONSET_MIN)=1320" \
      "$(message_key MESSAGE_KEY_KEY_MOON_PHASE_E6)=500000"
}

validate_model() {
  local supported
  for supported in "${ALL_MODELS[@]}"; do
    if [ "$model" = "$supported" ]; then
      return 0
    fi
  done

  echo "Unsupported emulator model: ${model}" >&2
  echo "Expected one of: all, ${ALL_MODELS[*]}" >&2
  exit 2
}

prepare_emulator() {
  wipe_running_emulator
  clean_stale_emulator
}

capture_asset() {
  local asset_output="${1:-$(default_output_for_model "$model")}"

  validate_model
  mkdir -p "$(dirname "$asset_output")"
  prepare_emulator

  echo "Starting ${model} emulator and installing watchface..."
  install_watchface

  if [ "$seed_emulator_data" != "0" ]; then
    echo "Seeding emulator data for watchface rendering..."
    seed_watchface_data
  fi

  if [ "$capture_gif" = "1" ]; then
    if ! command -v ffmpeg >/dev/null 2>&1; then
      echo "ffmpeg is required for GIF capture." >&2
      exit 1
    fi

    echo "Recording GIF to ${asset_output}..."
    python3 "${ROOT_DIR}/scripts/capture-emulator-gif.py" \
      --platform "$model" \
      --output "$asset_output" \
      --delay "$startup_delay_seconds" \
      --duration "$gif_duration_seconds" \
      --cycle-seconds "$gif_cycle_seconds" \
      --cycles "$gif_capture_cycles" \
      --fps "$gif_fps"
  else
    echo "Waiting ${startup_delay_seconds}s for app startup..."
    sleep "$startup_delay_seconds"

    echo "Capturing screenshot to ${asset_output}..."
    run_with_retries "Screenshot from ${model}" \
      pebble screenshot "$asset_output" --emulator "$model" --no-open
  fi

  echo "Saved ${asset_output}"

  if [ "$kill_emulator_on_exit" != "0" ]; then
    pebble kill >/dev/null 2>&1 || true
    sleep 1
  fi
}

load_target_platforms

if [ "$model" = "all" ]; then
  if [ -n "$output" ]; then
    echo "Output path is ignored when capturing all models." >&2
  fi

  setup_pebble_emulator_compat

  echo "Building ${bundle}..."
  (cd "$ROOT_DIR" && pebble build)

  succeeded_models=()
  failed_models=()

  for model in "${ALL_MODELS[@]}"; do
    echo ""
    echo "=== ${model} ==="
    if capture_asset "$(default_output_for_model "$model")"; then
      succeeded_models+=("$model")
    else
      failed_models+=("$model")
      echo "Capture for ${model} failed; continuing with remaining models." >&2
    fi
  done

  echo ""
  if [ "${#succeeded_models[@]}" -gt 0 ]; then
    if [ "$capture_gif" = "1" ]; then
      echo "Saved GIFs for: ${succeeded_models[*]}"
    else
      echo "Saved screenshots for: ${succeeded_models[*]}"
    fi
  fi
  if [ "${#failed_models[@]}" -gt 0 ]; then
    echo "Failed captures for: ${failed_models[*]}" >&2
    exit 1
  fi
else
  if [ -z "$output" ]; then
    output="$(default_output_for_model "$model")"
  fi

  setup_pebble_emulator_compat

  echo "Building ${bundle}..."
  (cd "$ROOT_DIR" && pebble build)

  capture_asset "$output"
fi
