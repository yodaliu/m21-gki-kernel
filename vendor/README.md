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
| `patches/kernel_platform/msm-kernel/` | 樹內模組的 patch：6 個上游 commit ＋ 9 個從原廠模組還原的 FiiO/Quectel 修改 |
| `patches/vendor/qcom/opensource/<techpack>/` | techpack 的 patch：audio-kernel、graphics-kernel、securemsm-kernel、display-drivers、wlan/qcacld-3.0（上游 commit／revert ＋ 還原的 FiiO／Quectel 修改） |
| `symvers/fiio-private.symvers` | FiiO 私有模組 `fiio_primary_modules` 導出給 msm_drm 的 2 個符號與 CRC（只有 ABI 中繼資料） |
| `fiio-modules/` | 從原廠 binary 逆向還原的 FiiO 模組原始碼：`primary_modules/`（`fiio_primary_modules`，9 個 .c）、`test/`（4 個除錯／測試模組）、`extcon/`、`codecs/`、`input/`（按鍵、觸控、goodix gt9xx 與 hynitron 觸控、aw95016 GPIO 擴充晶片）、`usb/`（fusb302 Type-C PD）、`misc/`、`leds/`、`underlay_modules/`、`fiio_config/`、`fpga/`（FPGA 載入與介面）、`board/`（類比板）、`power/`（電量計、充電 IC、電源板、charger manager）、`quectel/`（Quectel 的 gpio 層、CCI 橋接與測試、模組檢查）、`include/linux/`（Quectel 標頭，檔名與行號依原廠 camera.ko 的 DWARF）、`include/fiio/`（共用標頭，結構依原廠 DWARF），共 35 個模組 |
| `results/qcom-intree.txt`、`results/qcom-techpack.txt` | 比對結果（每個模組一行） |
| `results/fiio-modules.txt` | FiiO/Quectel 模組比對結果（每個模組一行） |
| `results/crc-verify.txt` | 換入重建模組後的 CRC 與載入順序驗證結果 |
| `../scripts/fetch-vendor-sources.sh` | 依 `SOURCES` 抓取原始碼（shallow fetch，約 1.5 GB） |
| `../scripts/build-vendor-modules.sh` | 建置樹內模組（`kernel`）和 techpack（`techpacks`），預設 `all` |
| `../scripts/compare-vendor-modules.py` | 比對原廠與重建的模組 |
| `../scripts/extract-gw-fpga-firmware.py`、`../scripts/extract-hynitron-firmware.py` | 建置時從原廠模組取出 FPGA bitstream 與觸控韌體，產生標頭（不收錄在 repo） |
| `../scripts/prepare-stock-modules.sh`、`../scripts/verify-all-modules.py` | 從韌體取出原廠三個分區的模組；離線驗證 CRC 與載入順序 |

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
- 公開基準是 msm-5.15 的 tag `KERNEL.PLATFORM.2.0.r12-01500-kernel.0`（5fc62c1db11e，2023-03-29，分支 `kernel.lnx.5.15.r16-rel`，即 LA.VENDOR.13.2.1.R2 用的 R12 kernel platform）。
  - 依據：逐一建置候選 tag，比較完全相同的樹內模組數。r12-01500 有 206/218 相同（先前用的 r1-08500 只有 192）。
  - 原廠另外帶了 r12-01500 之後的 6 個上游 commit（gcc-khaje SDCC floor ops、4 個 WALT commit、dwc3 core 標為 syscore 裝置 f4a82a39），以原作者資訊收在 `patches/`。
  - 其餘差異是 FiiO/Quectel 的修改：用反組譯比對還原成原始碼 patch，重建後與原廠一致（見下方）。
