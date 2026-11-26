# FiiO M21 GKI kernel

A custom kernel for the FiiO M21 (Snapdragon 680 / SM6225, FW 1.1.0, Android 13), built from Google's Android Common Kernel `android13-5.15`. It loads all of the stock vendor modules unmodified.

這個專案從 Google 的 ACK `android13-5.15` 建置 FiiO M21 可用的 kernel。它可以直接替換原廠 boot 分區，原廠的全部驅動模組都能照常載入，不需要任何修改。完整的建置說明與除錯紀錄見 [docs/M21-Kernel-Guide.md](docs/M21-Kernel-Guide.md)。

## 內容

| 路徑 | 內容 |
|---|---|
| `UPSTREAM` | 上游的 URL、分支、commit、版本、KMI generation，以及 `LOCALVERSION` |
| `patches/` | 套用在上游 commit 上的補丁（`git format-patch` 格式） |
| `config/m21.config` | 完整的 kernel config：原廠 config，加上 `CONFIG_I2C_GPIO=y`，並關閉 `MODULE_SIG` 與 `TRIM_UNUSED_KSYMS` |
| `kmi/vmlinux.symvers` | KMI 參考檔：每次建置都和它比對，CRC 與命名空間都不能改變 |
| `scripts/build.sh` | 檢查環境、套用補丁、建置 `Image`，並做 KMI 比對 |
| `scripts/pack.sh` | 依原廠參數打包成 boot.img（header v4，附 AVB footer） |
| `scripts/prepare-stock-modules.sh` | 從原廠韌體取出 vendor_boot、vendor_dlkm、system_dlkm 的模組 |
| `scripts/verify-all-modules.py` | 離線驗證全部原廠模組的相容性（CRC、未解析、重複導出、命名空間、載入順序） |
| `scripts/test_verify_all_modules.py` | `verify-all-modules.py` 的單元測試（`cd scripts && python3 -m unittest test_verify_all_modules`，共 94 項） |
| `scripts/update-upstream.sh` | 把補丁搬到新的上游 tag 或 commit 上，並更新 `UPSTREAM`、`patches/` 和 config |
| `vendor/`、`scripts/{fetch,build,compare}-vendor-*` | 從 CodeLinaro 公開原始碼重建 Qualcomm 的 vendor 模組（msm-kernel 樹內 + techpack），並和原廠比對：275 個中有 234 個完全相同（見 [vendor/README.md](vendor/README.md)） |

## 需求

- Linux 或 WSL2，並安裝 `git make bc bison flex perl python3 openssl-devel elfutils-libelf-devel dwarves cpio lz4 kmod`。
- AOSP prebuilt clang **r450784e**，放在 `~/toolchains/clang-r450784e`（下載方式見指南第 3 節）。
- AOSP 的 [mkbootimg](https://android.googlesource.com/platform/system/tools/mkbootimg) 和 [avbtool.py](https://android.googlesource.com/platform/external/avb)，預設路徑為 `~/tools/`。
- 原廠 FW 1.1.0 的 `payload.bin`，以及從中解出的映像，放在 `~/m21-fw/`（見指南第 4 節）。只有驗證模組時需要。

## 建置

```bash
. ./UPSTREAM
git init -q ~/kernel-common && git -C ~/kernel-common remote add origin "$UPSTREAM_URL"
git -C ~/kernel-common fetch --depth 1 origin "$UPSTREAM_COMMIT"
git -C ~/kernel-common switch -q -c m21 FETCH_HEAD

scripts/build.sh                     # 套用補丁、建置，並和 kmi/ 比對（約 12 分鐘，16 核）
scripts/prepare-stock-modules.sh     # 選用：準備原廠模組
sudo python3 scripts/verify-all-modules.py --kernel-config ~/out-m21/.config ~/out-m21/vmlinux.symvers
scripts/pack.sh                      # 產生 dist/M21-boot-<release>.img
```

路徑可以用環境變數覆寫：`SRC`、`OUT`、`CLANG`、`DIST`、`FW`、`MKBOOTIMG_DIR`、`AVBTOOL`。

## 開機與寫入

先暫啟，確認正常後再寫入。寫入前務必用 root 備份目前的 boot 分區，詳細步驟見指南第 7 節。

```bash
adb reboot bootloader
fastboot boot dist/M21-boot-*.img                 # 只載入 RAM，重開機就回到原本的 kernel
fastboot getvar current-slot                      # 確認後再寫入對應 slot
fastboot flash boot_<slot> dist/M21-boot-*.img
```

## 跟進上游

建議跟每月的 release tag，例如 `android13-5.15-2027-01_r1`：

```bash
scripts/update-upstream.sh android13-5.15-2027-01_r1
#   exit 0：完成；exit 2：config 有變動，請檢查 config/m21.config.new；exit 1：補丁衝突或 KMI generation 改變
scripts/build.sh                     # KMI 比對必須通過（crc_diff=0、ns_diff=0、removed=0）
sudo python3 scripts/verify-all-modules.py --kernel-config ~/out-m21/.config ~/out-m21/vmlinux.symvers
scripts/pack.sh && fastboot boot dist/M21-boot-*.img      # 實機確認開機、USB、音訊、Wi-Fi、藍牙
cp ~/out-m21/vmlinux.symvers kmi/vmlinux.symvers          # 只有確認相容時才更新參考檔
git add -A && git commit -m "Rebase onto <tag>"
```

**注意事項**
- **`KMI_GENERATION` 必須維持 8**。原廠模組是針對 android13-5.15 KMI 8 編譯的；如果 generation 變了，`update-upstream.sh` 會直接停止。
- 更新後要確認 `CONFIG_I2C_GPIO=y` 還在，並用 root 確認沒有任何裝置停在 `waiting_for_supplier=1`。少了 i2c-gpio，USB 會失效，開機也會被重設（原因見指南第 8 節）。
- 建置時源碼必須是已 commit 的乾淨狀態，所以版本字串是 `5.15.220-m21-android13-8-g<commit>`，不會帶 `-dirty`。

## 授權

`patches/` 是 Linux kernel 的修改，依 kernel 的授權為 GPL-2.0-only。散佈編好的 `Image` 或 boot.img 時，必須同時提供對應的源碼：上游 commit 加上本專案的補丁和 config。本專案不包含 FiiO 的韌體或模組。
