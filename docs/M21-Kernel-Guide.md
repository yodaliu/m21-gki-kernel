# FiiO M21 自編 GKI Kernel：建置、驗證與除錯全紀錄

> 作者：Claude Opus 5.5
> 日期：2026-11-23
> 適用：FiiO M21（Snapdragon 680 / SM6225），韌體 FW 1.1.0（Android 13）

本文件說明如何從公開的 Android Common Kernel（ACK）源碼，建置一個能在 FiiO M21 上正常開機、並和原廠全部驅動模組相容的 kernel。後半部完整記錄這個 kernel 從「無法開機」到「正常開進 Android」的除錯過程、使用的方法和每一步的依據。

**目前狀態**：依本文建置的 kernel 已用 `fastboot boot` 實際開機成功，結果如下：
- 29 秒內 `boot_completed=1`；
- 桌面、觸控、耳機輸出、音量控制、USB adb 都正常；
- 實際載入的模組數量和原廠相同，都是 310 個（只比對了數量，見 2.3 節）。

最終版已**寫入 slot `_a` 的 boot 分區**，寫入前已先備份原本的 boot 分區。寫入後再確認：Wi‑Fi 連線、藍牙配對與連線也都正常。

**尚未驗證**：休眠喚醒、長時間播放。

---

## 0. 配套檔案與操作環境

本文搭配 repo **`m21-gki-kernel`** 使用：<https://github.com/yodaliu/m21-gki-kernel>。repo 裡有：

| 檔案 | 用途 |
|---|---|
| `patches/` | 第 5 節的兩個源碼補丁 |
| `config/m21.config` | 第 5 節產生的最終 config |
| `kmi/vmlinux.symvers` | KMI 參考檔，比對 CRC 是否改變 |
| `scripts/build.sh`、`scripts/pack.sh` | 自動完成第 5–7 節的建置與打包 |
| `scripts/prepare-stock-modules.sh`、`scripts/verify-all-modules.py` | 第 6 節的原廠模組離線驗證 |

本文的指令分兩個環境執行：

| 環境 | 做什麼 | 文中標示 |
|---|---|---|
| **WSL2（CentOS Stream 10）** | 下載韌體、建置 kernel、驗證、打包 | ```` ```bash ```` 區塊 |
| **Windows PowerShell**，安裝 [Android platform-tools](https://developer.android.com/tools/releases/platform-tools)（adb、fastboot） | 透過 USB 操作裝置：備份、暫啟、寫入 | ```` ```powershell ```` 區塊 |

WSL2 預設看不到 USB 裝置，所以 adb 和 fastboot 一律在 Windows 執行。WSL 產生的檔案要先複製到 Windows 目錄（見 7.2 節）。

第 5–7 節的手動指令都完整寫在文中，就算沒有 repo 也能建置、打包和寫入。repo 只是把這些步驟自動化，另外提供第 6 節的模組驗證腳本和 KMI 參考檔。

---

## 1. 結論摘要

| 項目 | 內容 |
|---|---|
| kernel 源碼 | ACK `android13-5.15`，commit `3540cf724bf0fe246a3131fd1f3e9342fa03b03e`（Linux 5.15.220，KMI generation 8） |
| 編譯器 | AOSP prebuilt clang r450784e（clang 14.0.7），與原廠相同 |
| config | 以原廠 `/proc/config.gz` 為基底，關閉 `MODULE_SIG`、`TRIM_UNUSED_KSYMS`，**開啟 `CONFIG_I2C_GPIO=y`** |
| 源碼補丁 | ① `extract-cert.c`：OpenSSL 3 相容 ② `kernel_read`、`kernel_write`、`filp_open` 改回不帶命名空間的導出（與原廠一致） |
| 關鍵發現 | 原廠 kernel 內建 `i2c-gpio` 驅動，但原廠 `/proc/config.gz` 卻顯示 `# CONFIG_I2C_GPIO is not set`。少了這個驅動，就會同時造成 USB／充電失效和開機重設，詳見第 8 節 |
| 最終成品 | `5.15.220-m21-android13-8-g3540cf724bf0-dirty`，完全依本文步驟建置（不含任何除錯補丁）。Image SHA-256 `07d39903…486beb50`，boot.img SHA-256 `8830a1b8…754168d5`。2026-11-23 已先用 `fastboot boot` 驗證，再寫入 `boot_a`，重開機 35 秒進入桌面 |

---

## 2. 裝置與原廠事實

| 項目 | 內容 |
|---|---|
| 原廠 kernel | `5.15.41-android13-8-gf7d0984aa5e2-dirty`。基底是 ACK，但 FiiO 有修改：commit `f7d0984aa5e2` 不在 AOSP 公開源碼中 |
| boot.img | header v4，只有 kernel（ramdisk 大小 0），cmdline 空白，`os_version 13.0.0`、`os_patch_level 2023-02`，結尾有 AVB hash footer（分區 100,663,296 bytes，algorithm NONE） |
| 模組分布 | vendor_boot ramdisk 259 個（first-stage 載入清單 87 行）、vendor_dlkm 227 個、system_dlkm 2 個（zram、zsmalloc） |
| 模組簽章 | vendor 的 486 個 `.ko` 都沒有簽章 |
| 分區 | virtual A/B。bootloader 已解鎖（orange），`veritymode=enforcing`，SELinux permissive（測試機的狀態，哪些是必要條件見下表） |

**前置條件：哪些必須符合、哪些只是測試機的狀態**

