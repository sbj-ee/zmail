#!/bin/sh
# Every GitHub Action in .github/workflows is pinned to a full commit SHA
# with the version in a comment, e.g.
#   uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
# A tag or branch (@v7, @main) can be moved to other code after review.
# Local actions (uses: ./path) are fine. Run from the repository root.
set -eu
cd "$(dirname "$0")/.."
bad=0
found=0
for f in .github/workflows/*.yml .github/workflows/*.yaml; do
    [ -f "$f" ] || continue
    while IFS= read -r line; do
        found=$((found + 1))
        case "$line" in
            *"uses: ./"*) continue ;;
        esac
        if ! printf '%s\n' "$line" | grep -Eq 'uses:[[:space:]]*[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}[[:space:]]+#[[:space:]]*v[0-9]+(\.[0-9]+)*[[:space:]]*$'; then
            echo "$f: not pinned to a commit SHA with a version comment: $line" >&2
            bad=1
        fi
    done <<EOT
$(grep -E '^[[:space:]-]*uses:' "$f" || true)
EOT
done
if [ "$found" -eq 0 ]; then
    echo "no 'uses:' lines found in .github/workflows" >&2
    exit 1
fi
[ "$bad" -eq 0 ] && echo "all $found action references are pinned by SHA"
exit "$bad"