- 以下 techpack 取自 manifest `AU_LINUX_ANDROID_LA.VENDOR.13.2.0.R1.11.00.00.925.127`：securemsm、mmrm、wlan platform、fw-api、bt、dataipa、datarmnet、nfc。
- qcacld-3.0 與 qca-wifi-host-cmn：原廠模組內的版本字串是 `2.0.9.15N`，對應 `wlan-cld3.driver.lnx.2.0.9.r1-rel` 分支的「Release 2.0.9.15N」（cabc600，2023-01-17）；cmn 取 `wlan-cmn.driver.lnx.2.0.9.r1-rel` 上在它之前的最後一個 commit（509b149）。
- manifest 裡沒有的 techpack（audio、graphics、display、camera、video），取各分支在 2023-02-01 之前最新的 commit（`SOURCES` 內有註明分支）。
  - graphics 改釘 `aa5e1f9`（2023-01-16）：在分支歷史中逐一比對原廠有／沒有的符號選出。
  - 原廠沒有、但釘選版本有的上游 commit 用 revert patch 拿掉；原廠有、但釘選版本沒有的用 cherry-pick patch 補上（保留原作者）。

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
- qcacld 以 `TARGET_BUILD_VARIANT=user` 和 `WLAN_CTRL_NAME=wlan` 建置，與原廠相同：user build 不含 crash inject、pktlog、TX descriptor 重複偵測等除錯功能；`WLAN_CTRL_NAME` 會加入 framework 用來開關 Wi-Fi 的 `qcwlanstate` 字元裝置。
- msm-kernel 的 `scripts/extract-cert.c` 套用 `patches/0001`，讓主機工具能用 OpenSSL 3 建置。
- `build-vendor-modules.sh` 會把 `vendor/patches/<路徑>/` 的 patch 依序套到該 checkout 的工作目錄；已套用過就跳過。
- msm_drm 導入 FiiO 私有模組的 `fiio_device_register`、`fiio_device_get_data_by_name`。建置時以 `KBUILD_EXTRA_SYMBOLS` 帶入 `symvers/fiio-private.symvers`，讓 msm_drm 記錄與原廠相同的 CRC。
- `techpacks` 最後建置 `fiio-modules`（複製到工作目錄外）：35 個模組在同一次 Kbuild 中建置，其他模組直接從 `fiio_primary_modules` 解析導入。
- 還原模組的導出 CRC 除了下列例外都與原廠相同（`fiio_primary_modules` 46 個、fiio_config 8 個、fiio_underlay_modules 4 個，以及 fiio_sw_hold、cw221X_fuel_gauge、aw95016、quectel_cci_test 各 1 個）。例外是 gw-fpga 的 `fpga_ctrl_power_by_mode`（9 個中 8 個相同；原廠 fpga_audio_interface 導入它，所以重建的 gw-fpga 不能和原廠 fpga_audio_interface 混用），以及 gpiolib-quectel、quectel_cci_driver（見下方「限制」）。genksyms 的 CRC 會受各 .c 引入哪些標頭影響，所以各檔的 `#include` 是照 CRC 比對結果調整的（例如 `dac_dai.h` 用 `sound/asound.h` 而不是 `sound/pcm.h`）。

## 結果

| 類別 | 模組數 | IDENTICAL | CLOSE | DIFFERENT | MISSING |
|---|---:|---:|---:|---:|---:|
| msm-kernel 樹內 | 218 | 216 | 2 | 0 | 0 |
| techpack | 57 | 49 | 6 | 1 | 1 |
| 合計 Qualcomm | 275 | 265 (96%) | 8 | 1 | 1 |
| FiiO/Quectel 逆向還原（`fiio-modules/`） | 35 | 24 | 7 | 4 | 0 |
| FiiO/Quectel 私有（其餘） | 1 | — | — | — | 無公開原始碼 |

### 不一致的原因

**樹內模組的 FiiO/Quectel 修改（已還原，`patches/kernel_platform/msm-kernel/0007`–`0015`）**：

| 模組 | 修改 | 重建結果 |
|---|---|---|
| phy_msm_snps_hs | sysfs `ctrl`：讀寫 PHY 調校參數 `param_ovrd0..3`、重新初始化；vbus 電流 1000 mA | IDENTICAL |
| socinfo | `quec_get_image_*_version`，逐欄位解析 image version | CLOSE（1 個函式的編譯器排程不同，邏輯相同） |
| sdhci_msm | 只有 mmc0（eMMC）開 `MMC_CAP_AGGRESSIVE_PM` | IDENTICAL |
| msm_geni_serial | ttyHS0 以外的 UART 固定 115200 | IDENTICAL |
| qpnp_power_on | 按鍵事件一律印出 | IDENTICAL |
| pinctrl_spmi_gpio | probe 結尾延遲 100 ms | IDENTICAL |
| qcom_spmi_adc5 | 新增 PM6125 AMUX1/AMUX3 電壓通道 | IDENTICAL |
| snd_usb_audio_qmi | offload 串流固定 480 frames × 8 periods；設定 endpoint 前先選 altsetting | IDENTICAL |
| dwc3_msm | suspend 時在 `disable_irq_nosync()` 後再對 power event irq 呼叫一次 `disable_irq()` | IDENTICAL |

