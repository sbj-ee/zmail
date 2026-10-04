#!/usr/bin/env bash
# Regenerate docs/screenshots/gmail-{setup,signin,synced}.png and compose-send.png from the mock
# Google server (no network, no real account). Needs xvfb-run, xfwm4,
# xdotool and ImageMagick's import. Usage: tools/mock-screenshots.sh [build-dir]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
shot() { # <mode> <out.png>
  xfwm4 --compositor=off >/dev/null 2>&1 &
  local wm=$!
  sleep 2
  local log; log="$(mktemp)"
  HOME="$(mktemp -d)" timeout 60 "$build/tests/zmail_mockshot" "$1" >"$log" 2>&1 &
  local app=$!
  for _ in $(seq 1 60); do grep -q READY "$log" && break; sleep 0.5; done
  sleep 1
  local w
  if [[ $1 == compose ]]; then
    w="$(xdotool search --onlyvisible --name ' — zmail [0-9.]+$' | head -1)"
    xdotool windowactivate --sync "$w" 2>/dev/null || true; sleep 1
  elif [[ $1 == synced ]]; then
    w="$(xdotool search --onlyvisible --name '^zmail [0-9.]+$' | head -1)"
    xdotool windowmove "$w" 0 0; sleep 1
  else
    w="$(xdotool search --onlyvisible --name '^Connect your Gmail$' | head -1)"
  fi
  import -frame -window "$w" "$2"
  kill "$app" "$wm" 2>/dev/null || true
  wait 2>/dev/null || true
}
export -f shot
for m in setup signin synced compose; do
  out="$root/docs/screenshots/gmail-$m.png"
  [[ $m == compose ]] && out="$root/docs/screenshots/compose-send.png"
  xvfb-run -a -s "-screen 0 1400x900x24" bash -c "build='$build'; $(declare -f shot); shot $m '$out'"
  ls -l "$out"
done
