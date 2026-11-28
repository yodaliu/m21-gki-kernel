# FiiO M21 GKI kernel

A custom kernel for the FiiO M21 (Snapdragon 680 / SM6225, FW 1.1.0, Android 13), built from Google's Android Common Kernel `android13-5.15`. It loads all of the stock vendor modules unmodified.

The kernel can directly replace the stock boot partition. For complete build instructions and the debugging record, see [docs/M21-Kernel-Guide_EN.md](docs/M21-Kernel-Guide_EN.md).

## Contents

| Path | Description |
|---|---|
| `UPSTREAM` | Upstream URL, branch, commit, version, KMI generation, and `LOCALVERSION` |
| `patches/` | Patches applied to the upstream commit (`git format-patch` format) |
| `config/m21.config` | Complete kernel config: the stock config with `CONFIG_I2C_GPIO=y` added and `MODULE_SIG` and `TRIM_UNUSED_KSYMS` disabled |
| `kmi/vmlinux.symvers` | KMI reference: every build is compared against it; neither CRCs nor namespaces may change |
| `scripts/build.sh` | Checks the environment, applies patches, builds `Image`, and runs the KMI comparison |
| `scripts/pack.sh` | Packages boot.img using the stock parameters (header v4 with an AVB footer) |
| `scripts/prepare-stock-modules.sh` | Extracts modules from vendor_boot, vendor_dlkm, and system_dlkm in the stock firmware |
| `scripts/verify-all-modules.py` | Verifies compatibility of all stock modules offline (CRCs, unresolved symbols, duplicate exports, namespaces, and load order) |
| `scripts/test_verify_all_modules.py` | Unit tests for `verify-all-modules.py` (`cd scripts && python3 -m unittest test_verify_all_modules`, 94 tests) |
| `scripts/update-upstream.sh` | Rebases the patches onto a new upstream tag or commit and updates `UPSTREAM`, `patches/`, and the config |
| `vendor/`, `scripts/{fetch,build,compare}-vendor-*` | Rebuilds the Qualcomm vendor modules (msm-kernel in-tree + techpacks) from public CodeLinaro sources and compares them with stock: 265 of 275 are identical; 35 of the 36 FiiO/Quectel private modules are also reconstructed from the stock binaries (see [vendor/README_EN.md](vendor/README_EN.md)) |

## Requirements

- Linux or WSL2 with `git make bc bison flex perl python3 openssl-devel elfutils-libelf-devel dwarves cpio lz4 kmod` installed.
- AOSP prebuilt clang **r450784e** in `~/toolchains/clang-r450784e` (see Section 3 of the guide for download instructions).
- AOSP [mkbootimg](https://android.googlesource.com/platform/system/tools/mkbootimg) and [avbtool.py](https://android.googlesource.com/platform/external/avb), under `~/tools/` by default.
- The stock FW 1.1.0 `payload.bin` and the images extracted from it, placed in `~/m21-fw/` (see Section 4 of the guide). These are needed only for module verification.

## Build

```bash
. ./UPSTREAM
git init -q ~/kernel-common && git -C ~/kernel-common remote add origin "$UPSTREAM_URL"
git -C ~/kernel-common fetch --depth 1 origin "$UPSTREAM_COMMIT"
git -C ~/kernel-common switch -q -c m21 FETCH_HEAD

scripts/build.sh                     # Apply patches, build, and compare against kmi/ (about 12 minutes on 16 cores)
scripts/prepare-stock-modules.sh     # Optional: prepare the stock modules
sudo python3 scripts/verify-all-modules.py --kernel-config ~/out-m21/.config ~/out-m21/vmlinux.symvers
scripts/pack.sh                      # Generate dist/M21-boot-<release>.img
```

Paths can be overridden using environment variables: `SRC`, `OUT`, `CLANG`, `DIST`, `FW`, `MKBOOTIMG_DIR`, and `AVBTOOL`.

## Booting and Flashing

Boot temporarily first, and flash only after confirming that everything works. Before flashing, always back up the current boot partition with root access; see Section 7 of the guide for detailed steps.

```bash
adb reboot bootloader
fastboot boot dist/M21-boot-*.img                 # Load into RAM only; rebooting restores the original kernel
fastboot getvar current-slot                      # Confirm the slot before flashing it
fastboot flash boot_<slot> dist/M21-boot-*.img
```

## Tracking Upstream

Following the monthly release tags is recommended, for example `android13-5.15-2027-01_r1`:

```bash
scripts/update-upstream.sh android13-5.15-2027-01_r1
#   exit 0: done; exit 2: config changed, review config/m21.config.new; exit 1: patch conflict or KMI generation changed
scripts/build.sh                     # The KMI comparison must pass (crc_diff=0, ns_diff=0, removed=0)
sudo python3 scripts/verify-all-modules.py --kernel-config ~/out-m21/.config ~/out-m21/vmlinux.symvers
scripts/pack.sh && fastboot boot dist/M21-boot-*.img      # Check boot, USB, audio, Wi-Fi, and Bluetooth on the device
cp ~/out-m21/vmlinux.symvers kmi/vmlinux.symvers          # Update the reference only after confirming compatibility
git add -A && git commit -m "Rebase onto <tag>"
```

**Important notes**
- **`KMI_GENERATION` must remain 8**. The stock modules were built for android13-5.15 KMI 8; `update-upstream.sh` stops immediately if the generation changes.
- After updating, confirm that `CONFIG_I2C_GPIO=y` is still enabled, and check with root access that no device is stuck at `waiting_for_supplier=1`. Without i2c-gpio, USB fails and the device resets during boot (see Section 8 of the guide for the cause).
- The source tree must be clean with all changes committed when building, so the version string is `5.15.220-m21-android13-8-g<commit>`, without `-dirty`.

## License

`patches/` contains modifications to the Linux kernel and, like the kernel itself, is licensed under GPL-2.0-only. When distributing a compiled `Image` or boot.img, you must also provide the corresponding source: the upstream commit plus this project's patches and config. This project does not include FiiO firmware or modules.