剩下 2 個 CLOSE：socinfo（`quec_get_image_version` 的錯誤分支排列順序不同，邏輯相同）、slimbus（函式全部相同，只有字串合併方式不同）。

**techpack 的修改（已還原，`patches/vendor/qcom/opensource/`）**：用 Ghidra 反編譯原廠模組（帶 DWARF），再對照反組譯與字串還原。

| techpack | patch | 重建結果 |
|---|---|---|
| audio-kernel | revert 82ba6ff（snd card standby 狀態）、revert 92a2717（ion free 加鎖）；FiiO：APM close-all 逾時時改用 `msm_audio_ion_free_fd_data()`，不 unmap ADSP 可能仍在用的 buffer；bengal machine driver 的 FiiO 音訊路由（PRI MI2S RX 接 `fiio_codec`、`qcom,mi2s-master` 時由 SoC 送出 I2S 時脈與 MCLK_2、`qcom,wcd-disabled`、外接喇叭 GPIO 與 `Ext_Spk_Switch` 控制項） | 26 個模組全部 IDENTICAL |
| securemsm-kernel | revert b5f2395（qseecom 32 位元 compat） | 全部 IDENTICAL |
| graphics-kernel | 上游 4d87657（deferred_destroy 空指標修正）、revert 085a050 | msm_kgsl CLOSE（只差 CFI jump table 的編號） |
| qcacld-3.0 | 上游 5a1d5c8（無效頻道的 CSA 改為忽略）、f0b7c74（11BE AP 連線修正）；FiiO/Quectel：RSO stop timer 只在 HO_FAILED 或 RSO_STATUS 成功（notif_params 為 0）時停止並印出 log、漫遊事件 log 加印 notif_params、ROAMING_IN_PROG 時再收到 roam start 不視為錯誤、`read_mac_addr_from_mac_file` 預設改為 1（從 mac 檔讀取 MAC 位址） | qca_cld3_wlan CLOSE（只差 CFI jump table 的編號） |
| display-drivers | revert 5d6cec2（原廠仍保留這些錯誤 log）、revert a551760（原廠 rotator 不設 `DMA_ATTR_SKIP_CPU_SYNC`）；FiiO：in-cell 觸控面板的 `qcom,platform-tp-reset-gpio` 重置、手勢喚醒時關機不關 reset/disp_en、透過 `fiio_device_register` 取得觸控驅動的 `ts module` 資料 | msm_drm CLOSE（只差 `dsi_panel_power_on` 的暫存器配置） |

**沒有還原的模組**：

| 模組 | 原因 |
|---|---|
| camera | 原廠把 Quectel 私有的 aw95016／CCI 驅動程式碼編進 camera.ko（約 60 個函式，並導入 `quectel_*` 符號）。這屬於私有驅動，依原則不還原。 |

剩下 CLOSE 的模組都已逐一比對反組譯：

- ipam、msm_kgsl、qca_cld3_wlan：只差 CFI jump table 名稱裡的編號（`name.NNNN_jt`），大小全部相同。
- rmnet_core：所有函式大小相同，只是比對工具對同名 static 函式的計數方式造成。
- bt_fm_slim：原廠多一個空函式 `btfm_slim_get_hwep_details`（只有 `ret`，沒有導出），公開原始碼沒有。
- msm_drm：`dsi_panel_power_on` 只差暫存器配置。

**MISSING**：`mmrm_test_module`，原始碼沒有公開。

### CRC 與載入驗證

把 309 個重建模組（Qualcomm 274 個＋FiiO 還原 35 個）換進原廠三個分區的完整結構（共 485 個檔案；有些模組同時在 vendor_boot 和 vendor_dlkm），再用 `scripts/verify-all-modules.py` 對照 5.15.220-m21 的 `vmlinux.symvers` 離線驗證。結果在 `results/crc-verify.txt`：

- 原廠模組（基準）：PASS。
- 換入重建模組後：
  - **0 個 CRC 不符**，共解析 23,075 個 kernel 符號參照與 2,431 個模組間參照，沒有缺少版本記錄的導入。重建的 aw95016 導入重建版 gpiolib-quectel，記錄的是重建版的 CRC。
  - 載入順序檢查沒有錯誤（先前 qcacld 釘選得比原廠新，多導入 `qcom_va_md_*`，造成 4 個載入順序錯誤；改釘原廠版本後已消失）。

