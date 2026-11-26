# 原廠 vendor 模組的公開原始碼還原

FiiO M21 FW 1.1.0 共有 311 個 vendor 模組。其中 275 個屬於 Qualcomm（含上游 Linux 驅動），本目錄從 CodeLinaro 的公開原始碼重新建置這些模組，再逐一和原廠 `.ko` 比對。M21 kernel 本身不需要這些模組（原廠模組可以直接載入，見 [../docs/M21-Kernel-Guide.md](../docs/M21-Kernel-Guide.md)）。這裡的用途是：

- 確認公開原始碼的哪些部分和原廠完全一致；
- 找出 FiiO/Quectel 修改過的部分；
- 日後若要修改或更新某個 Qualcomm 驅動，可以從一個已驗證的起點開始。

English: [README_EN.md](README_EN.md)

## 內容

| 路徑 | 內容 |
|---|---|
| `SOURCES` | 17 個 CodeLinaro 倉庫與釘選的 commit（格式 `路徑\|專案\|commit`） |
| `bengal-m21.config` | 疊加在 `gki_defconfig` + `vendor/bengal_GKI.config` 上的設定 |
| `modules/qcom-intree.list` | 218 個 msm-kernel 樹內模組 |
| `modules/qcom-techpack.list` | 57 個 techpack（外部模組）模組 |
| `modules/fiio-private.list` | 36 個沒有公開原始碼的 FiiO/Quectel/第三方晶片模組 |
| `results/qcom-intree.txt`、`results/qcom-techpack.txt` | 比對結果（每個模組一行） |
| `results/crc-verify.txt` | 換入重建模組後的 CRC 與載入順序驗證結果 |
| `../scripts/fetch-vendor-sources.sh` | 依 `SOURCES` 抓取原始碼（shallow fetch，約 1.5 GB） |
| `../scripts/build-vendor-modules.sh` | 建置樹內模組（`kernel`）和 techpack（`techpacks`），預設 `all` |
| `../scripts/compare-vendor-modules.py` | 比對原廠與重建的模組 |

## 使用方式

需求與主 kernel 相同（WSL2/Linux、clang r450784e 在 `~/toolchains/clang-r450784e`），另外需要 `patch`。工作目錄預設為 `~/m21-vendor`（可用 `VWORK` 改）。

```bash
scripts/fetch-vendor-sources.sh            # 抓取並 checkout 到釘選的 commit
scripts/build-vendor-modules.sh            # 樹內模組約 12 分鐘、techpack 約 25 分鐘（16 核）
scripts/compare-vendor-modules.py ~/m21-vendor/stock ~/m21-vendor/dist \
    --only vendor/modules/qcom-techpack.list --json cmp.json
```

`~/m21-vendor/stock` 是原廠模組，放在同一個目錄（用 `scripts/prepare-stock-modules.sh` 從韌體取出 vendor_boot、vendor_dlkm、system_dlkm 後合併）。

比對方法：兩邊都用同一版 clang 建置，比較

- 函式符號與大小；
- 導出符號（`__ksymtab_`）；
- 未解析的導入符號；
- `.rodata` 字串。

clang 產生的 `.cfi`、`.llvm.N` 等編譯器符號不列入比較。字串裡的建置路徑只保留檔名。

| 判定 | 意思 |
|---|---|
| IDENTICAL | 函式與大小、導出、導入、字串全部相同 |
| CLOSE | ≥95% 函式大小相同，導出與導入相同（通常是內聯或編號差異） |
| DIFFERENT | 其他有建出來的模組 |
| MISSING | 原廠有，但沒有建出來 |

## 原始碼版本怎麼找出來的

- 原廠 vermagic 是 `5.15.41-gf7d0984aa5e2-dirty`。f7d0984aa5e2 不在任何公開倉庫中，是 FiiO/Quectel BSP 的 commit。
- 公開基準是 msm-5.15 的 tag `KERNEL.PLATFORM.2.0.r1-08500-kernel.0`（e4ae393050a4，2023-01-30）。
  - 依據：原廠 `debugcc-khaje.ko` 註冊時脈 mux 的順序是 apss、disp、gpu、mc、gcc。08500 這個 tag 把 gcc 移到最後，更早的 tag 不是這個順序。
  - 用這個基準重建後，218 個樹內模組中有 192 個完全相同。
- 以下 techpack 取自 manifest `AU_LINUX_ANDROID_LA.VENDOR.13.2.0.R1.11.00.00.925.127`：securemsm、mmrm、wlan、bt、dataipa、datarmnet、nfc。
- manifest 裡沒有的 techpack（audio、graphics、display、camera、video），取各分支在 2023-02-01 之前最新的 commit（`SOURCES` 內有註明分支）。

## 建置時的調整（都不改模組程式碼或 CRC）

- `bengal-m21.config`：
  - `CONFIG_FPGA=m`：原廠模組需要，但公開的 bengal 片段沒有。
  - 關閉 `MODULE_SIG` 與 `TRIM_UNUSED_KSYMS`。
  - `DEBUG_INFO_BTF` **必須保持開啟**。`DEBUG_INFO_BTF_MODULES` 會在 `struct module` 加入 `btf_data`、`btf_data_size`，關掉後約 8,800 個 KMI 符號的 CRC 會改變（包括 `module_layout`），模組就無法在 M21 kernel 上載入。
