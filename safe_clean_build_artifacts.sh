#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "[1/3] Running make distclean if available"
if [[ -f Makefile ]]; then
  make distclean || true
fi

echo "[2/3] Removing generated config files and build artifacts"
find . \
  \( -type d \( -name .libs -o -name autom4te.cache \) \) -prune -print0 | \
  xargs -0r rm -rf

find . -type f \
  \( -name config.status -o -name config.log -o -name '*.o' -o -name '*.lo' -o -name '*.la' -o -name '*.lai' \) \
  -print0 | xargs -0r rm -f

while IFS= read -r makefile; do
  if ! git ls-files --error-unmatch "$makefile" >/dev/null 2>&1; then
    rm -f "$makefile"
  fi
done < <(find . -type f -name Makefile)

echo "[3/3] Cleanup complete"