這只是靜態的 ABI 與中繼資料檢查，不代表模組在硬體上能正常 probe 或運作。

### 限制

- 36 個 FiiO/Quectel 私有模組沒有公開原始碼。其中 35 個已依 Ghidra 反編譯和原廠 DWARF 重寫，24 個建出來與原廠 IDENTICAL、7 個 CLOSE、4 個 DIFFERENT：
  - 共用模組 `fiio_primary_modules`（裝置／sysfs 屬性註冊、notifier、I2C／regmap 包裝、DAC 與觸控選擇工具）；
  - 4 個除錯／測試模組（ADC、GPIO、UART 除錯與 SPI 測試）；
  - extcon-quec、fiio_snd_soc_dummy_dai、fiio_gpio_key、fiio_sw_hold、fiio_externel_key（外接按鍵配件）、lcd_id、ts_core、fiio_wakeup、led、fiio_underlay_modules、fiio_config（fiioconfig 分區的設定紀錄）。
  - 音訊鏈：fpga_interface、fpga_audio_interface、cs43198、fiio_headset_jack（IDENTICAL），gw-fpga（Gowin FPGA 載入器）、dac_core_module（CLOSE，邏輯相同，只差少數函式的分支排列或暫存器配置），anolog_board_m21（DIFFERENT，26 個函式中 24 個大小相同）。
  - 電源：cw221X_fuel_gauge（電量計，CLOSE 21/22）、fiio-charger-manager（CLOSE 51/53）、sgm41510_charger（充電 IC，DIFFERENT 31/33）、power_board_m21（電源板，DIFFERENT 35/37）。四個模組的導出、導入與字串都和原廠相同，cw221X 導出的 `extern_get_cap` CRC 也相同。
  - 原廠 kernel 的 `enum power_supply_property` 在 68 插入 `POWER_SUPPLY_PROP_CHARGING_ENABLED`，之後的值都比 GKI 大 1。`include/fiio/fiio_power_supply.h` 用原廠的編號。
  - 原廠的 `struct power_supply_config`（0x30）和 `struct power_supply_desc`（0x60）沒有 `ANDROID_KABI_RESERVE`，GKI 版各多 8 位元組。所以 charger manager 結構在 0x478 之後的位移都差 8，`fiio_charger_manager_probe` 也大 12 位元組。這是 kernel 標頭的差異，模組端無法修正。
  - gw-fpga 內含 FPGA bitstream，本 repo 不收錄：建置時由 `scripts/extract-gw-fpga-firmware.py` 從原廠 `gw-fpga.ko` 取出並產生標頭。找不到原廠模組時改用全零的 bitstream 建置（只能用於比對）。
  - Quectel（IDENTICAL）：gpiolib-quectel（GPIO 擴充晶片的統一 gpio 編號與 debugfs `quectel-gpio`）、quectel_cci_driver（camera.ko 註冊 CCI 讀寫函式，供其他驅動使用）、quectel_cci_test（CCI 與 aw95016 gpio 的 debugfs 測試）、q_check_kernel_client（透過 127.0.0.1:9083 對 modem 服務送 AT 指令，檢查 NV 資料或隨機數算出的檢查碼）。這 4 個模組沒有 DWARF，只依反編譯和反組譯重寫；結構和標頭取自原廠 camera.ko 的 DWARF。
  - Quectel 模組的導出 CRC：
    - q_check_kernel_client：原廠的 genksyms 沒有為 `quectel_kernel_model_check` 產生 CRC（`__crc_` 是未定義的 weak 符號），重建版用 `#ifndef __GENKSYMS__` 重現。
    - gpiolib-quectel：11 個中 9 個相同（gpio 參數寫成 `unsigned`，`irq_func` 是 `void (*)(unsigned, void *)`）。`quectel_get_named_gpio`、`quectel_gpio_register_dev` 不同，兩者都會展開 `struct device_node`，推測原廠 kernel 在這條型別鏈上和 GKI 不同。
    - quectel_cci_driver：9 個中 2 個相同（`quectel_cci_load_result`，以及用 `typedef int (*subdev_probe)(void *)` 的 `quectel_cci_register_subdev`）。另外 7 個含 camera 型別，原廠的 Quectel 標頭應該自帶一份 camera 型別，內容無法從 binary 還原，所以對不上。
    - quectel_cci_test：`quectel_cci_test_func` 相同。
    - 原廠 camera.ko 導入這些符號，所以重建的 Quectel 模組不能和原廠 camera.ko 混用。
  - aw95016（Awinic 16 路 I2C GPIO 擴充晶片，`input/misc/`，CLOSE 46/48）：兩顆晶片（chip_index 2 為 ex2，其餘為 ex3）向 gpiolib-quectel 註冊腳位，並在 debugfs `aw95016_ex2`／`aw95016_ex3` 提供測試檔。結構依原廠 camera.ko 內那份 aw95016.c 的 DWARF。導出的 `extern_gpio_config` CRC 相同。差異只在兩個 debugfs 測試函式的暫存器配置（原廠沒有把兩個分支共同的第一次 I2C 讀取提到分支前）。
  - fusb302（`usb/typec/fusb302/`，CLOSE 29/30）：以 Rockchip kernel 的 `drivers/mfd/fusb302.c`（f6c3b58，2020-12，加入 sink cap 之前的版本）為基礎，原廠 DWARF 的行號與這一版一致。FiiO 的修改：暫存器存取改用 `fiio_regmap_*`、拿掉 VBUS_EN extcon、充電協定 `charger_protocol_qti module`（power role、VBUS 狀態、cc-only 模式切換）、角色切換時保留 VBUS（`preserve_vbus`）、插拔與 OTG 透過 notifier 通知、sysfs `cc_state`。差異只在大幅內聯的 `fusb302_work_func`（所有呼叫都相同，只差暫存器配置）。
  - goodix_gt9xx_ts（`input/touchscreen/goodix_gt9xx_v281_fiio/`，CLOSE 76/78）：以 Goodix 公開的 gt9xx_driver_android V2.8.1 為基礎，gt9xx_update.c 和 goodix_tool.c 與上游逐行相同。FiiO 的修改：ts_core 註冊（`try_to_ping`，探測失敗時延後重試）、6 個按鍵、`inversion-x`、依 version id 選 `goodix,cfg-data-bk%d`／`goodix,cfg-data%d`、hold 開關時丟棄觸控事件、sysfs `gt9xx_gesture_enable`、改由 ts_core 呼叫 suspend／resume（沒有 PM ops）。差異只在 `gtp_get_fw_info` 的暫存器配置（8 bytes）。
  - hynitron_touch（`input/touchscreen/hynitron_cst/`，DIFFERENT 191/220）：以 Hynitron 公開驅動 V2.21 為基礎，補到原廠的 V2.25 20260320。cst76xx、cst840u、cst923xx 沒有公開原始碼，依反編譯從相近晶片的程式改寫（cst840u、cst76xx 由 cst66xx 改寫，cst923xx 由 cst92xx 改寫並加上 93xx 系列的分頁寫入與晶片表）；cst36xxes、cst8xxT 與核心、sysfs（`hynselftest`）、手勢也依原廠修正。導出、導入與字串都和原廠相同。差異只在暫存器配置、堆疊配置、迴圈展開（cst840u／cst76xx 的 `set_workmode`）和內聯（cst8xxT 的 `set_workmode` 被內聯進 init／resum）。觸控韌體不收錄：建置時由 `scripts/extract-hynitron-firmware.py` 從原廠 `hynitron_touch.ko` 取出，產生 `hyn_chips/*_fw.h`；找不到原廠模組時改用全零的韌體。`MODULE_IMPORT_NS(VFS_internal_...)` 是 5.15.220 的 `kernel_read` 需要的，原廠 5.15.41 沒有。
  - 其餘 1 個（aw35615_whole）找不到公開原始碼可作基礎（Awinic 移植的 onsemi FUSB30x PD 協定堆疊，約 410 個函式），只能保留原廠 binary。
- `spi_test_driver` 的原廠模組資訊沿用 AD7280A 驅動的作者與描述，重建版照原樣保留。
- 還原的 FiiO 修改是依反編譯結果重寫的，沒有在硬體上驗證；camera 的重建版也與原廠不同。**不要**把重建的模組寫進裝置，這個目錄的輸出只用於比對與研究。