| 項目 | 必須符合？ | 怎麼檢查 | 不符合時 |
|---|---|---|---|
| 韌體 FW 1.1.0 | **必須** | `adb shell getprop ro.product.version` | **停止**。原廠模組的 ABI 只對 1.1.0 驗證過；其他版本要先用該版韌體重做第 4、6 節的驗證 |
| bootloader 已解鎖 | **必須** | `fastboot getvar unlocked`，或 `adb shell getprop ro.boot.flash.locked`（0 表示已解鎖） | **停止**。上鎖的 bootloader 不接受未簽章的 boot 映像。解鎖會清除資料，也不在本文範圍內 |
| A/B 分區 | **必須** | `adb shell getprop ro.boot.slot_suffix` 有值 | 停止，第 7 節的流程是以 A/B 為前提寫的 |
| 備份用的 root | 建議 | 見 7.3 | 沒有 root 時，改用韌體裡的原廠 boot 作為還原依據（7.3） |
| SELinux permissive | 不是條件 | `adb shell getenforce` | 只是測試機的狀態。本 kernel 不會改變 SELinux 模式；在 enforcing 的裝置上沒有測試過 |
| `veritymode=enforcing` | 不是條件 | — | 只寫入 boot 時沒有影響；不要修改 vbmeta 或 vendor 分區 |

### 2.1 判定原廠為 GKI 建置的依據

- **版本字串**：`5.15.41-android13-8-…` 中的 `android13-8`，就是 ACK 的分支 `android13-5.15` 和 KMI generation 8。
- **建置路徑**：原廠 config 的 `CONFIG_UNUSED_KSYMS_WHITELIST` 洩漏了 FiiO 的建置路徑 `/mnt/SSD4T/work/sm6225_4/target/kernel_platform/out/msm-kernel-bengal-gki/gki_kernel/common/abi_symbollist.raw`，指向 Qualcomm kernel_platform 裡的 `common/`，也就是 ACK。
- **平台驅動不在 kernel 裡**：原廠 config 明確關閉 QCOM 平台驅動，例如 `# CONFIG_COMMON_CLK_QCOM is not set`，`QCOM_SMEM`、`PINCTRL_MSM`、`ARM_SMMU`、`SLIMBUS`、`QRTR` 也都沒開；原廠 Image 中也找不到 `ipc_log_string`、`clk_rcg2_ops`、`qcom_smem_get`、`walt_pause_cpus` 等字串。這些符號都由 vendor_boot 的模組導出，例如 `clk-qcom.ko`、`qcom-scm.ko`、`qcom_ipc_logging.ko`、`smem.ko`、`sched-walt.ko`。
- **實證**：用 ACK 加原廠 config 建置的 kernel，對全部 488 個原廠模組檔的 25,523 筆符號引用做比對，CRC 不符 0、未解析 0、重複導出 0。
- **但 FiiO 改過 kernel 本體**：commit `f7d0984aa5e2` 不在 AOSP 公開源碼中；實際找到的差異有內建的 `i2c-gpio` 驅動，以及 VFS 導出命名空間（見下方）。

### 2.2 原廠模組的載入流程

1. **first-stage init**：依 vendor_boot ramdisk 的 `lib/modules/modules.load`（87 行，去重後 84 個）**嚴格依序**載入平台基礎驅動，例如時脈、pinctrl、SCM、SMMU、watchdog、qseecom。任何一個載入失敗，init 就會中止開機並要求重開機。
2. **第二階段**：`init.qti.kernel.rc` 在 early-init 啟動 `/vendor/bin/vendor_modprobe.sh`，載入 vendor_dlkm 的 `modules.load`（260 行，去重後 227 個）。
   - 先依 blocklist 過濾。
   - 清單的**第一個模組必須成功**，否則腳本改試 `/vendor/lib/modules`，最後以 exit 1 結束。
   - 其餘模組在背景**平行載入**，個別失敗只會略過。
   - 全部跑完後設定 `vendor.all.modules.ready=1`。`init.qti.kernel.rc` 在 post-fs 階段會 `wait_for_prop` 等這個屬性。
3. **system_dlkm**：`/vendor/bin/system_dlkm_modprobe.sh` 用 `/system_dlkm/lib/modules/*/` 萬用字元找目錄，載入 zram 和 zsmalloc，所以不受 `uname -r` 改變影響。
4. **vermagic**：模組帶有 CRC（modversions）時，`same_magic()` 會略過版本字串，只比對 `SMP preempt mod_unload modversions aarch64` 這些旗標。因此 5.15.41 的模組可以直接載入 5.15.220 的 kernel。

### 2.3 「相容」的範圍：數字怎麼對應

| 數字 | 指什麼 | 怎麼驗證 |
|---|---|---|
| **488** | 三個分區的 `.ko` **檔案**數：vendor_boot 259 + vendor_dlkm 227 + system_dlkm 2 | **靜態**：用 `verify-all-modules.py` 檢查全部 25,523 筆符號引用，CRC 不符、未解析、重複導出、命名空間都是 0 |
| **311** | 不同的模組**名稱**數。有 177 個名稱同時出現在 vendor_boot 和 vendor_dlkm，而且是不同的檔案 | 同上（靜態） |
| **486** | vendor 的 `.ko` 檔案數（259 + 227），全部**沒有簽章** | 靜態：檢查檔尾的簽章標記 |
| **310** | 開機後 `/proc/modules` 裡**實際載入**的模組數，原廠和本 kernel 都是 310 | **實機**：只比對了數量，沒有逐一比對名稱 |

所以「全部模組相容」的意思是：488 個檔案都通過了靜態 ABI 檢查，而且實機開機後載入的模組數量和原廠相同，主要功能也都正常。這**不代表**每個模組在每一種硬體狀態下的行為都和原廠完全一樣。

