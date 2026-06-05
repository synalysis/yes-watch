#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_JSON="${ROOT_DIR}/package.json"
bundle="${ROOT_DIR}/build/yes-watch.pbw"
compat_dir=""
ALL_MODELS=()

usage() {
  cat <<'USAGE'
Usage: scripts/open-config.sh [model]

Without a model:
  Renders the YES Watchface settings page and opens it in your default browser
  (preview only; save does not reach a watch or emulator).

With a model:
  Wipes emulator data, installs the watchface, and opens the real settings page
  wired to the emulator (save updates pkjs storage and sends AppMessages).

Arguments:
  model   Pebble emulator model (e.g. basalt, chalk, emery)

Environment:
  CONFIG_LANG                      Preview language tag (default: en-US)
  CONFIG_USE_INTERNET              Preview initial value: 1 or 0
  CONFIG_UNITS_MODE                Preview initial value: metric or imperial
  CONFIG_UI_UPDATE_INTERVAL_SEC    Preview initial value: 5, 10, 30, or 60
  CONFIG_LANGUAGE                  Preview language code: en, de, fr, es, pt, it
  CONFIG_LAT_E6 / CONFIG_LON_E6    Preview location coordinates (degrees * 1e6)
  CONFIG_LOCATION_CITY             Preview closest-city label for those coordinates
  CONFIG_PREVIEW_OUTPUT            Preview HTML path (default: build/config-preview.html)
  CONFIG_PREVIEW_NO_OPEN           Set to 1 to render preview only, without opening a browser
  CONFIG_WIPE_EMULATOR             Wipe emulator data before launch (default: 1)
  CONFIG_CLEAN_EMULATOR            Kill stale emulator processes before launch (default: 1)
  CONFIG_KILL_ON_EXIT              Kill emulator when the script exits (default: 0)
  PEBBLE_RETRIES                   Install retry attempts (default: 6)
  PEBBLE_RETRY_DELAY               Seconds between install retries (default: 5)
  PEBBLE_BZ2_COMPAT                Create temporary libbz2.so.1.0 shim if needed (default: 1)
USAGE
}

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

validate_model() {
  local supported
  for supported in "${ALL_MODELS[@]}"; do
    if [ "$model" = "$supported" ]; then
      return 0
    fi
  done

  echo "Unsupported emulator model: ${model}" >&2
  echo "Expected one of: ${ALL_MODELS[*]}" >&2
  exit 2
}

cleanup() {
  if [ -n "$compat_dir" ] && [ -d "$compat_dir" ]; then
    rm -rf "$compat_dir"
  fi
  if [ "${kill_emulator_on_exit:-0}" != "0" ]; then
    pebble kill >/dev/null 2>&1 || true
  fi
}

setup_pebble_emulator_compat() {
  if [ "${pebble_bz2_compat:-1}" = "0" ]; then
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

wipe_running_emulator() {
  if [ "${wipe_emulator:-1}" = "0" ]; then
    return
  fi

  echo "Wiping any running Pebble emulator data..."
  pebble wipe >/dev/null 2>&1 || true
}

clean_stale_emulator() {
  if [ "${clean_emulator:-1}" = "0" ]; then
    return
  fi

  echo "Stopping any stale Pebble emulator processes..."
  pebble kill >/dev/null 2>&1 || true
  sleep 1
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

open_browser_preview() {
  local output="${CONFIG_PREVIEW_OUTPUT:-${ROOT_DIR}/build/config-preview.html}"
  export CONFIG_LANG="${CONFIG_LANG:-en-US}"

  local html_path
  html_path="$(node "${ROOT_DIR}/scripts/render-config-preview.mjs")"
  echo "Rendered ${html_path}"

  if [ "${CONFIG_PREVIEW_NO_OPEN:-0}" = "1" ]; then
    return 0
  fi

  local file_url="file://${html_path}"
  if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$file_url" >/dev/null 2>&1 &
  elif command -v open >/dev/null 2>&1; then
    open "$file_url"
  else
    echo "Open this file in your browser: ${html_path}"
  fi
}

open_emulator_config() {
  trap cleanup EXIT

  load_target_platforms
  validate_model
  setup_pebble_emulator_compat
  wipe_running_emulator
  clean_stale_emulator

  echo "Building ${bundle}..."
  (cd "$ROOT_DIR" && pebble build)

  echo "Installing watchface on ${model} emulator..."
  install_watchface

  local config_html="${ROOT_DIR}/build/config-emulator.html"
  echo "Rendering settings page to ${config_html}..."
  CONFIG_EMULATOR=1 CONFIG_PREVIEW_OUTPUT="${config_html}" \
    node "${ROOT_DIR}/scripts/render-config-preview.mjs" >/dev/null

  echo "Opening settings page for ${model} emulator..."
  # pebble-tool cannot open data: URLs from file:// (meta-refresh is blocked), so pass real HTML.
  pebble emu-app-config --emulator "$model" --file "${config_html}"
}

case "${1:-}" in
  -h|--help)
    usage
    exit 0
    ;;
esac

model="${1:-}"
pebble_retries="${PEBBLE_RETRIES:-6}"
pebble_retry_delay="${PEBBLE_RETRY_DELAY:-5}"
wipe_emulator="${CONFIG_WIPE_EMULATOR:-1}"
clean_emulator="${CONFIG_CLEAN_EMULATOR:-1}"
kill_emulator_on_exit="${CONFIG_KILL_ON_EXIT:-0}"
pebble_bz2_compat="${PEBBLE_BZ2_COMPAT:-1}"

if [ -z "$model" ]; then
  open_browser_preview
else
  open_emulator_config
fi
