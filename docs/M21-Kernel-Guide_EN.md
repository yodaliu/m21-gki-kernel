# A Custom GKI Kernel for the FiiO M21: A Complete Record of Building, Verification, and Debugging

> Author: Claude Opus 5.5
> Date: 2026-11-23
> Applies to: FiiO M21 (Snapdragon 680 / SM6225), firmware FW 1.1.0 (Android 13)

This document explains how to build a kernel from publicly available Android Common Kernel (ACK) source that boots properly on the FiiO M21 and is compatible with all stock driver modules. The second half provides a complete record of debugging this kernel from “unable to boot” to “booting into Android normally,” including the methods used and the evidence behind each step.

**Current status**: The kernel built according to this document has successfully booted on the device using `fastboot boot`, with the following results:
- `boot_completed=1` within 29 seconds;
- The home screen, touchscreen, headphone output, volume control, and USB adb all work normally;
- The number of modules actually loaded is the same as with the stock kernel: 310 (only the counts were compared; see Section 2.3).

The final version has been **flashed to the boot partition of slot `_a`**, after backing up the original boot partition. After flashing, Wi‑Fi connectivity and Bluetooth pairing and connectivity were also confirmed to work normally.

**Not yet verified**: Suspend/resume and extended playback.

---

## 0. Companion Files and Operating Environments

This document accompanies the **`m21-gki-kernel`** repository: <https://github.com/yodaliu/m21-gki-kernel>. The repository contains:

| File | Purpose |
|---|---|
| `patches/` | The two source patches in Section 5 |
| `config/m21.config` | The final config produced in Section 5 |
| `kmi/vmlinux.symvers` | KMI reference file for checking whether CRCs have changed |
| `scripts/build.sh`, `scripts/pack.sh` | Automate the building and packaging in Sections 5–7 |
| `scripts/prepare-stock-modules.sh`, `scripts/verify-all-modules.py` | Offline verification of stock modules in Section 6 |

Commands in this document run in two environments:

| Environment | Tasks | Notation in this document |
|---|---|---|
| **WSL2 (CentOS Stream 10)** | Download firmware, build the kernel, verify, and package | ```` ```bash ```` blocks |
| **Windows PowerShell**, with [Android platform-tools](https://developer.android.com/tools/releases/platform-tools) (adb, fastboot) installed | Operate the device over USB: back up, temporarily boot, and flash | ```` ```powershell ```` blocks |

WSL2 cannot see USB devices by default, so adb and fastboot are always run on Windows. Files generated in WSL must first be copied to a Windows directory (see Section 7.2).

All manual commands for Sections 5–7 are included in full, so you can build, package, and flash without the repository. The repository simply automates these steps and additionally provides the module verification scripts and KMI reference file for Section 6.

---

## 1. Summary of Conclusions

| Item | Details |
|---|---|
| Kernel source | ACK `android13-5.15`, commit `3540cf724bf0fe246a3131fd1f3e9342fa03b03e` (Linux 5.15.220, KMI generation 8) |
| Compiler | AOSP prebuilt clang r450784e (clang 14.0.7), the same as stock |
| Config | Based on the stock `/proc/config.gz`, with `MODULE_SIG` and `TRIM_UNUSED_KSYMS` disabled and **`CONFIG_I2C_GPIO=y` enabled** |
| Source patches | ① `extract-cert.c`: OpenSSL 3 compatibility ② Restore non-namespaced exports for `kernel_read`, `kernel_write`, and `filp_open` (matching stock) |
| Key finding | The stock kernel has the `i2c-gpio` driver built in, but the stock `/proc/config.gz` shows `# CONFIG_I2C_GPIO is not set`. Without this driver, both USB/charging failures and resets during boot occur; see Section 8 |
| Final artifacts | `5.15.220-m21-android13-8-g3540cf724bf0-dirty`, built entirely according to this document (without any debugging patches). Image SHA-256 `07d39903…486beb50`, boot.img SHA-256 `8830a1b8…754168d5`. On 2026-11-23, it was first verified using `fastboot boot`, then flashed to `boot_a`; the home screen appeared 35 seconds after reboot |

---

## 2. Device and Stock Firmware Facts

