#!/bin/bash
# Build the FiiO M21 kernel Image: ACK (UPSTREAM_COMMIT) + patches/ + config/m21.config
#
# Usage:  scripts/build.sh [--check]
#   --check  only verify toolchain, source tree and patches; do not build
# Environment (optional):
#   SRC    ACK git clone (shallow is fine)      default: $HOME/kernel-common
#   OUT    build output directory               default: $HOME/out-m21
#   CLANG  AOSP prebuilt clang r450784e         default: $HOME/toolchains/clang-r450784e
#   DIST   copies of Image/vmlinux.symvers/log  default: <repo>/dist
#   JOBS   parallel jobs                         default: nproc
#   SYMVERS_REF  reference symvers for the KMI check  default: <repo>/kmi/vmlinux.symvers
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/UPSTREAM"
SRC=${SRC:-$HOME/kernel-common}; OUT=${OUT:-$HOME/out-m21}
CLANG=${CLANG:-$HOME/toolchains/clang-r450784e}; DIST=${DIST:-$ROOT/dist}; JOBS=${JOBS:-$(nproc)}
SYMVERS_REF=${SYMVERS_REF:-$ROOT/kmi/vmlinux.symvers}
export PATH=$CLANG/bin:$PATH
export GIT_COMMITTER_NAME=${GIT_COMMITTER_NAME:-$(git config user.name || echo m21-build)}
export GIT_COMMITTER_EMAIL=${GIT_COMMITTER_EMAIL:-$(git config user.email || echo m21-build@localhost)}
die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$CLANG/bin/clang" ] || die "clang not found in $CLANG"
clang --version | head -1 | grep -q 'r450784e' || die "expected AOSP clang r450784e"
git -C "$SRC" cat-file -e "$UPSTREAM_COMMIT^{commit}" 2>/dev/null || die "$SRC lacks $UPSTREAM_COMMIT (git -C $SRC fetch --depth 1 origin $UPSTREAM_COMMIT)"
git -C "$SRC" merge-base --is-ancestor "$UPSTREAM_COMMIT" HEAD || die "$SRC HEAD is not based on $UPSTREAM_COMMIT (git -C $SRC switch --detach $UPSTREAM_COMMIT)"
grep -qx "KMI_GENERATION=$KMI_GENERATION" "$SRC/build.config.common" || die "KMI_GENERATION in $SRC differs from $KMI_GENERATION"

pending=0   # a patch counts as applied if it reverse-applies cleanly
for p in "$ROOT"/patches/*.patch; do
  if git -C "$SRC" apply --reverse --check "$p" 2>/dev/null; then echo "applied:     $(basename "$p")"
  elif [ "${1:-}" = --check ]; then echo "NOT applied: $(basename "$p")"; pending=1
  else git -C "$SRC" am -q --3way "$p" || die "git am failed for $p"; echo "now applied: $(basename "$p")"; fi
done
[ -z "$(git -C "$SRC" status --short --untracked-files=no)" ] || die "$SRC has uncommitted changes"
if [ "${1:-}" = --check ]; then [ $pending = 0 ] || die "patches not applied (run without --check)"; echo "check OK"; exit 0; fi

m() { make -C "$SRC" O="$OUT" ARCH=arm64 LLVM=1 BRANCH="$UPSTREAM_BRANCH" KMI_GENERATION="$KMI_GENERATION" "$@"; }
mkdir -p "$OUT" "$DIST"
cp "$ROOT/config/m21.config" "$OUT/.config"
m olddefconfig >/dev/null
diff -q <(grep '^CONFIG_' "$ROOT/config/m21.config") <(grep '^CONFIG_' "$OUT/.config") >/dev/null \
  || die "olddefconfig changed the config; review with scripts/update-upstream.sh or update config/m21.config"
SECONDS=0
m -j"$JOBS" Image > "$DIST/build.log" 2>&1 || { tail -20 "$DIST/build.log"; die "build failed (see $DIST/build.log)"; }
echo "build OK in ${SECONDS}s"
cp "$OUT/arch/arm64/boot/Image" "$OUT/vmlinux.symvers" "$DIST/"
m -s kernelrelease | tee "$DIST/kernelrelease.txt"
sha256sum "$DIST/Image"

# KMI check against the reference: no CRC or namespace change, nothing removed; VFS exports must have no namespace.
python3 - "$OUT/vmlinux.symvers" "$SYMVERS_REF" <<'EOF'
import sys
def r(p): return {f[1]: (f[0], f[4] if len(f) > 4 else '') for f in (l.rstrip('\n').split('\t') for l in open(p)) if f[0].startswith('0x')}
a, b = r(sys.argv[1]), r(sys.argv[2]); com = set(a) & set(b)
crc = sorted(s for s in com if a[s][0] != b[s][0]); ns = sorted(s for s in com if a[s][1] != b[s][1])
removed = sorted(set(b) - set(a)); added = sorted(set(a) - set(b))
vfs = [s for s in ('kernel_read', 'kernel_write', 'filp_open') if a.get(s, ('', 'missing'))[1]]
print(f'KMI vs reference: exports={len(a)} ref={len(b)} added={len(added)} removed={len(removed)} crc_diff={len(crc)} ns_diff={len(ns)} vfs_ns={vfs or "none"}')
for title, lst in (('removed', removed), ('crc_diff', crc), ('ns_diff', ns)):
    if lst: print(f'  {title}: {", ".join(lst[:10])}{" ..." if len(lst) > 10 else ""}')
sys.exit(1 if crc or ns or removed or vfs else 0)
EOF
