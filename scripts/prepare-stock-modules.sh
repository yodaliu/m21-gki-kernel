#!/bin/bash
# Prepare ALL stock module trees for scripts/verify-all-modules.py (safe to re-run):
#   vendor_boot ramdisk -> /tmp/rv/rd/lib/modules   (first-stage modules, 259 .ko)
#   vendor_dlkm.img     -> /mnt/vdlkm  (read-only)  (227 .ko)
#   system_dlkm.img     -> /mnt/sdlkm  (read-only)  (2 .ko)
# Images are taken from $FW/extracted/ when present, otherwise extracted from payload.bin into /tmp/rv/ex.
# Note: WSL drops loop mounts when the distro shuts down -- just run this script again.
set -euo pipefail
FW=${FW:-$HOME/m21-fw}
SCRATCH=/tmp/rv
mkdir -p "$SCRATCH/ex"

img() {  # img <partition> -> prints path of <partition>.img
  local p="$FW/extracted/$1.img"
  if [ ! -f "$p" ]; then
    p="$SCRATCH/ex/$1.img"
    [ -f "$p" ] || "$FW/payload-dumper-go" -partitions "$1" -o "$SCRATCH/ex" "$FW/payload.bin" >/dev/null
  fi
  echo "$p"
}

VB=$(img vendor_boot)
VD=$(img vendor_dlkm)
SD=$(img system_dlkm)

# vendor_boot v3/v4 header: page_size @8+4, vendor_ramdisk_size @24, header_size after cmdline[2048]+tags_addr+name[16]
python3 - "$VB" "$SCRATCH/ramdisk.bin" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'VNDRBOOT', 'not a vendor_boot image'
ver, page = struct.unpack_from('<II', d, 8)
rd_size = struct.unpack_from('<I', d, 24)[0]
hdr_size = struct.unpack_from('<I', d, 28 + 2048 + 4 + 16)[0]
start = -(-hdr_size // page) * page
open(sys.argv[2], 'wb').write(d[start:start + rd_size])
print(f'vendor_boot v{ver}: ramdisk {rd_size} bytes @ {start}')
EOF
rm -rf "$SCRATCH/rd" && mkdir -p "$SCRATCH/rd" && cd "$SCRATCH/rd"
(lz4 -dc "$SCRATCH/ramdisk.bin" 2>/dev/null || gzip -dc "$SCRATCH/ramdisk.bin") | cpio -idm --quiet
echo "vendor_boot modules: $(ls lib/modules/*.ko | wc -l), first-stage modules.load: $(wc -l < lib/modules/modules.load) lines"

sudo mkdir -p /mnt/vdlkm /mnt/sdlkm
mountpoint -q /mnt/vdlkm || sudo mount -o ro,loop "$VD" /mnt/vdlkm
mountpoint -q /mnt/sdlkm || sudo mount -o ro,loop "$SD" /mnt/sdlkm
echo "vendor_dlkm modules: $(sudo find /mnt/vdlkm -name '*.ko' | wc -l), system_dlkm modules: $(sudo find /mnt/sdlkm -name '*.ko' | wc -l)"
