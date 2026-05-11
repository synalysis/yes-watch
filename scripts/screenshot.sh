#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/screenshot.sh [model] [output]

Starts a Pebble emulator, installs the watchface, waits for app startup, and
writes a screenshot under screenshots/<model>.

Arguments:
  model   Pebble model/platform to run (default: basalt)
  output  Screenshot path (default: screenshots/<model>/app-start.png)

Environment:
  STARTUP_DELAY_SECONDS  Seconds to wait after install before capture (default: 5)
  PEBBLE_RETRIES         Attempts for emulator commands (default: 6)
  PEBBLE_RETRY_DELAY     Seconds between emulator command attempts (default: 5)
  SEED_EMULATOR_DATA     Send deterministic location/sun/moon data (default: 1)
  SCREENSHOT_LAT_E6      Seed latitude in degrees * 1e6 (default: 37774900)
  SCREENSHOT_LON_E6      Seed longitude in degrees * 1e6 (default: -122419400)
  SCREENSHOT_TZ_MIN      Seed timezone offset minutes (default: -480)
USAGE
}

case "${1:-}" in
  -h|--help)
    usage
    exit 0
    ;;
esac

model="${1:-basalt}"
output="${2:-screenshots/${model}/app-start.png}"
startup_delay_seconds="${STARTUP_DELAY_SECONDS:-5}"
pebble_retries="${PEBBLE_RETRIES:-6}"
pebble_retry_delay="${PEBBLE_RETRY_DELAY:-5}"
seed_emulator_data="${SEED_EMULATOR_DATA:-1}"
seed_lat_e6="${SCREENSHOT_LAT_E6:-37774900}"
seed_lon_e6="${SCREENSHOT_LON_E6:--122419400}"
seed_tz_min="${SCREENSHOT_TZ_MIN:--480}"
bundle="build/pebble-yes-watch.pbw"
message_keys_file="build/src/message_keys.auto.c"

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

seed_watchface_data() {
  local now_unix
  now_unix="$(date +%s)"

  # These values only need to move the emulator past the startup data gate.
  # The watchface persists them, just like data from the phone companion.
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

case "$model" in
  basalt|chalk|diorite|emery|flint|aplite|gabbro)
    ;;
  *)
    echo "Unsupported emulator model: ${model}" >&2
    echo "Expected one of: basalt, chalk, diorite, emery, flint, aplite, gabbro" >&2
    exit 2
    ;;
esac

mkdir -p "$(dirname "$output")"

echo "Building ${bundle}..."
pebble build

echo "Starting ${model} emulator and installing watchface..."
run_with_retries "Install on ${model}" pebble install "$bundle" --emulator "$model"

if [ "$seed_emulator_data" != "0" ]; then
  echo "Seeding emulator data for watchface rendering..."
  seed_watchface_data
fi

echo "Waiting ${startup_delay_seconds}s for app startup..."
sleep "$startup_delay_seconds"

echo "Capturing screenshot to ${output}..."
run_with_retries "Screenshot from ${model}" pebble screenshot "$output" --emulator "$model" --no-open

echo "Saved ${output}"