**與原廠 kernel 不一致、必須處理的三點：**

1. **i2c-gpio 驅動**：原廠內建（`soc:i2c_gpio` 綁定 `i2c-gpio`，底下的 `0-0022` fusb302 提供 `extcon1`），config 卻沒有顯示。
2. **VFS 導出命名空間**：原廠的 `kernel_read`、`kernel_write`、`filp_open` 沒有命名空間；ACK 則放在 `VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver` 下。原廠的 `hynitron_touch.ko` 會用到這些符號，卻沒有宣告匯入，不修正就無法載入。
3. **模組簽章／PROTECT**：原廠開著 `MODULE_SIG=y` 和 `MODULE_SIG_PROTECT=y`。我們的 kernel 沒有 FiiO 的金鑰，所以直接關閉。

---

## 3. 環境（WSL2 CentOS Stream 10）

```bash
sudo dnf install -y git make bc bison flex perl python3 curl unzip file \
  openssl openssl-devel elfutils-libelf-devel ncurses-devel dwarves cpio lz4 kmod
mkdir -p ~/toolchains/clang-r450784e && cd ~/toolchains
curl -fL -o clang.tar.gz 'https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/5538393cf30cb39da6d53730a9b85f36958230da/clang-r450784e.tar.gz'
tar xzf clang.tar.gz -C clang-r450784e && rm clang.tar.gz     # tarball 沒有頂層目錄
export PATH=$HOME/toolchains/clang-r450784e/bin:$PATH
clang --version   # Android (8508608, based on r450784e) clang version 14.0.7
```

- `cpio`：原廠 config 開了 `CONFIG_IKHEADERS=y`，建置時需要。
- `dwarves`（pahole）：原廠 config 開了 `CONFIG_DEBUG_INFO_BTF=y`，建置時需要。
- 不用發行版自帶的 clang：它缺 ARM32 backend，編不了 compat vDSO。

**打包工具**：用 AOSP 的 mkbootimg 和 avbtool 打包 boot.img。

```bash
mkdir -p ~/tools
git clone --depth 1 https://android.googlesource.com/platform/system/tools/mkbootimg ~/tools/mkbootimg
curl -fsSL 'https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py?format=TEXT' \
  | base64 -d > ~/tools/avbtool.py
python3 ~/tools/avbtool.py version     # avbtool 1.3.0
```

**本文的 repo**：

```bash
git clone https://github.com/yodaliu/m21-gki-kernel ~/m21-gki-kernel
cd ~/m21-gki-kernel                    # 第 6、7 節的 scripts/ 都在這裡執行
```

---

## 4. 取得原廠素材

```bash
mkdir -p ~/m21-fw && cd ~/m21-fw
curl -fL -C - -o M21-update-1.1.0.zip 'https://m21pack.oss-cn-beijing.aliyuncs.com/1.0.9/M21-update-1.1.0.zip'
unzip -o M21-update-1.1.0.zip payload.bin
curl -fL -o pdg.tar.gz https://github.com/ssut/payload-dumper-go/releases/download/1.2.2/payload-dumper-go_1.2.2_linux_amd64.tar.gz
tar xzf pdg.tar.gz
./payload-dumper-go -partitions boot,vendor_boot,vendor_dlkm,system_dlkm,init_boot -o extracted payload.bin
```

**原廠 config**：kernel 把 config 用 gzip 壓縮後，夾在 `IKCFG_ST` 和 `IKCFG_ED` 兩個標記之間，嵌在 boot.img 裡。把它解出來存成 `~/m21-fw/m21-kernel-config.txt`，第 5 節會用到：

```bash
cd ~/m21-fw
python3 - <<'EOF'
import gzip
d = open('extracted/boot.img', 'rb').read()
i, j = d.find(b'IKCFG_ST'), d.find(b'IKCFG_ED')
open('m21-kernel-config.txt', 'wb').write(gzip.decompress(d[i + 8:j]))
EOF
md5sum m21-kernel-config.txt      # 089add4afe3bbc46f3cc7712d591d8a8，187,988 bytes
head -3 m21-kernel-config.txt     # 應出現 "Linux/arm64 5.15.41 Kernel Configuration"
```

另外兩種取得方式，結果完全相同：
- 第 5 節取得 kernel 源碼後，執行 `~/kernel-common/scripts/extract-ikconfig extracted/boot.img > m21-kernel-config.txt`；
- 從正在執行原廠 kernel 的裝置讀取：在 PowerShell 執行 `adb pull /proc/config.gz`，複製到 WSL 後用 `gunzip -c config.gz > m21-kernel-config.txt` 解壓。不要在 PowerShell 用 `>` 轉存，它會改變編碼和換行。

---

## 5. 源碼、補丁與 config

> 捷徑：本專案的 `patches/`、`config/m21.config` 和 `scripts/build.sh` 會自動完成本節與第 6 節的建置步驟，第 7 節的打包則由 `scripts/pack.sh` 處理。下面列出的是手動步驟，方便了解每一步在做什麼。

```bash
mkdir -p ~/kernel-common && cd ~/kernel-common && git init -q
git fetch --depth 1 https://android.googlesource.com/kernel/common 3540cf724bf0fe246a3131fd1f3e9342fa03b03e
git checkout -q --detach FETCH_HEAD
```

**補丁 1：`scripts/extract-cert.c`**
OpenSSL 3.5 已不再宣告 ENGINE API，host 工具會編譯失敗。把 pkcs11 分支的條件改成：

