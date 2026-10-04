#!/usr/bin/env bash
# Fails if any bundled asset lacks a LICENSES entry, or if an entry uses a
# license outside the allow-list for its kind.
#   assets/sounds/LICENSES : *.wav *.ogg *.mp3 *.flac *.oga  -> CC0-1.0, original-MIT
#   assets/icons/LICENSES  : *.svg *.png (recursive)         -> ISC, MIT, CC0-1.0, original-MIT
# LICENSES line format: <path relative to the LICENSES dir> | <source> | <license> | <url>
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
rc=0

check() { # <dir> <allowed licenses (space separated)> <find -iname patterns...>
  local dir="$1" allowed="$2"; shift 2
  local lic="$dir/LICENSES"
  [[ -f "$lic" ]] || { echo "missing $lic" >&2; rc=1; return; }
  local expr=() first=1
  for p in "$@"; do
    if [[ $first -eq 1 ]]; then first=0; else expr+=(-o); fi
    expr+=(-iname "$p")
  done
  local n=0
  while IFS= read -r -d '' f; do
    n=$((n + 1))
    local rel="${f#"$dir"/}"
    local line
    line="$(awk -F'|' -v r="$rel" '{k=$1; gsub(/^[ \t]+|[ \t]+$/, "", k)} k==r {print; exit}' "$lic")"
    if [[ -z "$line" ]]; then
      echo "no LICENSES entry for ${dir#"$root"/}/$rel" >&2; rc=1; continue
    fi
    local license
    license="$(echo "$line" | awk -F'|' '{gsub(/^[ \t]+|[ \t]+$/, "", $3); print $3}')"
    if [[ " $allowed " != *" $license "* ]]; then
      echo "${dir#"$root"/}/$rel: license '$license' not allowed ($allowed)" >&2; rc=1
    fi
  done < <(find "$dir" -type f \( "${expr[@]}" \) -print0)
  echo "${dir#"$root"/}: checked $n file(s)"
}

check "$root/assets/sounds" "CC0-1.0 original-MIT" '*.wav' '*.ogg' '*.mp3' '*.flac' '*.oga'
check "$root/assets/icons" "ISC MIT CC0-1.0 original-MIT" '*.svg' '*.png'

[[ $rc -eq 0 ]] && echo "asset licenses OK"
exit $rc
