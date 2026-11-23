#!/bin/bash
# Package a kernel Image into a FiiO M21 boot.img (header v4, kernel only, AVB hash footer like stock FW 1.1.0).
#
# Usage:  ./pack.sh [Image] [output.img]
#   Image       default: <repo>/dist/Image (from scripts/build.sh)
#   output.img  default: <repo>/dist/M21-boot-<kernelrelease>.img
# Environment (optional):
#   MKBOOTIMG_DIR  AOSP platform/system/tools/mkbootimg checkout  default: $HOME/tools/mkbootimg
#   AVBTOOL        AOSP external/avb avbtool.py                    default: $HOME/tools/avbtool.py
#   STOCK_BOOT     stock boot.img to compare header arguments with (recommended)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${1:-$ROOT/dist/Image}
MKBOOTIMG_DIR=${MKBOOTIMG_DIR:-$HOME/tools/mkbootimg}; AVBTOOL=${AVBTOOL:-$HOME/tools/avbtool.py}
die() { echo "ERROR: $*" >&2; exit 1; }
[ -f "$IMG" ] || die "kernel Image not found: $IMG"
[ -f "$MKBOOTIMG_DIR/mkbootimg.py" ] || die "mkbootimg.py not found in $MKBOOTIMG_DIR"
[ -f "$AVBTOOL" ] || die "avbtool.py not found: $AVBTOOL"
REL=$(grep -a -m1 -oE 'Linux version [0-9][^ ]*' "$IMG" | head -1 | cut -d' ' -f3 || true)
[ -n "$REL" ] || die "cannot read kernel release from $IMG"
OUTIMG=${2:-$(dirname "$IMG")/M21-boot-$REL.img}

# Values copied from the stock FW 1.1.0 boot.img header and AVB footer.
python3 "$MKBOOTIMG_DIR/mkbootimg.py" --header_version 4 --kernel "$IMG" \
  --os_version 13.0.0 --os_patch_level 2023-02 --output "$OUTIMG"
python3 "$AVBTOOL" add_hash_footer --image "$OUTIMG" --partition_name boot --partition_size 100663296 \
  --algorithm NONE --hash_algorithm sha256 --salt 0808e2c6451cd7de58ae6013ad20c00d61ebd1933d74707e114a870a34fce622 \
  --rollback_index 0 --rollback_index_location 0 --flags 0 \
  --prop com.android.build.boot.fingerprint:qti/bengal_515/bengal_515:13/TKQ1.230110.001/fiio11111908:user/dev-keys \
  --prop com.android.build.boot.os_version:13 --prop com.android.build.boot.security_patch:2023-02-05

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
A=$(python3 "$MKBOOTIMG_DIR/unpack_bootimg.py" --boot_img "$OUTIMG" --out "$T/new" --format=mkbootimg)
cmp -s "$T/new/kernel" "$IMG" && echo "kernel payload == $IMG" || die "kernel payload mismatch"
if [ -n "${STOCK_BOOT:-}" ]; then
  B=$(python3 "$MKBOOTIMG_DIR/unpack_bootimg.py" --boot_img "$STOCK_BOOT" --out "$T/stock" --format=mkbootimg)
  norm() { sed -E "s#--kernel [^ ]+##; s#--ramdisk [^ ]+##" <<<"$1"; }
  [ "$(norm "$A")" = "$(norm "$B")" ] && echo "header arguments identical to stock" || die "header arguments differ from stock"
fi
echo "release: $REL"
echo "output:  $OUTIMG ($(stat -c%s "$OUTIMG") bytes)"
sha256sum "$OUTIMG" | cut -c1-64