```c
#if defined(OPENSSL_IS_BORINGSSL) || OPENSSL_VERSION_NUMBER >= 0x30000000L
```

**補丁 2：VFS 導出命名空間（比照原廠）**

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

**config**

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

| 設定 | 理由 |
|---|---|
| `I2C_GPIO=y` | **必要**。少了它，`fusb30x@22` 這個 extcon 裝置不會出現，fw_devlink 會一直擋住 USB 控制器和 FiiO 電源板（詳見第 8 節） |
| 關閉 `MODULE_SIG*` | CentOS 10 的 OpenSSL 3 拒絕產生 SHA1 簽章金鑰；而且原廠模組本來就沒有簽章 |
| 關閉 `TRIM_UNUSED_KSYMS` | 原廠白名單指向 FiiO 建置機上的絕對路徑；關閉後會導出全部符號 |
| 保留 KASAN 等原廠設定 | `CONFIG_KASAN` 會改變 `struct kmem_cache` 的結構，關掉會影響 CRC |

`scripts/config` 的每個 `-e`、`-d` 只能接一個選項。

---

## 6. 建置與離線驗證

```bash
m -j"$(nproc)" Image            # 16 核約 12 分鐘
m -s kernelrelease
```

把原廠全部模組準備好（解出 vendor_boot ramdisk，唯讀掛載 vendor_dlkm、system_dlkm），再做 CRC、未解析、重複導出的檢查：

```bash
scripts/prepare-stock-modules.sh             # 讀取 $FW（預設 ~/m21-fw）下的原廠映像
sudo python3 scripts/verify-all-modules.py --kernel-config ~/out-m21/.config ~/out-m21/vmlinux.symvers
# 結果：OVERALL: PASS（exit 0）；FAIL 時 exit 1；輸入有誤時 exit 2
```

這兩支腳本在 repo 的 `scripts/` 目錄（第 0 節），請在 `~/m21-gki-kernel` 執行。`prepare-stock-modules.sh` 會把 vendor_boot 的 ramdisk 解到 `/tmp/rv`，並把 vendor_dlkm、system_dlkm 唯讀掛載到 `/mnt/vdlkm`、`/mnt/sdlkm`；掛載需要 sudo。`verify-all-modules.py` 除了 CRC、未解析、重複導出，也會依開機階段檢查載入順序，並檢查命名空間匯入和 GPL 限制。

**另外要確認 KMI 沒有改變**：拿這次建置的 `vmlinux.symvers` 和參考檔比對。參考檔可以用 repo 裡的 `kmi/vmlinux.symvers`，或你上一次驗證過的建置。CRC 差異、命名空間差異、被移除的導出都必須是 0，而且 `kernel_read`、`kernel_write`、`filp_open` 不能帶命名空間。`scripts/build.sh` 會自動做這項比對；手動建置的話，執行：

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
# 預期：exports=15477 crc_diff=0 ns_diff=0 removed=0 vfs_ns=none
```

開啟 `I2C_GPIO` 前後實測都是 15,477 個導出，CRC 差異為 0。

---

## 7. 打包、備份、暫啟與寫入

**請照 7.1 → 7.6 的順序做。**備份必須在原本的系統還在、而且有 root 時先完成。暫啟新 kernel 之後是否還有 root，取決於你用的 root 方案：

| root 方案 | root 存在哪裡 | 暫啟或寫入本 kernel 後 |
|---|---|---|
| APatch、KernelSU（修補 boot 或 kernel） | boot 分區的 kernel | **root 消失**，除非先用同一個工具修補 `m21-boot.img`（本文實測過 APatch 修補後可正常使用） |
| Magisk 修補 init_boot | init_boot 的 ramdisk，本 kernel 不會碰到 | 應該**仍有 root**：本 kernel 沿用分區裡的 init_boot。這點是推論，本文沒有實測 |

不論哪種方案，備份都要在暫啟**之前**完成。

### 7.1 打包（WSL）

```bash
cd ~/m21-gki-kernel
STOCK_BOOT=~/m21-fw/extracted/boot.img scripts/pack.sh ~/out-m21/arch/arm64/boot/Image ~/m21-boot.img
```

`pack.sh` 等同以下兩條指令，並會確認 header 參數和原廠一致：

```bash
python3 ~/tools/mkbootimg/mkbootimg.py --header_version 4 --kernel ~/out-m21/arch/arm64/boot/Image \
  --os_version 13.0.0 --os_patch_level 2023-02 --output ~/m21-boot.img
python3 ~/tools/avbtool.py add_hash_footer --image ~/m21-boot.img --partition_name boot --partition_size 100663296 \
  --prop com.android.build.boot.os_version:13 --prop com.android.build.boot.security_patch:2023-02-05
