#!/bin/bash
# Rebuild the stock vendor modules of FiiO M21 FW 1.1.0: Qualcomm ones from public sources, FiiO/Quectel ones from vendor/fiio-modules.
#   1. msm-kernel with gki_defconfig + vendor/bengal_GKI.config + vendor/bengal-m21.config  -> in-tree modules
#   2. each techpack as an external module against that build                                -> techpack modules
#   3. the FiiO/Quectel modules reconstructed in vendor/fiio-modules (last step of "techpacks")
# Sources come from scripts/fetch-vendor-sources.sh.  Output: $VOUT/kernel (in-tree) and $VDIST (all .ko).
# The rebuilt modules are for reference and comparison (scripts/compare-vendor-modules.py); the M21 kernel
# itself does not need them.  Usage: build-vendor-modules.sh [kernel|techpacks|all]   (default all)
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
VWORK=${VWORK:-$HOME/m21-vendor}
VOUT=${VOUT:-$VWORK/out}
VDIST=${VDIST:-$VWORK/dist}
CLANG=${CLANG:-$HOME/toolchains/clang-r450784e}
JOBS=${JOBS:-$(nproc)}
SRC=$VWORK/kernel_platform/msm-kernel
V=$VWORK/vendor/qcom/opensource
LOG=$VOUT/logs
STAGE=${1:-all}

[ -d "$SRC" ] || { echo "missing $SRC: run scripts/fetch-vendor-sources.sh first" >&2; exit 1; }
"$CLANG/bin/clang" --version | grep -q 'r450784e' || { echo "need clang r450784e at $CLANG" >&2; exit 1; }
export PATH=$CLANG/bin:$PATH
mkdir -p "$VOUT/kernel" "$LOG" "$VDIST"
# pahole >= 1.22 emits BTF kinds (enum64, decl/type tags) that 5.15.41's resolve_btfids/libbpf cannot load
# ("load BTF from vmlinux: Invalid argument"); 5.15.41 passes no flags to turn them off, so wrap pahole.
PAHOLE_W=$VOUT/pahole-5.15
cat > "$PAHOLE_W" <<'EOF'
#!/bin/sh
[ "$1" = --version ] && exec pahole --version
exec pahole --skip_encoding_btf_enum64 --skip_encoding_btf_decl_tag --skip_encoding_btf_type_tag "$@"
EOF
chmod +x "$PAHOLE_W"
# KCFLAGS as in msm-kernel build.config.common (qcacld keys its cfg80211 MLO API on it)
K=(make -C "$SRC" O="$VOUT/kernel" ARCH=arm64 LLVM=1 LLVM_IAS=1 -j"$JOBS" KCFLAGS=-D__ANDROID_COMMON_KERNEL__
   PAHOLE="$PAHOLE_W")

