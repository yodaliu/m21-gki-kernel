#!/bin/bash
# Move the M21 patches onto a newer ACK commit/tag and refresh patches/, config/ and UPSTREAM.
#
# Usage:  scripts/update-upstream.sh <ref> [--accept-config]
#   <ref>  release tag (recommended, e.g. android13-5.15-2027-01_r1), branch name or commit SHA
#   --accept-config  write the olddefconfig result to config/m21.config instead of stopping for review
# Environment (optional): SRC (default $HOME/kernel-common), CLANG (default $HOME/toolchains/clang-r450784e)
#
# Exit codes: 0 = done, 2 = config changed (review config/m21.config.new), 1 = error/conflict.
# After success: scripts/build.sh, scripts/verify-all-modules.py, boot test, then commit this repo.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/UPSTREAM"
REF=${1:?usage: $0 <tag|branch|commit> [--accept-config]}; ACCEPT=${2:-}
SRC=${SRC:-$HOME/kernel-common}; CLANG=${CLANG:-$HOME/toolchains/clang-r450784e}
export PATH=$CLANG/bin:$PATH
export GIT_COMMITTER_NAME=${GIT_COMMITTER_NAME:-$(git config user.name || echo m21-build)}
export GIT_COMMITTER_EMAIL=${GIT_COMMITTER_EMAIL:-$(git config user.email || echo m21-build@localhost)}
die() { echo "ERROR: $*" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then git init -q "$SRC"; git -C "$SRC" remote add origin "$UPSTREAM_URL"; fi
[ -z "$(git -C "$SRC" status --short --untracked-files=no)" ] || die "$SRC has uncommitted changes"

# 1. fetch only the requested commit (shallow) plus the current base (needed for --3way)
case "$REF" in
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) SPEC=$REF ;;
  android*-*-*_r*) SPEC=refs/tags/$REF ;;
  *) SPEC=refs/heads/$REF ;;
esac
git -C "$SRC" fetch -q --depth 1 origin "$SPEC" || die "cannot fetch $REF"
NEW=$(git -C "$SRC" rev-parse FETCH_HEAD^{commit})
git -C "$SRC" cat-file -e "$UPSTREAM_COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch -q --depth 1 origin "$UPSTREAM_COMMIT"
echo "current base: $UPSTREAM_COMMIT ($UPSTREAM_VERSION)"
echo "new base:     $NEW ($REF)"

# 2. KMI guard: the stock vendor modules only work with the same KMI generation
NEWKMI=$(git -C "$SRC" show "$NEW:build.config.common" | sed -n 's/^KMI_GENERATION=//p')
NEWBR=$(git -C "$SRC" show "$NEW:build.config.constants" | sed -n 's/^BRANCH=//p')
[ "$NEWKMI" = "$KMI_GENERATION" ] || die "KMI_GENERATION changed ($KMI_GENERATION -> $NEWKMI): stock modules will not load"
[ "$NEWBR" = "$UPSTREAM_BRANCH" ] || die "BRANCH changed ($UPSTREAM_BRANCH -> $NEWBR)"
VER=$(git -C "$SRC" show "$NEW:Makefile" | awk '/^VERSION =/{v=$3}/^PATCHLEVEL =/{p=$3}/^SUBLEVEL =/{s=$3}END{print v"."p"."s}')
echo "new version:  $VER, KMI generation $NEWKMI"

# 3. replay the patches on the new base
BR=m21-${NEW:0:12}
git -C "$SRC" switch -q -C "$BR" "$NEW"
for p in "$ROOT"/patches/*.patch; do
  git -C "$SRC" am -q --3way "$p" || die "conflict in $(basename "$p"): resolve in $SRC, 'git am --continue', then re-run this script with ref $NEW"
  echo "applied: $(basename "$p")"
done
rm -f "$ROOT"/patches/*.patch
git -C "$SRC" format-patch -q --zero-commit --no-signature "$NEW..HEAD" -o "$ROOT/patches"

# 4. config: carry config/m21.config forward with olddefconfig and report any change
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp "$ROOT/config/m21.config" "$T/.config"
make -s -C "$SRC" O="$T" ARCH=arm64 LLVM=1 BRANCH="$UPSTREAM_BRANCH" KMI_GENERATION="$KMI_GENERATION" olddefconfig >/dev/null
sed -i '/^# Linux\/arm64 .* Kernel Configuration/d' "$T/.config"
cfgchange=0
if ! diff -q <(grep '^CONFIG_\|^# CONFIG_' "$ROOT/config/m21.config") <(grep '^CONFIG_\|^# CONFIG_' "$T/.config") >/dev/null; then
  cfgchange=1
  echo "config changes from olddefconfig:"
  diff <(grep '^CONFIG_\|^# CONFIG_' "$ROOT/config/m21.config") <(grep '^CONFIG_\|^# CONFIG_' "$T/.config") | grep '^[<>]' | head -40 || true
fi
grep -qx 'CONFIG_I2C_GPIO=y' "$T/.config" || die "CONFIG_I2C_GPIO=y got lost (required on M21)"

# 5. record the new base
sed -i -e "s|^UPSTREAM_REF=.*|UPSTREAM_REF=$REF|" -e "s|^UPSTREAM_COMMIT=.*|UPSTREAM_COMMIT=$NEW|" \
       -e "s|^UPSTREAM_VERSION=.*|UPSTREAM_VERSION=$VER|" "$ROOT/UPSTREAM"
echo "UPSTREAM updated; patches regenerated; source tree on branch $BR"
if [ $cfgchange = 1 ]; then
  if [ "$ACCEPT" = --accept-config ]; then cp "$T/.config" "$ROOT/config/m21.config"; echo "config/m21.config updated"
  else cp "$T/.config" "$ROOT/config/m21.config.new"; echo "review config/m21.config.new, then mv it to config/m21.config"; exit 2; fi
fi
echo "next: scripts/build.sh && verify && boot test (see README)"