- 新版 pahole（≥1.22）會產生 5.15.41 的 `resolve_btfids` 讀不了的 BTF 類型（enum64、decl/type tag），錯誤是 `load BTF from vmlinux: Invalid argument`。腳本用一個 pahole 包裝程式，加上 `--skip_encoding_btf_enum64 --skip_encoding_btf_decl_tag --skip_encoding_btf_type_tag`。
- `KCFLAGS=-D__ANDROID_COMMON_KERNEL__`：和 Qualcomm 的 `build.config.common` 相同。qcacld 用這個巨集判斷 cfg80211 的 MLO API。
- 外部 techpack 的 Kbuild 用 tab 縮排指令，還有單獨的 `export`。GNU make ≥4.3/4.4 會把它們當成 recipe，或把 kbuild 巨集展開到環境中，導致錯誤或幾乎跑不完。腳本的 `untab()` 會：
  - 改成空白縮排；
  - 刪掉單獨的 `export`。audio 的每個子目錄都會自己 include `config/*.conf`，所以刪掉不影響結果。
- `mmrm-driver/test/`（mmrm_test_module）的原始碼沒有公開，只建置 driver。
- msm-kernel 的 `scripts/extract-cert.c` 套用 `patches/0001`，讓主機工具能用 OpenSSL 3 建置。

## 結果

| 類別 | 模組數 | IDENTICAL | CLOSE | DIFFERENT | MISSING |
|---|---:|---:|---:|---:|---:|
| msm-kernel 樹內 | 218 | 192 | 13 | 13 | 0 |
| techpack | 57 | 42 | 3 | 11 | 1 |
| 合計 Qualcomm | 275 | 234 (85%) | 16 | 24 | 1 |
| FiiO/Quectel 私有 | 36 | — | — | — | 無公開原始碼 |

### 不一致的原因

**FiiO/Quectel 對 Qualcomm 驅動的修改**。這些函式只在原廠模組裡有，公開原始碼找不到：

| 模組 | 原廠多出的內容 |
|---|---|
| socinfo | `quec_get_image_version` |
| phy_msm_snps_hs | `msm_hsphy_ctrl_show/store`（sysfs） |
| msm_geni_serial | `set_manual_flow` |
| sdhci_msm | `set_caps` |
| rtc_pm8xxx | `us_val` 取代 `ms_val` |
| slimbus | `slim_vote_for_suspend` |
| machine_dlkm（音訊 machine driver） | `ext_spk_switch_get/put`、`is_ext_spk_gpio_support`（外接喇叭開關 GPIO） |
| msm_drm | `dsi_panel_fiio_load_work`，並導入 `fiio_device_register` 等 FiiO 符號 |

**版本不完全吻合**。FiiO 的 BSP 混用了不同時間點的 techpack：

| 模組 | 差異 |
|---|---|
| audio（audio_pkt、audpkt_ion、snd_event、spf_core、q6_notifier） | 原廠有 `msm_audio_ion_free_fd_data`，但沒有 `snd_event_master_register_v2` |
| msm_kgsl | 原廠有 hw-fence 和 syncobj tracepoint，表示比釘選的 graphics commit 新 |
| qca_cld3_wlan | 原廠用的是較舊的 `hdd_bus_bw_*`，表示比釘選的 qcacld commit 舊 |
| camera | 原廠多導入 GPIO API（可能是 FiiO 修改，也可能是版本差異） |
| qseecom_dlkm | 公開版本多了 32 位元 compat 轉換函式（設定差異） |

其餘 CLOSE 的模組只差在內聯或 `.N` 編號，功能相同。

**MISSING**：`mmrm_test_module`，原始碼沒有公開。

### CRC 與載入驗證

把 274 個重建模組換進原廠三個分區的完整結構（共 420 個檔案；有些模組同時在 vendor_boot 和 vendor_dlkm），再用 `scripts/verify-all-modules.py` 對照 5.15.220-m21 的 `vmlinux.symvers` 離線驗證。結果在 `results/crc-verify.txt`：

- 原廠模組（基準）：PASS。
- 換入重建模組後：
  - **0 個 CRC 不符**，共解析 23,036 個 kernel 符號參照與 2,423 個模組間參照。
  - 只剩 qca_cld3_wlan 的 4 個載入順序錯誤：釘選的 qcacld 比原廠新，多導入了 `qcom_va_md_*`（minidump），原廠的載入順序不保證先載入它的提供者。

這只是靜態的 ABI 與中繼資料檢查，不代表模組在硬體上能正常 probe 或運作。

### 限制

- 36 個 FiiO/Quectel 私有模組無法從公開原始碼重建，例如 FPGA、DAC、充電、按鍵、觸控、`fiio_*`。這些模組只能保留原廠 binary，或用 Ghidra 做部分逆向。
- 上表中標為 FiiO 修改的模組，如果要替換成重建版，會失去對應的 FiiO 功能，所以**不要**把重建的模組寫進裝置。這個目錄的輸出只用於比對與研究。