```

- `os_version` 和 `os_patch_level` 要沿用原廠值，**不要調低**。
- 不需要 ramdisk：generic ramdisk 由原廠 `init_boot` 提供。

### 7.2 交給 Windows

在 WSL 把映像複製到 Windows 的工作目錄，之後的 PowerShell 指令都在這個目錄執行：

```bash
mkdir -p /mnt/c/m21 && cp ~/m21-boot.img ~/m21-fw/extracted/boot.img /mnt/c/m21/
mv /mnt/c/m21/boot.img /mnt/c/m21/stock-boot-fw1.1.0.img      # 原廠 boot，最後一道還原手段
```

```powershell
cd C:\m21
Get-FileHash m21-boot.img -Algorithm SHA256       # 和 WSL 的 sha256sum 比對
```

### 7.3 備份目前的 boot（在原本的系統上，需要 root）

先確認目前開機用的 slot。下面以 `_a` 為例；如果是 `_b`，就把所有的 `boot_a` 換成 `boot_b`。**只動目前的 slot，另一個 slot 保持原樣。**

```powershell
adb shell getprop ro.boot.slot_suffix                       # 例如 _a
adb shell "su -c 'dd if=/dev/block/by-name/boot_a of=/sdcard/Download/boot_a-backup.img bs=4M'"
adb pull /sdcard/Download/boot_a-backup.img
(Get-Item boot_a-backup.img).Length                          # 應為 100663296
```

- **如果沒有 root**：無法備份目前的 boot。這時的還原依據，是 7.2 從韌體解出的原廠 `stock-boot-fw1.1.0.img`。它只適用於裝置正在跑 **FW 1.1.0** 的情況，而且沒有任何 root 修補。
- **如果原本的 boot 是 APatch 修補過的**：備份檔保留了修補，可以用它還原；但寫入新 kernel 後 root 就會消失。

### 7.4 暫啟（不寫入任何分區）

```powershell
adb reboot bootloader
fastboot getvar current-slot             # 必須和 7.3 的 slot 一致
fastboot boot m21-boot.img               # 只載入 RAM，重開機就回到原本的 kernel
```

`adb wait-for-device` 只代表 adb 連上了，不代表 Android 已經開機完成；而且失敗時它會一直等下去。請改用下面這段輪詢：每 5 秒檢查一次，最多等 180 秒。

先把 `$expect` 設成這次建置的完整版本字串：也就是 WSL 執行 `make -s kernelrelease` 的輸出，或 `scripts/pack.sh` 印出的 `release:`。

```powershell
$expect = '5.15.220-m21-android13-8-g3540cf724bf0-dirty'   # 換成你這次建置的版本字串
$deadline = (Get-Date).AddSeconds(180); $result = 'TIMEOUT'
while ((Get-Date) -lt $deadline) {
  if ((fastboot devices) -match 'fastboot') { $result = 'BACK_IN_FASTBOOT'; break }
  if ((adb get-state 2>$null) -eq 'device') {
    $rel = ((adb shell uname -r 2>$null) -join '').Trim()
    if (-not $rel) { Start-Sleep 5; continue }                        # 讀不到版本：不判定，繼續等
    if ($rel -ne $expect) { $result = "FELL_BACK ($rel)"; break }       # 版本不完全相同就算失敗
    if (((adb shell getprop sys.boot_completed 2>$null) -join '').Trim() -eq '1') { $result = "OK ($rel)"; break }
  }
  Start-Sleep 5
}
$result
```

只有在版本字串**完全等於** `$expect`、而且 `sys.boot_completed` 為 `1` 時才判定 `OK`。讀不到版本時繼續等，不會被當成成功；含有 `-m21-` 但版本不同的舊 kernel，也會被判為 `FELL_BACK`。

| 結果 | 代表什麼 | 接下來 |
|---|---|---|
| `OK (<$expect>)` | 版本完全等於 `$expect`，而且開機完成。實測約 30 秒 | 檢查 `/proc/asound/cards` 有 `bengal-idp-snd-card`、`/proc/modules` 約 310 行；再實際確認觸控、耳機輸出、Wi‑Fi、藍牙，都正常才進入 7.5 |
| `FELL_BACK (<版本>)` | 跑的不是這次的 kernel：通常是新 kernel 失敗、裝置被重設後回到原本的 kernel（除錯期間約 60–75 秒出現） | 不要寫入。暫啟沒有寫入任何東西，裝置已經在原本的系統，不需要還原 |
| `BACK_IN_FASTBOOT` | 開機失敗，回到 bootloader | 不要寫入。執行 `fastboot reboot` 回到原本的系統 |
| `TIMEOUT` | 180 秒內 adb 沒上線，或沒有開機完成；畫面通常停在開機 Logo 或黑畫面 | 長按電源鍵約 20 秒強制重開（本機實測這樣就能回到原本的系統），不要寫入 |

這些檢查都不需要 root，所以暫啟後沒有 root 也沒關係。

### 7.5 寫入（確認 7.4 正常後）

```powershell
adb reboot bootloader
fastboot getvar current-slot             # 必須和 7.3 備份時的 slot 相同，不同就停止（見下方）
fastboot flash boot_a m21-boot.img       # 只寫入 7.3 已備份的那個 slot
fastboot reboot
```

**slot 和 7.3 不同時，立刻停止，不要寫入。**如果某個 slot 連續多次開機失敗，bootloader 可能自動改用另一個 slot；本專案除錯期間，裝置確實從 `_b` 變成 `_a`。另一個 slot 你沒有備份，上面的韌體版本也可能不同，例如 OTA 只更新了其中一個。這時請：
1. 執行 `fastboot reboot`，讓裝置進入目前 slot 的系統；
2. 用 `getprop ro.product.version` 確認這個 slot 的韌體是不是 FW 1.1.0。不是就停止，因為本文只驗證過 1.1.0；
3. 從 7.3 開始，對這個 slot 重新備份、重新暫啟，再寫入。

### 7.6 還原與救援

**原則：還原「你寫入過的那個 slot」，使用「那個 slot 的備份」，然後把開機 slot 切回它。**你寫入的 slot 不一定是目前的 slot：如果寫入後連續開機失敗，bootloader 可能已經自動改用另一個 slot。

以下以「7.3 備份 `_a`、7.5 寫入 `boot_a`」為例。如果你寫的是 `_b`，請把 `a` 和 `b` 互換。

1. **進入 fastboot**
   - Android 還能開機：執行 `adb reboot bootloader`。
   - Android 開不起來：長按電源鍵強制關機，再用按鍵組合進入 fastboot（參考 [FiiO M21 官方 FAQ](https://fiio.com/newsinfo/1045496.html)）。在 PowerShell 用 `fastboot devices` 確認已連上。
2. **查看 slot 狀態**

   ```powershell
   fastboot getvar current-slot
   fastboot getvar slot-unbootable:a        # 部分 bootloader 不支援，回報錯誤時可以略過
   fastboot getvar slot-retry-count:a
   ```

3. **寫回寫入過的那個 slot**：不論 `current-slot` 顯示什麼，都明確指定 `boot_a`：

   ```powershell
   fastboot flash boot_a boot_a-backup.img        # 沒有備份時，用 stock-boot-fw1.1.0.img（只適用於 FW 1.1.0）
   ```

4. **確保從原本的 slot 開機**
   - `current-slot` 仍是 `a`：直接執行 `fastboot reboot`。
   - `current-slot` 已經變成 `b`（bootloader 自動切換過）：執行 `fastboot set_active a`，再 `fastboot reboot`。
     - 為什麼要切回去：`_b` 的系統可能和 userdata 的版本不一致，例如它是 OTA 之前的舊版本，在上面開機可能出問題。
     - `set_active` 也會清除 `_a` 被標記成無法開機的狀態。
5. **確認**：開機後執行 `adb shell getprop ro.boot.slot_suffix` 應為 `_a`，`adb shell uname -r` 應為原本的版本。

**這些情況不要做**：不要寫入沒有備份的另一個 slot；不要修改 vbmeta 或 vendor 分區。

**fastboot 也進不去時**：只能用官方韌體重新升級，或用 EDL（QFIL）救援。這些都屬於高風險操作，不在本文範圍內。

**注意**：「The charging is disabled currently」不是 kernel 問題，而是 FiiO 的電池保護設定：`persist.sys.charging.disable=1`、`disable_batt_charging_level=90`。原廠 kernel 在電量 90% 以上一樣顯示「不充電」（`status: 4`）。

---

## 8. 除錯過程：從 ACK 5.15.220 到成功開機

### 8.1 起點與症狀

依第 5 節建置的 kernel（當時還**沒有**開 `I2C_GPIO`）通過了全部的離線驗證：488 個模組檔、25,523 筆 CRC 引用，0 錯誤。但用 `fastboot boot` 暫啟時：
- 停在開機畫面，約 1 分鐘內**自動回到原廠 kernel**（基準測試從暫啟到原廠 `boot_completed` 共 59 秒）；
- 拿不到任何 kernel 日誌，pstore 也是空的。

整個除錯過程共做了 58 輪暫啟，沒有任何刷寫或清除。

### 8.2 方法

| 方法 | 用途 | 校準或限制 |
|---|---|---|
| **4-byte 停點**：把某個 kernel 函式的第一條指令（PACIASP）換成 `B .` | 有停住（停在 Logo）＝執行到了這裡；沒停住、自行回原廠＝重設發生在這之前，或走的不是這條路 | 必須有「只差這 4 bytes 的基準映像」做對照；入口處的 SMC→NOP 差分證實了這個方法有效 |
| **PON 原因**（root 讀原廠 dmesg） | 區分重設來源 | 失敗時和一般 `adb reboot` 一樣是 `PS_HOLD` 加冷開機；長按電源鍵則是 `KPDPWR` 或 `S3` |
| **`module_blacklist=`**（cmdline） | 二分模組 | first-stage 是嚴格依序載入，只能用順序二分；第二階段可以分組封鎖 |
| **APatch root + 無線 adb** | 候選 kernel 沒有 USB 時，仍能讀日誌、手動 `insmod` | 每輪需要使用者開啟無線偵錯 |
| **kprobe 事件**（`/sys/kernel/tracing`） | 記錄函式呼叫順序 | 重設前最後幾十毫秒的資料會在無線傳輸中遺失 |
| **修補模組二進位**（`.ko` 函式內放 `B .`） | 在沒有原始碼的模組內二分 | 只適用於手動載入的模組 |
| **自製除錯模組**（`m21probe`、`m21peek`） | 單步記錄並加延遲；安全讀取模組的私有資料 | 用候選 kernel 的建置樹編譯 |

### 8.3 關鍵輪次

| 階段 | 發現 |
|---|---|
| 觀測工具校準 | 用 ramoops 或 pstore 記日誌一直失敗。SMC 關機當標記沒有效果（會被轉成重開機）；「停在入口」的標記則可用。停點證實 kernel 能執行到 `start_kernel`、banner `printk`、`kernel_init_freeable` 返回 |
| 基準與重設來源 | 未修補的基準 59 秒自行回原廠；在執行 `/init` 前停住則會一直停著，所以失敗發生在 userspace 開始之後。停在 `kernel_restart`、`panic`、`machine_restart` 都攔不到（已用正對照證實停點有效），重設不經過 kernel 的重啟路徑。PON 是 `PS_HOLD`，而一般重開機本身就是冷重設，所以 pstore 留不住日誌 |
| first-stage 二分 | 分別封鎖 watchdog（`qcom_wdt_core`）、first-stage 的最後一個模組、vendor 的第一個模組，結果都是停住、沒被重設。這表示 first-stage 的模組（包括 `qseecom`）都**載入完成，載入期間也沒有觸發重設**，觸發點在第二階段。這只代表「載入階段已通過」，不代表這些模組在執行期的行為全都正常：例如 8.4 節推測，最後是由 `qcom_wdt_core` 的 die notifier 把 oops 轉成重設 |
| vendor_dlkm 二分 | 223 → 112 → 56 → 28 → 14 → 7 → 3 → 1：**`dac_core_module.ko`**。只封鎖它，候選 kernel 就能開進桌面，但 USB、充電、耳機都不能用，開機時卡在 splash 約 5 分鐘 |
| 無線日誌 | USB 控制器 `4e00000.ssusb` 沒有綁定，`waiting_for_supplier=1`。它的 extcon 指向 `i2c_gpio/fusb30x@22`，但候選 kernel 沒有 i2c-gpio 驅動。`fw_devlink=permissive`、`clk_ignore_unused` 都無效 |
| 有 root 的單步追蹤 | 在桌面上手動 `insmod dac_core` 就能重現重設。kprobe 追蹤顯示，dac 元件一註冊，音效卡 `bengal-idp-snd-card` 就開始綁定。先用 `driver_override` 擋住綁定再載入不會重設，一放行綁定就立刻重設 |
| 綁定流程二分 | 用 kernel 停點（`snd_card_register`、`snd_soc_link_init`、`snd_soc_card_jack_new`）把範圍縮到元件 probe 階段；再修補 `dac_core.ko`，縮到 `snd_dac_probe` 呼叫 `wakeup_source_register()` 的前一條指令 |
| 根因 | 靜態分析顯示，`snd_dac_probe` 會解參考 `priv→[+8]→[+8]`，而 `priv+8` 只在 `snd_dac_load_work` 裡寫入。`m21peek` 實測，候選 kernel 上 `priv+8 = NULL`。同時發現 `soc:power_board` 也一直 `waiting_for_supplier=1`，而它的 extcon 同樣是 `fusb30x@22`。原廠系統上 `soc:i2c_gpio` 綁定的是**內建**的 `i2c-gpio` 驅動。開啟 `CONFIG_I2C_GPIO=y` 後，29 秒正常開機，所有功能恢復 |

### 8.4 根因鏈

每一步都標示了證據等級：**[實測]** 在裝置上直接觀察到；**[靜態]** 由反組譯或原始碼分析得出；**[推論]** 由前後結果推得，但沒有直接觀察到。

```text
[實測] 候選 kernel 沒有 i2c-gpio 驅動；原廠的 soc:i2c_gpio 綁定內建的 i2c-gpio，但原廠 config 沒有顯示
 └─ [實測] soc/i2c_gpio/fusb30x@22（fusb302，extcon 提供者）不存在：沒有 0-0022 裝置，也沒有 extcon1
     ├─ [實測] 4e00000.ssusb 停在 waiting_for_supplier=1 ──── USB／adb／充電失效
     └─ [實測] soc:power_board（FiiO 電源板）停在 waiting_for_supplier=1
         └─ [靜態＋追蹤] fiio_underlay 的 check_delay_work 每約 20 ms 輪詢 7 個 FiiO 裝置，
            要全部到齊才會繼續，power_board 就是其中之一
             └─ [推論] fiio_load 不執行 → dac_core 的 snd_dac_load_work 不執行
                 └─ [實測] dac 私有結構的 priv+8 一直是 NULL
                    [靜態] priv+8 只在 snd_dac_load_work 寫入；它應存放 "dac_dai module" 的資料（推論）
                     └─ [實測＋靜態] 音效卡綁定時，snd_dac_probe 解參考 priv+8：
                        停在解參考之前，裝置存活；停在之後，裝置被重設
                         └─ [推論] kernel oops → [實測] PS_HOLD 冷重設 → 回到原本的 kernel
