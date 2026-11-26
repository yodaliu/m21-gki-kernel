#!/bin/bash
# Fetch the public Qualcomm sources listed in vendor/SOURCES into $VWORK, in Qualcomm's own layout:
#   $VWORK/kernel_platform/msm-kernel   msm-kernel (ACK 5.15.41 + Qualcomm drivers)
#   $VWORK/vendor/qcom/opensource/...   techpacks (audio, wlan, camera, display, ...)
# Each repository is a shallow (depth 1) fetch of the pinned commit, checked out detached.
# Safe to re-run: existing checkouts are only moved to the pinned commit.  About 1.5 GB in total.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
VWORK=${VWORK:-$HOME/m21-vendor}
BASE=${CLO_BASE:-https://git.codelinaro.org/clo/la}
mkdir -p "$VWORK"

grep -vE '^\s*(#|$)' "$HERE/vendor/SOURCES" | while IFS='|' read -r path proj commit; do
  d="$VWORK/$path"
  if [ ! -e "$d/.git" ]; then
    git init -q "$d"
    git -C "$d" remote add origin "$BASE/$proj.git"
  fi
  if ! git -C "$d" cat-file -e "$commit^{commit}" 2>/dev/null; then
    echo "fetch  $proj"
    git -C "$d" fetch -q --depth 1 origin "$commit"
  fi
  git -C "$d" checkout -q --detach "$commit"
  echo "ok     $path @ ${commit:0:12} ($(git -C "$d" log -1 --format=%cs HEAD))"
done