| Item | Details |
|---|---|
| Stock kernel | `5.15.41-android13-8-gf7d0984aa5e2-dirty`. Based on ACK, but modified by FiiO: commit `f7d0984aa5e2` is not in the public AOSP source |
| boot.img | Header v4, kernel only (ramdisk size 0), empty cmdline, `os_version 13.0.0`, `os_patch_level 2023-02`, with an AVB hash footer at the end (partition 100,663,296 bytes, algorithm NONE) |
| Module distribution | 259 in the vendor_boot ramdisk (87 lines in the first-stage load list), 227 in vendor_dlkm, 2 in system_dlkm (zram, zsmalloc) |
| Module signatures | All 486 vendor `.ko` files are unsigned |
| Partitions | Virtual A/B. Bootloader unlocked (orange), `veritymode=enforcing`, SELinux permissive (the test device's state; see the table below for which conditions are required) |

**Prerequisites: Which Conditions Are Required and Which Only Describe the Test Device**

| Item | Required? | How to check | If not satisfied |
|---|---|---|---|
| Firmware FW 1.1.0 | **Required** | `adb shell getprop ro.product.version` | **Stop**. The stock module ABI has only been verified for 1.1.0; for other versions, first repeat the verification in Sections 4 and 6 using that firmware version |
| Unlocked bootloader | **Required** | `fastboot getvar unlocked`, or `adb shell getprop ro.boot.flash.locked` (0 means unlocked) | **Stop**. A locked bootloader does not accept unsigned boot images. Unlocking erases data and is outside the scope of this document |
| A/B partitions | **Required** | `adb shell getprop ro.boot.slot_suffix` returns a value | Stop; the procedure in Section 7 assumes A/B |
| Root for backup | Recommended | See 7.3 | Without root, use the stock boot image from the firmware as the restoration source instead (7.3) |
| SELinux permissive | Not a requirement | `adb shell getenforce` | This only describes the test device. This kernel does not change the SELinux mode; it has not been tested on a device in enforcing mode |
| `veritymode=enforcing` | Not a requirement | — | Has no effect when flashing only boot; do not modify vbmeta or vendor partitions |

### 2.1 Evidence That the Stock Kernel Is a GKI Build

- **Version string**: In `5.15.41-android13-8-…`, `android13-8` corresponds to the ACK branch `android13-5.15` and KMI generation 8.
- **Build path**: `CONFIG_UNUSED_KSYMS_WHITELIST` in the stock config reveals FiiO's build path, `/mnt/SSD4T/work/sm6225_4/target/kernel_platform/out/msm-kernel-bengal-gki/gki_kernel/common/abi_symbollist.raw`, which points to `common/` in Qualcomm's kernel_platform, i.e. ACK.
- **Platform drivers are not in the kernel**: The stock config explicitly disables QCOM platform drivers, for example `# CONFIG_COMMON_CLK_QCOM is not set`; `QCOM_SMEM`, `PINCTRL_MSM`, `ARM_SMMU`, `SLIMBUS`, and `QRTR` are also disabled. Strings such as `ipc_log_string`, `clk_rcg2_ops`, `qcom_smem_get`, and `walt_pause_cpus` are also absent from the stock Image. These symbols are exported by modules in vendor_boot, such as `clk-qcom.ko`, `qcom-scm.ko`, `qcom_ipc_logging.ko`, `smem.ko`, and `sched-walt.ko`.
- **Empirical evidence**: Comparing the kernel built from ACK plus the stock config against all 25,523 symbol references in all 488 stock module files yielded 0 CRC mismatches, 0 unresolved symbols, and 0 duplicate exports.
- **But FiiO did modify the kernel itself**: Commit `f7d0984aa5e2` is not in the public AOSP source; differences actually found include the built-in `i2c-gpio` driver and VFS export namespaces (see below).

### 2.2 How Stock Modules Are Loaded

1. **First-stage init**: Loads basic platform drivers, such as clocks, pinctrl, SCM, SMMU, watchdog, and qseecom, in **strict order** according to `lib/modules/modules.load` in the vendor_boot ramdisk (87 lines, 84 unique modules). If any one fails to load, init aborts boot and requests a reboot.
2. **Second stage**: During early-init, `init.qti.kernel.rc` starts `/vendor/bin/vendor_modprobe.sh` to load `modules.load` from vendor_dlkm (260 lines, 227 unique modules).
   - First filters according to the blocklist.
   - The **first module in the list must succeed**; otherwise, the script tries `/vendor/lib/modules` instead and ultimately exits with status 1.
   - The remaining modules are **loaded in parallel** in the background; individual failures are simply skipped.
   - Once all attempts finish, it sets `vendor.all.modules.ready=1`. During post-fs, `init.qti.kernel.rc` uses `wait_for_prop` to wait for this property.
3. **system_dlkm**: `/vendor/bin/system_dlkm_modprobe.sh` uses the wildcard `/system_dlkm/lib/modules/*/` to find the directory and load zram and zsmalloc, so changes to `uname -r` do not affect it.
4. **vermagic**: When modules carry CRCs (modversions), `same_magic()` skips the version string and compares only the flags `SMP preempt mod_unload modversions aarch64`. This allows modules for 5.15.41 to load directly into a 5.15.220 kernel.

### 2.3 The Scope of “Compatible”: What the Numbers Mean

| Number | Meaning | Verification |
|---|---|---|
| **488** | Number of `.ko` **files** across the three partitions: vendor_boot 259 + vendor_dlkm 227 + system_dlkm 2 | **Static**: `verify-all-modules.py` checks all 25,523 symbol references; CRC mismatches, unresolved symbols, duplicate exports, and namespace issues are all 0 |
| **311** | Number of distinct module **names**. 177 names occur in both vendor_boot and vendor_dlkm, as different files | Same as above (static) |
| **486** | Number of vendor `.ko` files (259 + 227), all **unsigned** | Static: check for the signature marker at the end of each file |
| **310** | Number of modules **actually loaded** in `/proc/modules` after boot: 310 for both stock and this kernel | **On-device**: only the counts were compared, not the names one by one |

Thus, “all modules are compatible” means that all 488 files passed static ABI checks, the number of modules loaded after boot on the device matches stock, and the main functions work normally. This **does not mean** that every module behaves exactly like stock in every hardware state.

**Three Differences from the Stock Kernel That Must Be Addressed:**

1. **i2c-gpio driver**: Built into stock (`soc:i2c_gpio` binds to `i2c-gpio`, with the `0-0022` fusb302 beneath it providing `extcon1`), yet not reflected in the config.
2. **VFS export namespaces**: Stock `kernel_read`, `kernel_write`, and `filp_open` have no namespace; ACK places them under `VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver`. The stock `hynitron_touch.ko` uses these symbols but does not declare the import, so it cannot load unless this is fixed.
3. **Module signatures / PROTECT**: Stock enables `MODULE_SIG=y` and `MODULE_SIG_PROTECT=y`. We do not have FiiO's key for our kernel, so these are simply disabled.

---

## 3. Environment (WSL2 CentOS Stream 10)

```bash
sudo dnf install -y git make bc bison flex perl python3 curl unzip file \
  openssl openssl-devel elfutils-libelf-devel ncurses-devel dwarves cpio lz4 kmod
mkdir -p ~/toolchains/clang-r450784e && cd ~/toolchains
curl -fL -o clang.tar.gz 'https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/5538393cf30cb39da6d53730a9b85f36958230da/clang-r450784e.tar.gz'
tar xzf clang.tar.gz -C clang-r450784e && rm clang.tar.gz     # The tarball has no top-level directory
export PATH=$HOME/toolchains/clang-r450784e/bin:$PATH
clang --version   # Android (8508608, based on r450784e) clang version 14.0.7
```

- `cpio`: Required at build time because the stock config enables `CONFIG_IKHEADERS=y`.
- `dwarves` (pahole): Required at build time because the stock config enables `CONFIG_DEBUG_INFO_BTF=y`.
- Do not use the distribution's bundled clang: it lacks the ARM32 backend and cannot compile the compat vDSO.

**Packaging tools**: Use AOSP's mkbootimg and avbtool to package boot.img.

```bash
mkdir -p ~/tools
git clone --depth 1 https://android.googlesource.com/platform/system/tools/mkbootimg ~/tools/mkbootimg
curl -fsSL 'https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py?format=TEXT' \
  | base64 -d > ~/tools/avbtool.py
python3 ~/tools/avbtool.py version     # avbtool 1.3.0
```

**This document's repository**:

```bash
git clone https://github.com/yodaliu/m21-gki-kernel ~/m21-gki-kernel
cd ~/m21-gki-kernel                    # Run the scripts/ commands in Sections 6 and 7 here
```

---

## 4. Obtaining Stock Materials

```bash
mkdir -p ~/m21-fw && cd ~/m21-fw
curl -fL -C - -o M21-update-1.1.0.zip 'https://m21pack.oss-cn-beijing.aliyuncs.com/1.0.9/M21-update-1.1.0.zip'
unzip -o M21-update-1.1.0.zip payload.bin
curl -fL -o pdg.tar.gz https://github.com/ssut/payload-dumper-go/releases/download/1.2.2/payload-dumper-go_1.2.2_linux_amd64.tar.gz
tar xzf pdg.tar.gz
./payload-dumper-go -partitions boot,vendor_boot,vendor_dlkm,system_dlkm,init_boot -o extracted payload.bin
```

**Stock config**: The kernel embeds the gzip-compressed config in boot.img between the markers `IKCFG_ST` and `IKCFG_ED`. Extract it and save it as `~/m21-fw/m21-kernel-config.txt` for use in Section 5:

```bash
cd ~/m21-fw
python3 - <<'EOF'
import gzip
d = open('extracted/boot.img', 'rb').read()
i, j = d.find(b'IKCFG_ST'), d.find(b'IKCFG_ED')
open('m21-kernel-config.txt', 'wb').write(gzip.decompress(d[i + 8:j]))
EOF
md5sum m21-kernel-config.txt      # 089add4afe3bbc46f3cc7712d591d8a8, 187,988 bytes
head -3 m21-kernel-config.txt     # Should show "Linux/arm64 5.15.41 Kernel Configuration"
```

Two other ways to obtain it produce exactly the same result:
- After obtaining the kernel source in Section 5, run `~/kernel-common/scripts/extract-ikconfig extracted/boot.img > m21-kernel-config.txt`;
- Read it from a device running the stock kernel: run `adb pull /proc/config.gz` in PowerShell, copy it to WSL, and decompress it using `gunzip -c config.gz > m21-kernel-config.txt`. Do not redirect it with `>` in PowerShell, which changes the encoding and line endings.

---

## 5. Source, Patches, and Config

> Shortcut: This project's `patches/`, `config/m21.config`, and `scripts/build.sh` automate the build steps in this section and Section 6; `scripts/pack.sh` handles packaging in Section 7. The manual steps below explain what each step does.

```bash
mkdir -p ~/kernel-common && cd ~/kernel-common && git init -q
git fetch --depth 1 https://android.googlesource.com/kernel/common 3540cf724bf0fe246a3131fd1f3e9342fa03b03e
git checkout -q --detach FETCH_HEAD
```

**Patch 1: `scripts/extract-cert.c`**
OpenSSL 3.5 no longer declares the ENGINE API, causing the host tool to fail to compile. Change the condition for the pkcs11 branch to:

```c
#if defined(OPENSSL_IS_BORINGSSL) || OPENSSL_VERSION_NUMBER >= 0x30000000L
```

**Patch 2: VFS Export Namespaces (Match Stock)**

```diff
--- fs/open.c
-EXPORT_SYMBOL_NS(filp_open, ANDROID_GKI_VFS_EXPORT_ONLY);
+EXPORT_SYMBOL(filp_open);
--- fs/read_write.c
-EXPORT_SYMBOL_NS(kernel_read, ANDROID_GKI_VFS_EXPORT_ONLY);
+EXPORT_SYMBOL(kernel_read);
-EXPORT_SYMBOL_NS(kernel_write, ANDROID_GKI_VFS_EXPORT_ONLY);
+EXPORT_SYMBOL(kernel_write);
```

**Config**

```bash
export PATH=$HOME/toolchains/clang-r450784e/bin:$PATH
m() { make -C ~/kernel-common O=$HOME/out-m21 ARCH=arm64 LLVM=1 BRANCH=android13-5.15 KMI_GENERATION=8 "$@"; }
mkdir -p ~/out-m21 && cp ~/m21-fw/m21-kernel-config.txt ~/out-m21/.config
~/kernel-common/scripts/config --file ~/out-m21/.config \
  -d MODULE_SIG -d MODULE_SIG_ALL -d MODULE_SIG_SHA1 -d MODULE_SIG_FORCE -d MODULE_SIG_PROTECT \
  -d TRIM_UNUSED_KSYMS -e I2C_GPIO --set-str LOCALVERSION "-m21"
m olddefconfig
grep -E '^CONFIG_(I2C_GPIO|MODVERSIONS|KASAN|LTO_CLANG_FULL)=' ~/out-m21/.config
```

| Setting | Reason |
|---|---|
| `I2C_GPIO=y` | **Required**. Without it, the `fusb30x@22` extcon device never appears, and fw_devlink keeps blocking the USB controller and FiiO power board (see Section 8) |
| Disable `MODULE_SIG*` | OpenSSL 3 on CentOS 10 refuses to generate SHA1 signing keys; the stock modules are unsigned anyway |
| Disable `TRIM_UNUSED_KSYMS` | The stock whitelist points to an absolute path on FiiO's build machine; disabling it exports all symbols |
| Preserve stock settings such as KASAN | `CONFIG_KASAN` changes the layout of `struct kmem_cache`; disabling it affects CRCs |

Each `-e` or `-d` in `scripts/config` accepts only one option.

---

## 6. Building and Offline Verification

```bash
m -j"$(nproc)" Image            # About 12 minutes on 16 cores
m -s kernelrelease
```

Prepare all stock modules (extract the vendor_boot ramdisk and mount vendor_dlkm and system_dlkm read-only), then check CRCs, unresolved symbols, and duplicate exports:

```bash
scripts/prepare-stock-modules.sh             # Reads stock images under $FW (default ~/m21-fw)
sudo python3 scripts/verify-all-modules.py --kernel-config ~/out-m21/.config ~/out-m21/vmlinux.symvers
# Result: OVERALL: PASS (exit 0); exit 1 on FAIL; exit 2 for invalid input
```

These two scripts are in the repository's `scripts/` directory (Section 0); run them from `~/m21-gki-kernel`. `prepare-stock-modules.sh` extracts the vendor_boot ramdisk to `/tmp/rv` and mounts vendor_dlkm and system_dlkm read-only at `/mnt/vdlkm` and `/mnt/sdlkm`; mounting requires sudo. In addition to CRCs, unresolved symbols, and duplicate exports, `verify-all-modules.py` checks load order by boot stage, namespace imports, and GPL restrictions.

**Also confirm that the KMI has not changed**: Compare this build's `vmlinux.symvers` against a reference file. Use the repository's `kmi/vmlinux.symvers` or a previously verified build as the reference. CRC differences, namespace differences, and removed exports must all be 0, and `kernel_read`, `kernel_write`, and `filp_open` must have no namespace. `scripts/build.sh` performs this comparison automatically; for a manual build, run:

```bash
python3 - ~/out-m21/vmlinux.symvers ~/m21-gki-kernel/kmi/vmlinux.symvers <<'EOF'
import sys
def r(p): return {f[1]: (f[0], f[4] if len(f) > 4 else '') for f in (l.rstrip('\n').split('\t') for l in open(p)) if f[0].startswith('0x')}
a, b = r(sys.argv[1]), r(sys.argv[2]); com = set(a) & set(b)
crc = [s for s in com if a[s][0] != b[s][0]]; ns = [s for s in com if a[s][1] != b[s][1]]; rm = set(b) - set(a)
vfs = [s for s in ('kernel_read', 'kernel_write', 'filp_open') if a.get(s, ('', 'missing'))[1]]
print(f'exports={len(a)} crc_diff={len(crc)} ns_diff={len(ns)} removed={len(rm)} vfs_ns={vfs or "none"}')
sys.exit(1 if crc or ns or rm or vfs else 0)
EOF
# Expected: exports=15477 crc_diff=0 ns_diff=0 removed=0 vfs_ns=none
```

Actual testing found 15,477 exports both before and after enabling `I2C_GPIO`, with 0 CRC differences.

---

## 7. Packaging, Backup, Temporary Boot, and Flashing

**Follow the order 7.1 → 7.6.** Complete the backup first, while the original system is still present and has root. Whether root remains available after temporarily booting the new kernel depends on the root method:

| Root method | Where root resides | After temporarily booting or flashing this kernel |
|---|---|---|
| APatch, KernelSU (patch boot or the kernel) | The kernel in the boot partition | **Root is lost** unless you first patch `m21-boot.img` with the same tool (this document's tests confirmed that it works normally after APatch patching) |
| Magisk patching init_boot | The ramdisk in init_boot, which this kernel does not touch | Root **should remain available**: this kernel reuses init_boot on the device's partition. This is an inference, not tested in this document |

Regardless of the method, complete the backup **before** the temporary boot.

### 7.1 Packaging (WSL)

```bash
cd ~/m21-gki-kernel
STOCK_BOOT=~/m21-fw/extracted/boot.img scripts/pack.sh ~/out-m21/arch/arm64/boot/Image ~/m21-boot.img
```

`pack.sh` is equivalent to the following two commands and also checks that the header parameters match stock:

```bash
python3 ~/tools/mkbootimg/mkbootimg.py --header_version 4 --kernel ~/out-m21/arch/arm64/boot/Image \
  --os_version 13.0.0 --os_patch_level 2023-02 --output ~/m21-boot.img
python3 ~/tools/avbtool.py add_hash_footer --image ~/m21-boot.img --partition_name boot --partition_size 100663296 \
  --prop com.android.build.boot.os_version:13 --prop com.android.build.boot.security_patch:2023-02-05
```

- Keep the stock `os_version` and `os_patch_level` values; **do not lower them**.
- No ramdisk is needed: the stock `init_boot` provides the generic ramdisk.

### 7.2 Transfer to Windows

In WSL, copy the images to a Windows working directory. Run all subsequent PowerShell commands from that directory:

```bash
mkdir -p /mnt/c/m21 && cp ~/m21-boot.img ~/m21-fw/extracted/boot.img /mnt/c/m21/
mv /mnt/c/m21/boot.img /mnt/c/m21/stock-boot-fw1.1.0.img      # Stock boot, the last-resort restoration image
```

```powershell
cd C:\m21
Get-FileHash m21-boot.img -Algorithm SHA256       # Compare with sha256sum in WSL
```

### 7.3 Back Up the Current Boot (On the Original System; Requires Root)

First confirm the slot currently used to boot. The example below uses `_a`; if yours is `_b`, replace every `boot_a` with `boot_b`. **Only touch the current slot; leave the other slot unchanged.**

```powershell
adb shell getprop ro.boot.slot_suffix                       # For example, _a
adb shell "su -c 'dd if=/dev/block/by-name/boot_a of=/sdcard/Download/boot_a-backup.img bs=4M'"
adb pull /sdcard/Download/boot_a-backup.img
(Get-Item boot_a-backup.img).Length                          # Should be 100663296
```

- **Without root**: You cannot back up the current boot. The restoration source is then the stock `stock-boot-fw1.1.0.img` extracted from the firmware in 7.2. It applies only when the device is running **FW 1.1.0**, and the image itself contains no root patches.
- **If the original boot was patched with APatch**: The backup retains the patch and can be used for restoration; however, flashing the new kernel will remove root.

### 7.4 Temporary Boot (Without Writing Any Partition)

```powershell
adb reboot bootloader
fastboot getvar current-slot             # Must match the slot in 7.3
fastboot boot m21-boot.img               # Loads into RAM only; rebooting returns to the original kernel
```

`adb wait-for-device` only means that adb has connected, not that Android has finished booting; it also waits indefinitely if boot fails. Use the following polling loop instead: check every 5 seconds, waiting at most 180 seconds.

First set `$expect` to this build's full version string: the output of `make -s kernelrelease` in WSL, or the `release:` printed by `scripts/pack.sh`.

```powershell
$expect = '5.15.220-m21-android13-8-g3540cf724bf0-dirty'   # Replace with this build's version string
$deadline = (Get-Date).AddSeconds(180); $result = 'TIMEOUT'
while ((Get-Date) -lt $deadline) {
  if ((fastboot devices) -match 'fastboot') { $result = 'BACK_IN_FASTBOOT'; break }
  if ((adb get-state 2>$null) -eq 'device') {
    $rel = ((adb shell uname -r 2>$null) -join '').Trim()
    if (-not $rel) { Start-Sleep 5; continue }                        # Cannot read version: don't decide yet; keep waiting
    if ($rel -ne $expect) { $result = "FELL_BACK ($rel)"; break }       # Any version that is not an exact match counts as failure
    if (((adb shell getprop sys.boot_completed 2>$null) -join '').Trim() -eq '1') { $result = "OK ($rel)"; break }
  }
  Start-Sleep 5
}
$result
```

The result is `OK` only if the version string **exactly equals** `$expect` and `sys.boot_completed` is `1`. If the version cannot be read, the loop keeps waiting rather than treating it as success; an older kernel containing `-m21-` but with a different version is also classified as `FELL_BACK`.

| Result | Meaning | Next step |
|---|---|---|
| `OK (<$expect>)` | The version exactly equals `$expect`, and boot is complete. About 30 seconds in testing | Check that `/proc/asound/cards` contains `bengal-idp-snd-card` and `/proc/modules` has about 310 lines; then actually confirm that touch, headphone output, Wi‑Fi, and Bluetooth all work before proceeding to 7.5 |
| `FELL_BACK (<version>)` | This build's kernel is not running: usually the new kernel failed and the device reset back to the original kernel (observed after about 60–75 seconds during debugging) | Do not flash. Temporary boot wrote nothing; the device is already in the original system, so no restoration is needed |
| `BACK_IN_FASTBOOT` | Boot failed and returned to the bootloader | Do not flash. Run `fastboot reboot` to return to the original system |
| `TIMEOUT` | adb did not come online or boot did not complete within 180 seconds; the screen usually remains on the boot logo or goes black | Hold the power button for about 20 seconds to force a reboot (confirmed on this device to return to the original system); do not flash |

None of these checks requires root, so losing root after temporary boot is not a problem.

### 7.5 Flashing (After Confirming That 7.4 Works Normally)

```powershell
adb reboot bootloader
fastboot getvar current-slot             # Must match the slot backed up in 7.3; stop if different (see below)
fastboot flash boot_a m21-boot.img       # Flash only the slot backed up in 7.3
fastboot reboot
```

**If the slot differs from 7.3, stop immediately and do not flash.** After repeated boot failures on one slot, the bootloader may automatically switch to the other; during this project's debugging, the device did in fact change from `_b` to `_a`. You have not backed up the other slot, and its firmware version may differ—for example, an OTA may have updated only one slot. In that case:
1. Run `fastboot reboot` to boot the system on the current slot;
2. Use `getprop ro.product.version` to confirm that this slot's firmware is FW 1.1.0. If not, stop, because this document has only verified 1.1.0;
3. Start again from 7.3: back up this slot, repeat the temporary boot, and only then flash.

### 7.6 Restoration and Recovery

**Principle: Restore “the slot you flashed,” using “that slot's backup,” then switch the boot slot back to it.** The slot you flashed is not necessarily the current slot: after repeated boot failures following flashing, the bootloader may already have switched to the other slot automatically.

The following example assumes “backed up `_a` in 7.3 and flashed `boot_a` in 7.5.” If you flashed `_b`, swap `a` and `b`.

1. **Enter fastboot**
   - If Android still boots: run `adb reboot bootloader`.
   - If Android will not boot: hold the power button to force shutdown, then use the button combination to enter fastboot (see the [official FiiO M21 FAQ](https://fiio.com/newsinfo/1045496.html)). In PowerShell, use `fastboot devices` to confirm the connection.
2. **Check slot status**

   ```powershell
   fastboot getvar current-slot
   fastboot getvar slot-unbootable:a        # Some bootloaders do not support this; errors may be ignored
   fastboot getvar slot-retry-count:a
   ```

3. **Write back to the slot you flashed**: Explicitly specify `boot_a`, regardless of what `current-slot` shows:

   ```powershell
   fastboot flash boot_a boot_a-backup.img        # Without a backup, use stock-boot-fw1.1.0.img (FW 1.1.0 only)
   ```

4. **Ensure that the device boots from the original slot**
   - If `current-slot` is still `a`: simply run `fastboot reboot`.
   - If `current-slot` has changed to `b` (the bootloader switched automatically): run `fastboot set_active a`, then `fastboot reboot`.
     - Why switch back: The system on `_b` may not match the version of userdata—for example, it may be the older version from before an OTA—and booting it may cause problems.
     - `set_active` also clears the unbootable flag on `_a`.
5. **Confirm**: After boot, `adb shell getprop ro.boot.slot_suffix` should return `_a`, and `adb shell uname -r` should show the original version.

**Do not do the following**: Do not flash the other slot without a backup; do not modify vbmeta or vendor partitions.

**If even fastboot is inaccessible**: The only options are to re-upgrade using official firmware or recover using EDL (QFIL). These are high-risk operations and are outside the scope of this document.

**Note**: “The charging is disabled currently” is not a kernel problem; it is FiiO's battery protection setting: `persist.sys.charging.disable=1`, `disable_batt_charging_level=90`. The stock kernel likewise shows “not charging” (`status: 4`) when the battery level exceeds 90%.

---

## 8. Debugging: From ACK 5.15.220 to a Successful Boot

### 8.1 Starting Point and Symptoms

The kernel built according to Section 5 (with `I2C_GPIO` **not yet** enabled at the time) passed all offline verification: 488 module files, 25,523 CRC references, 0 errors. But when temporarily booted using `fastboot boot`:
- It remained on the boot screen and **automatically returned to the stock kernel** within about 1 minute (the baseline test took 59 seconds from temporary boot to stock `boot_completed`);
- No kernel logs could be obtained, and pstore was also empty.

The entire debugging process involved 58 rounds of temporary boots, with no flashing or erasing at all.

### 8.2 Methods

| Method | Purpose | Calibration or limitations |
|---|---|---|
| **4-byte halt point**: Replace a kernel function's first instruction (PACIASP) with `B .` | If it halts (stays at the logo), execution reached this point; if it does not halt and returns to stock by itself, the reset occurred earlier or execution followed a different path | Requires comparison with a “baseline image differing by only these 4 bytes”; the SMC→NOP difference at entry confirmed that this method works |
| **PON reason** (read stock dmesg with root) | Distinguish reset sources | Failures show `PS_HOLD` plus a cold boot, just like a normal `adb reboot`; a long press of the power button shows `KPDPWR` or `S3` |
| **`module_blacklist=`** (cmdline) | Bisect modules | First-stage loading is strictly sequential, so only order-based bisection is possible; second-stage modules can be blocked in groups |
| **APatch root + wireless adb** | Read logs and manually `insmod` even when the candidate kernel has no USB | Requires the user to enable wireless debugging each round |
| **kprobe events** (`/sys/kernel/tracing`) | Record function call order | Data from the last few tens of milliseconds before reset is lost during wireless transmission |
| **Patch module binaries** (put `B .` inside `.ko` functions) | Bisect inside modules without source code | Only applicable to manually loaded modules |
| **Custom debugging modules** (`m21probe`, `m21peek`) | Log step by step and add delays; safely read private module data | Compile using the candidate kernel's build tree |

### 8.3 Key Rounds

| Stage | Finding |
|---|---|
| Calibrating observation tools | Attempts to log using ramoops or pstore consistently failed. SMC shutdown did not work as a marker (it was converted into a reboot); a “halt at entry” marker did work. Halt points confirmed that the kernel reached `start_kernel`, the banner `printk`, and the return from `kernel_init_freeable` |
| Baseline and reset source | The unpatched baseline returned to stock by itself after 59 seconds; halting before executing `/init` kept it halted indefinitely, placing the failure after userspace starts. Halts at `kernel_restart`, `panic`, and `machine_restart` did not intercept it (positive controls had confirmed that the halt points worked), so the reset bypassed the kernel's restart path. PON was `PS_HOLD`; because a normal reboot is itself a cold reset, pstore could not retain logs |
| First-stage bisection | Separately blocking the watchdog (`qcom_wdt_core`), the last first-stage module, or the first vendor module all caused a halt with no reset. This shows that first-stage modules (including `qseecom`) **all finished loading and did not trigger a reset during loading**; the trigger was in the second stage. This only means “the loading stage passed,” not that these modules behave correctly at runtime in every respect: for example, Section 8.4 hypothesizes that the die notifier in `qcom_wdt_core` ultimately converted an oops into a reset |
| vendor_dlkm bisection | 223 → 112 → 56 → 28 → 14 → 7 → 3 → 1: **`dac_core_module.ko`**. Blocking only this module allowed the candidate kernel to reach the home screen, but USB, charging, and headphones did not work, and boot remained on the splash screen for about 5 minutes |
| Wireless logs | USB controller `4e00000.ssusb` was not bound, with `waiting_for_supplier=1`. Its extcon pointed to `i2c_gpio/fusb30x@22`, but the candidate kernel lacked the i2c-gpio driver. Neither `fw_devlink=permissive` nor `clk_ignore_unused` helped |
| Step-by-step tracing with root | Manually running `insmod dac_core` from the home screen reproduced the reset. kprobe tracing showed that as soon as the dac component registered, sound card `bengal-idp-snd-card` began binding. Blocking binding with `driver_override` before loading prevented the reset; allowing binding immediately triggered it |
| Binding-flow bisection | Kernel halt points (`snd_card_register`, `snd_soc_link_init`, `snd_soc_card_jack_new`) narrowed the problem to the component probe stage; patching `dac_core.ko` further narrowed it to the instruction immediately before `snd_dac_probe` calls `wakeup_source_register()` |
| Root cause | Static analysis showed that `snd_dac_probe` dereferences `priv→[+8]→[+8]`, while `priv+8` is written only in `snd_dac_load_work`. On-device measurements with `m21peek` found `priv+8 = NULL` on the candidate kernel. Meanwhile, `soc:power_board` was also stuck at `waiting_for_supplier=1`, and its extcon was likewise `fusb30x@22`. On the stock system, `soc:i2c_gpio` bound to the **built-in** `i2c-gpio` driver. After enabling `CONFIG_I2C_GPIO=y`, the device booted normally in 29 seconds and all functions recovered |

### 8.4 Root-Cause Chain

Each step is labeled with its evidence level: **[Measured]** directly observed on the device; **[Static]** derived from disassembly or source analysis; **[Inference]** deduced from preceding and subsequent results, but not directly observed.

```text
[Measured] The candidate kernel lacks the i2c-gpio driver; stock soc:i2c_gpio binds to built-in i2c-gpio, but the stock config does not reflect this
 └─ [Measured] soc/i2c_gpio/fusb30x@22 (fusb302, extcon provider) is absent: no 0-0022 device and no extcon1
     ├─ [Measured] 4e00000.ssusb is stuck at waiting_for_supplier=1 ──── USB/adb/charging fail
     └─ [Measured] soc:power_board (FiiO power board) is stuck at waiting_for_supplier=1
         └─ [Static + tracing] check_delay_work in fiio_underlay polls 7 FiiO devices about every 20 ms;
            it proceeds only when all are present, and power_board is one of them
             └─ [Inference] fiio_load does not run → snd_dac_load_work in dac_core does not run
                 └─ [Measured] priv+8 in the dac private structure remains NULL
                    [Static] priv+8 is written only in snd_dac_load_work; it should hold data for "dac_dai module" (inference)
                     └─ [Measured + static] When the sound card binds, snd_dac_probe dereferences priv+8:
                        halting before the dereference leaves the device alive; halting after it results in a reset
                         └─ [Inference] kernel oops → [Measured] PS_HOLD cold reset → return to the original kernel
[Measured] After enabling CONFIG_I2C_GPIO=y: 0-0022 and extcon1 appear, both ssusb and power_board finish binding, and boot and audio work normally
```

**The 7 FiiO devices**: `check_delay_work` queries the following names in order using `fiio_device_get_data_by_name`: `dac_dai module`, `dac module`, `fpga module`, `fpga_device module`, `fuel_gauge module`, `fiio-charger-manager module`, `power_board module`. This list comes from strings obtained through disassembly, cross-checked against call addresses recorded with kprobe.

**Unconfirmed Parts**
- **Why an oops causes a reset without passing through `panic`**: The hypothesis is that the die notifier registered by `qcom_wdt_core` (which imports both `register_die_notifier` and `register_restart_handler`) directly triggers a hardware reset when the oops occurs. This does not conflict with the first-stage bisection results: the watchdog module itself loads normally; the hypothesis concerns its role in converting the error into a reset **after** the oops occurs. This is expected behavior on Qualcomm platforms, not a malfunction.
- **Why the stock config does not show i2c-gpio**: FiiO may have compiled it into the kernel through another Kconfig option, or used its own version.
- **What ensures that `snd_dac_load_work` runs before sound-card binding on stock**: Not tested; all that is known is that the order is correct on stock.

### 8.5 Lessons Learned

1. **Passing all offline ABI checks does not mean the kernel can boot**. The stock kernel is “ACK plus vendor modifications,” and its config may not reflect its actual contents. When root is available, directly comparing `/sys/bus/*/drivers` and `waiting_for_supplier` on the stock system can be the fastest way to find missing built-in drivers.
2. **Calibrate observation tools first**. pstore does not survive a cold reset, and SMC shutdown is converted into a reboot. What actually works is a “differential image differing by only a few bytes” plus an observable result (a halt or an automatic reset).
3. **When a reset bypasses the kernel restart path, suspect a vendor die or panic notifier**.
4. **Modules without source code can still be bisected**: Patch manually loaded `.ko` files, create custom kprobe debugging modules, and use disassembly to find where pointers come from.
5. **Two seemingly unrelated symptoms may share a root cause**: In this case, USB failure and resets during boot had the same root cause.

---

## 9. Notes on Online Community Findings

The following is compiled from community sources, and **not all of it has been tested on this device**. Items confirmed on this device are explicitly marked.

### 9.1 Device State

- **Bootloader unlocked out of the box**: A [JM21 hands-on article](https://note.com/adotann/n/n50153999ce31) states that “it arrived with the bootloader already unlocked”; an M21 [Reddit record](https://www.reddit.com/r/Magisk/comments/1lr33vn/help_rooting_digital_audio_player_fiio_m21/) also shows `DEVICE STATE = unlocked` and `SECURE BOOT = no`.
  - Confirmed on this device: `ro.boot.flash.locked=0`, `verifiedbootstate=orange`.
  - This only describes this particular device; it cannot be generalized to all M21 units.
- **SELinux permissive**: Mentioned in the same JM21 article. This device was also confirmed to be Permissive.
- **ABL uses a QCT test key**: A reverse-engineering conclusion from [iamcheyan/m21-baseos-assessment](https://github.com/iamcheyan/m21-baseos-assessment), not verified on this device. Although the boot footer's fingerprint says `dev-keys`, that refers to the Android system signing key and does not prove that the bootloader uses a test key.
- **When replacing only the boot partition, vbmeta/AVB does not need to be changed** (confirmed on this device: actually flashed and booted). Modifying a hashtree-protected partition such as vendor_dlkm may still trigger dm-verity and requires separate handling.

### 9.2 Community Rooting and Flashing Approaches

1. **Magisk patching init_boot (mainstream)**: Extract `init_boot.img` from payload.bin, patch it with Magisk, then flash it; to remove root, flash back the stock init_boot ([Head-Fi](https://www.head-fi.org/threads/fiio-m21-qualcomm-snapdragon-680-fiio-android-portable-music-player-is-available.975977/page-114#post-18815827)). A patched init_boot is not a kernel image that can boot on its own and cannot be used directly with `fastboot boot`.
2. **APatch / KernelSU patching boot**: boot.img contains only the kernel, which can be patched or replaced directly. The custom kernel in this document can likewise be patched in APatch before flashing.
   - Community prebuilt GKI, KernelSU, and [MakoSU](https://spring-bulid.github.io/MakoSU/guide/) images are **not guaranteed** to work directly on the M21: this document found that the stock kernel has a built-in `i2c-gpio` driver and VFS exports that differ from ACK. Without `i2c-gpio`, the boot resets and USB failure documented here occur.
3. **fastbootd / EDL**: [4pda](https://4pda.to/forum/lofiversion/index.php?t1105749-1860.html) uses `fastboot reboot fastboot` to enter fastbootd and flash dynamic partitions; Head-Fi uses QFIL (EDL) for full restoration. These are all high-risk recovery methods.

### 9.3 Kernel-Related Notes

- **GKI lifecycle**: The source information has not been independently verified. GKI releases for android13-5.15 continue through 2026, and EOL for 5.15 LTS itself is 2026-12-31. What could be confirmed here is only that the `android13-5.15` branch still had a commit on 2026-11-13, at version 5.15.220.
- **FiiO has not published the M21 kernel source**. However, the stock modules are all labeled GPL v2 or GPL (`modinfo` can verify this); under the GPL, you can request source code from FiiO.

### 9.4 Community Resources

| Resource | Purpose |
|---|---|
| [4pda M21 thread](https://4pda.to/forum/index.php?showtopic=1105749) | Root, fastbootd, partition table, firmware collection |
| [Head-Fi M21 thread](https://www.head-fi.org/threads/fiio-m21-qualcomm-snapdragon-680-fiio-android-portable-music-player-is-available.975977) | Magisk init_boot, QFIL, WheresWaldo's tuning tools |
| [note.com JM21 optimization article](https://note.com/adotann/n/n50153999ce31) | Factory-unlocked bootloader, Magisk, KernelSU, swap/zram optimization |
| [Reddit M21 rooting discussion](https://www.reddit.com/r/Magisk/comments/1lr33vn/help_rooting_digital_audio_player_fiio_m21/) | Individual bootloader-state report |
| [WheresWaldo/FiiOat](https://github.com/WheresWaldo/FiiOat) | Magisk module for adjusting kernel parameters on M21/JM21 |
| [iamcheyan/m21-baseos-assessment](https://github.com/iamcheyan/m21-baseos-assessment) | M21 reverse engineering (DTB, modules, boot chain) |
| [KernelSU](https://kernelsu.org) / [MakoSU](https://spring-bulid.github.io/MakoSU/guide/) | GKI kernel-mode root |
| [TQMatvey/linux-sm6225](https://gitlab.com/TQMatvey/linux-sm6225) | Experimental SM6225 mainline (no display or audio; for reference only) |
| [FiiO M21 firmware page](https://forum.fiio.com/note/showNoteContent.do?id=202505140958060216556&tid=17) | Official firmware downloads |
| [AOSP kernel/common](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.15) | ACK source used in this document |