[實測] 開啟 CONFIG_I2C_GPIO=y 後：0-0022 與 extcon1 出現，ssusb 與 power_board 都完成綁定，開機與音訊正常
```

**7 個 FiiO 裝置**：`check_delay_work` 依序用 `fiio_device_get_data_by_name` 查詢以下名稱：`dac_dai module`、`dac module`、`fpga module`、`fpga_device module`、`fuel_gauge module`、`fiio-charger-manager module`、`power_board module`。這份清單是反組譯取得的字串，再用 kprobe 記錄的呼叫位址交叉確認。

**尚未證實的部分**
- **oops 之後為什麼不經過 `panic` 就重設**：推測是 `qcom_wdt_core` 註冊的 die notifier（它同時匯入 `register_die_notifier` 和 `register_restart_handler`）在 oops 當下直接觸發了硬體重設。這和 first-stage 二分的結果不衝突：watchdog 模組本身載入正常；推測的是 oops 發生**之後**，它負責把錯誤轉成重設。這是 Qualcomm 平台的預期行為，不算故障。
- **原廠 config 為什麼沒有顯示 i2c-gpio**：可能是 FiiO 另外用其他 Kconfig 選項把它編進 kernel，或用了自己的版本。
- **原廠上是什麼保證 `snd_dac_load_work` 在音效卡綁定前先執行**：沒有實測，只知道原廠上順序正確。

### 8.5 經驗

1. **離線 ABI 驗證全過，不代表能開機**。原廠 kernel 是「ACK 加廠商修改」，config 也不一定反映實際內容。拿到 root 時，可以在原廠系統上直接比對 `/sys/bus/*/drivers` 和 `waiting_for_supplier`，最快找出缺少的內建驅動。
2. **觀測工具要先校準**。pstore 撐不過冷重設，SMC 關機會被轉成重開機。真正有效的是「只差幾個 bytes 的差分映像」加上可觀察的結果（停住或自行重設）。
3. **重設不經過 kernel 重啟路徑時，要懷疑廠商的 die 或 panic notifier**。
4. **無原始碼的模組也能二分**：修補手動載入的 `.ko`、自製 kprobe 除錯模組、反組譯找出指標的來源。
5. **兩個看似無關的症狀可能是同一個根因**：這次的 USB 失效和開機重設就是。

---

## 9. 網路社群發現備註

以下整理自社群資料，**並非全部都在本機實測過**。本機有確認的地方會特別標明。

### 9.1 裝置狀態

- **出廠就解鎖 bootloader**：[JM21 實測文章](https://note.com/adotann/n/n50153999ce31)提到「最初からブートローダーがアンロックされた状態で届いた」；M21 的 [Reddit 紀錄](https://www.reddit.com/r/Magisk/comments/1lr33vn/help_rooting_digital_audio_player_fiio_m21/)也顯示 `DEVICE STATE = unlocked`、`SECURE BOOT = no`。
  - 本機確認：`ro.boot.flash.locked=0`、`verifiedbootstate=orange`。
  - 這只代表這台機器，不能推論所有 M21 都是如此。
- **SELinux permissive**：同一篇 JM21 文章有提到。本機確認也是 Permissive。
- **ABL 使用 QCT test key**：出自 [iamcheyan/m21-baseos-assessment](https://github.com/iamcheyan/m21-baseos-assessment) 的逆向結論，本機沒有驗證。boot footer 的 fingerprint 雖然是 `dev-keys`，但那是 Android 系統的簽章金鑰，不能拿來證明 bootloader 用的是 test key。
- **只換 boot 分區時，不需要動 vbmeta/AVB**（本機確認：已實際寫入並開機）。如果要修改 vendor_dlkm 這類使用 hashtree 的分區，仍可能觸發 dm-verity，需要另外處理。

### 9.2 社群的 root 與刷機路線

1. **Magisk 修補 init_boot（主流）**：從 payload.bin 解出 `init_boot.img`，用 Magisk 修補後寫入；要移除 root 就寫回原廠 init_boot（[Head-Fi](https://www.head-fi.org/threads/fiio-m21-qualcomm-snapdragon-680-fiio-android-portable-music-player-is-available.975977/page-114#post-18815827)）。修補過的 init_boot 不是可以單獨啟動的 kernel 映像，不能直接拿來 `fastboot boot`。
2. **APatch／KernelSU 修補 boot**：boot.img 裡只有 kernel，可以直接修補或替換。本文的自編 kernel 也一樣，可以在 APatch 裡修補後再寫入。
   - 社群的預編譯 GKI、KernelSU、[MakoSU](https://spring-bulid.github.io/MakoSU/guide/) 映像，**不保證**能直接用在 M21：本文發現原廠 kernel 內建了 `i2c-gpio` 驅動，VFS 導出也和 ACK 不同。少了 `i2c-gpio`，就會出現本文記錄的開機重設和 USB 失效。
3. **fastbootd／EDL**：[4pda](https://4pda.to/forum/lofiversion/index.php?t1105749-1860.html) 用 `fastboot reboot fastboot` 進入 fastbootd 寫入動態分區；Head-Fi 則用 QFIL（EDL）做全量還原。這些都屬於高風險的救援手段。

### 9.3 kernel 相關

- **GKI 生命週期**：資料來源未獨立驗證。android13-5.15 的 GKI release 持續到 2026 年，5.15 LTS 本體的 EOL 是 2026-12-31。本機只能確認 `android13-5.15` 分支在 2026-11-13 仍有提交，版本 5.15.220。
- **FiiO 沒有公開 M21 的 kernel 原始碼**。但原廠模組的授權都標示為 GPL v2 或 GPL（`modinfo` 可查證），依 GPL 可以向 FiiO 索取原始碼。

### 9.4 社群資源

| 資源 | 用途 |
|---|---|
| [4pda M21 討論串](https://4pda.to/forum/index.php?showtopic=1105749) | root、fastbootd、分區表、韌體整理 |
| [Head-Fi M21 討論串](https://www.head-fi.org/threads/fiio-m21-qualcomm-snapdragon-680-fiio-android-portable-music-player-is-available.975977) | Magisk init_boot、QFIL、WheresWaldo 的調校工具 |
| [note.com JM21 優化文章](https://note.com/adotann/n/n50153999ce31) | 出廠解鎖、Magisk、KernelSU、swap／zram 優化 |
| [Reddit M21 root 討論](https://www.reddit.com/r/Magisk/comments/1lr33vn/help_rooting_digital_audio_player_fiio_m21/) | bootloader 狀態個案 |
| [WheresWaldo/FiiOat](https://github.com/WheresWaldo/FiiOat) | M21／JM21 的 Magisk kernel 參數調整模組 |
| [iamcheyan/m21-baseos-assessment](https://github.com/iamcheyan/m21-baseos-assessment) | M21 逆向分析（DTB、模組、啟動鏈） |
| [KernelSU](https://kernelsu.org) / [MakoSU](https://spring-bulid.github.io/MakoSU/guide/) | GKI kernel-mode root |
| [TQMatvey/linux-sm6225](https://gitlab.com/TQMatvey/linux-sm6225) | SM6225 實驗性 mainline（沒有顯示與音訊，僅供參考） |
| [FiiO M21 韌體頁](https://forum.fiio.com/note/showNoteContent.do?id=202505140958060216556&tid=17) | 官方韌體下載 |
| [AOSP kernel/common](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.15) | 本文使用的 ACK 源碼 |
