#!/usr/bin/env bash
# Fails if any bundled sound in assets/sounds/ lacks a LICENSES entry, or if
# an entry uses a license other than CC0-1.0 or original-MIT.
set -euo pipefail
dir="$(cd "$(dirname "$0")/.." && pwd)/assets/sounds"
lic="$dir/LICENSES"
[[ -f "$lic" ]] || { echo "missing $lic" >&2; exit 1; }
rc=0
shopt -s nullglob nocaseglob
for f in "$dir"/*.wav "$dir"/*.ogg "$dir"/*.mp3 "$dir"/*.flac "$dir"/*.oga; do
  name="$(basename "$f")"
  line="$(grep -E "^${name//./\\.}[[:space:]]*\|" "$lic" || true)"
  if [[ -z "$line" ]]; then
    echo "no LICENSES entry for $name" >&2; rc=1; continue
  fi
  license="$(echo "$line" | awk -F'|' '{gsub(/^ +| +$/, "", $3); print $3}')"
  case "$license" in
    CC0-1.0|original-MIT) ;;
    *) echo "$name: license '$license' not allowed (CC0-1.0 or original-MIT only)" >&2; rc=1 ;;
  esac
done
[[ $rc -eq 0 ]] && echo "sound licenses OK"
exit $rc