# vendor/patches/<checkout path>/*.patch: upstream fixes the stock modules carry beyond the pinned commit, and
# FiiO/Quectel changes reconstructed from the stock modules.  The series is applied to the working tree on
# top of the commit pinned in vendor/SOURCES; skipped when the touched files already hold the patched result.
apply_vendor_patches() {
  local dir=$1 pd="$HERE/vendor/patches/$1" ps base tree idx
  [ -d "$pd" ] || return 0
  ps=("$pd"/*.patch)
  base=$(awk -F'|' -v d="$dir" '{sub(/\r$/, "")} $1 == d {print $3}' "$HERE/vendor/SOURCES")
  [ -n "$base" ] || { echo "$dir: not in vendor/SOURCES" >&2; exit 1; }
  idx=$(mktemp)
  tree=$(cd "$VWORK/$dir" && export GIT_INDEX_FILE=$idx && git read-tree "$base" &&
         git apply --cached "${ps[@]}" && git write-tree)
  rm -f "$idx"
  if git -C "$VWORK/$dir" diff --quiet "$tree" -- $(sed -n 's|^+++ b/||p' "${ps[@]}" | sort -u); then
    return 0
  fi
  git -C "$VWORK/$dir" apply "${ps[@]}"
  echo "$dir: applied ${#ps[@]} patches"
}

build_kernel() {
  apply_vendor_patches kernel_platform/msm-kernel
  # extract-cert only: lets the host tool build with OpenSSL 3 (same patch as the M21 kernel)
  # (msm-kernel's extract-cert.c is 7 lines off ACK 5.15.220, hence patch(1) with fuzz, not git apply)
  local p=$(ls "$HERE"/patches/0001-*.patch)
  if ! patch -d "$SRC" -p1 -R -F3 --dry-run -s < "$p" >/dev/null 2>&1; then
    patch -d "$SRC" -p1 -F3 -N -s < "$p"
  fi
  "${K[@]}" gki_defconfig > "$LOG/kernel-config.log"
  (cd "$SRC" && scripts/kconfig/merge_config.sh -m -O "$VOUT/kernel" "$VOUT/kernel/.config" \
      arch/arm64/configs/vendor/bengal_GKI.config "$HERE/vendor/bengal-m21.config" >> "$LOG/kernel-config.log")
  "${K[@]}" olddefconfig >> "$LOG/kernel-config.log"
  echo "kernel: building in-tree modules (log: $LOG/kernel.log)"
  "${K[@]}" modules > "$LOG/kernel.log" 2>&1 || { tail -20 "$LOG/kernel.log"; exit 1; }
  echo "kernel: $(find "$VOUT/kernel" -name '*.ko' | wc -l) modules"
}

# GNU make >= 4.3 reads tab-indented lines after a rule as recipe lines; several techpack Kbuild files
# indent 'include'/assignments with tabs inside ifeq blocks.  Re-indent with spaces (no code change).
# Kbuild files that define rules keep their tab-indented recipe lines; only directives are re-indented there.
# A bare 'export' (export every variable) makes make >= 4.4 expand kbuild's rule macros into each recipe's
# environment (error "prerequisites cannot be defined in recipes" and near-endless runs); every audio
# sub-Kbuild includes its own config/*.conf, so the bare exports are dropped.
untab() {
  local f
  while IFS= read -r -d '' f; do
    sed -i -E '/^[[:space:]]*export[[:space:]]*$/d' "$f"
    if grep -qE '^[^[:space:]#][^=]*[^:=]:([^=]|$)' "$f"; then
      sed -i -E 's/^\t+((include|ifeq|ifneq|ifdef|ifndef|else|endif|export|[A-Za-z_][A-Za-z0-9_-]*[[:space:]]*[:+?]?=))/    \1/' "$f"
    else
      sed -i -E 's/^\t+/    /' "$f"
    fi
  done < <(find "$1" -name Kbuild -print0)
}

tp() {  # tp <name> <dir> [kbuild args...]
  local n=$1 d=$2; shift 2
  # TP_ONLY="audio kgsl": build only these techpacks
  [ -z "${TP_ONLY:-}" ] || [[ " $TP_ONLY " == *" $n "* ]] || return 0
  untab "$d"
  if "${K[@]}" M="$d" KERNEL_SRC="$SRC" modules "$@" > "$LOG/$n.log" 2>&1; then
    echo "ok     $n: $(find "$d" -name '*.ko' | wc -l) modules"
  else
    echo "FAIL   $n (log: $LOG/$n.log)"; grep -m3 -E 'error:|No rule' "$LOG/$n.log" | sed 's/^/         /'
  fi
}
sym() { echo "$V/$1/Module.symvers"; }

build_techpacks() {
  [ -f "$VOUT/kernel/Module.symvers" ] || { echo "build the kernel stage first" >&2; exit 1; }
  local pd
  for pd in $(cd "$HERE/vendor/patches" && find vendor -name '*.patch' -printf '%h\n' | sort -u); do
    apply_vendor_patches "$pd"
  done
  # mmrm_test_module's source (test/) is not public; build the driver only
  [ -f "$V/mmrm-driver/test/Kbuild" ] || { mkdir -p "$V/mmrm-driver/test"; : > "$V/mmrm-driver/test/Kbuild"; }
  tp securemsm "$V/securemsm-kernel" SSG_MODULE_ROOT="$V/securemsm-kernel" BOARD_PLATFORM=bengal
  tp mmrm "$V/mmrm-driver" MMRM_ROOT="$V/mmrm-driver" BOARD_PLATFORM=bengal
  # khaje WLAN platform (WCN3990 over SNOC); qcacld picks its bus (CONFIG_ROME_IF=snoc) from the same options
  local wp=(CONFIG_CNSS_OUT_OF_TREE=y CONFIG_ICNSS2=m CONFIG_ICNSS2_QMI=y CONFIG_CNSS_GENL=m
            CONFIG_WCNSS_MEM_PRE_ALLOC=m CONFIG_CNSS_UTILS=m CONFIG_CNSS_QMI_SVC=m)
  tp wlan-platform "$V/wlan/platform" WLAN_PLATFORM_ROOT="$V/wlan/platform" "${wp[@]}" \
     KBUILD_EXTRA_SYMBOLS="$(sym securemsm-kernel)"
  # qca_cld3_wlan.ko (Android renames wlan.ko); khaje pairs with WCN3990 = profile "adrastea".
  # Stock is a user build (no crash inject/pktlog/dup-detect debug) with WLAN_CTRL_NAME set,
  # which adds the qcwlanstate char device the framework uses for Wi-Fi on/off.
  tp wlan "$V/wlan/qcacld-3.0" WLAN_ROOT="$V/wlan/qcacld-3.0" WLAN_COMMON_ROOT=cmn \
     TARGET_BUILD_VARIANT=user WLAN_CTRL_NAME=wlan \
     WLAN_COMMON_INC="$V/wlan/qcacld-3.0/cmn" WLAN_FW_API="$V/wlan/fw-api" WLAN_PLATFORM_INC="$V/wlan/platform/inc" \
     WLAN_PROFILE=adrastea MODNAME=wlan DEVNAME=wlan CONFIG_QCA_CLD_WLAN=m BOARD_PLATFORM=bengal "${wp[@]}" \
     KBUILD_EXTRA_SYMBOLS="$(sym wlan/platform)"
  tp bt "$V/bt-kernel" BT_ROOT="$V/bt-kernel" CONFIG_MSM_BT_POWER=m CONFIG_BTFM_SLIM=m CONFIG_I2C_RTC6226_QCA=m \
     KBUILD_EXTRA_SYMBOLS="$(sym wlan/platform)"
  tp audio "$V/audio-kernel" AUDIO_ROOT="$V/audio-kernel" MODNAME=audio
  tp kgsl "$V/graphics-kernel" KGSL_PATH="$V/graphics-kernel" MODNAME=msm_kgsl BOARD_PLATFORM=bengal
  # msm_drm imports fiio_device_register/fiio_device_get_data_by_name from the FiiO private module
  # fiio_primary_modules, which is only built in the fiio-modules pass below; vendor/symvers/fiio-private.symvers
  # holds its export CRCs (the same as the rebuilt module's)
  tp display "$V/display-drivers/msm" DISPLAY_ROOT="$V/display-drivers" MODNAME=msm_drm BOARD_PLATFORM=bengal \
     KBUILD_EXTRA_SYMBOLS="$(sym mmrm-driver) $(sym securemsm-kernel) $HERE/vendor/symvers/fiio-private.symvers"
  tp video "$V/video-driver" VIDEO_ROOT="$V/video-driver" MODNAME=msm_video BOARD_PLATFORM=bengal \
     KBUILD_EXTRA_SYMBOLS="$(sym mmrm-driver)"
  tp camera "$V/camera-kernel" CAMERA_KERNEL_ROOT="$V/camera-kernel" KERNEL_ROOT="$SRC" MODNAME=camera BOARD_PLATFORM=bengal
  tp dataipa "$V/dataipa" KBUILD_EXTRA_SYMBOLS="$(sym securemsm-kernel)"
  tp rmnet-core "$V/datarmnet/core" KBUILD_EXTRA_SYMBOLS="$(sym dataipa)"
  tp rmnet-wlan "$V/datarmnet-ext/wlan" RMNET_CORE_PATH="$V/datarmnet/core" RMNET_CORE_INC_DIR="$V/datarmnet/core" \
     KBUILD_EXTRA_SYMBOLS="$(sym datarmnet/core)"
  tp nfc "$VWORK/vendor/nxp/opensource/driver" NFC_ROOT="$VWORK/vendor/nxp/opensource/driver" CONFIG_NXP_NFC_I2C=m \
     KBUILD_EXTRA_SYMBOLS="$(sym securemsm-kernel)"
  # FiiO/Quectel modules reconstructed from the stock binaries (vendor/fiio-modules), built out of
  # the repo in one pass, so the others resolve their imports from fiio_primary_modules
  rm -rf "$VWORK/fiio-modules"; cp -r "$HERE/vendor/fiio-modules" "$VWORK/fiio-modules"
  # the FPGA bitstream is FiiO firmware: the repo has a placeholder, the real bytes come from the local stock module
  gwko=$(find "$VWORK/stock/" -name gw-fpga.ko 2>/dev/null | head -1)
  if [ -n "$gwko" ]; then
    python3 "$HERE/scripts/extract-gw-fpga-firmware.py" "$gwko" "$VWORK/fiio-modules/fpga/gowin/gw_fpga_firmware.h"
  else
    echo "warning: no stock gw-fpga.ko under $VWORK/stock, building gw-fpga with a zero FPGA bitstream" >&2
  fi
  # same for the Hynitron touch firmware: the hyn_chips/*_fw.h headers are generated, not kept in the repo
  hynko=$(find "$VWORK/stock/" -name hynitron_touch.ko 2>/dev/null | head -1)
  [ -n "$hynko" ] || echo "warning: no stock hynitron_touch.ko under $VWORK/stock, building it with zero touch firmware" >&2
  python3 "$HERE/scripts/extract-hynitron-firmware.py" "${hynko:--}" "$VWORK/fiio-modules/input/touchscreen/hynitron_cst/hyn_chips"
  tp fiio "$VWORK/fiio-modules"
}

case $STAGE in
  kernel) build_kernel ;;
  techpacks) build_techpacks ;;
  all) build_kernel; build_techpacks ;;
  *) echo "usage: $0 [kernel|techpacks|all]" >&2; exit 2 ;;
esac

rm -rf "$VDIST"; mkdir -p "$VDIST"
find "$VOUT/kernel" "$V" "$VWORK/vendor/nxp" "$VWORK/fiio-modules" -name '*.ko' -exec cp {} "$VDIST/" \;
# file names as shipped on the device
[ -f "$VDIST/wlan.ko" ] && mv "$VDIST/wlan.ko" "$VDIST/qca_cld3_wlan.ko"
echo "dist: $(ls "$VDIST" | wc -l) modules in $VDIST"
