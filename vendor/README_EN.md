# Rebuilding the stock vendor modules from public source

The FiiO M21 FW 1.1.0 ships 311 vendor modules, and 275 of them are Qualcomm or upstream-Linux drivers. This directory rebuilds those 275 from public CodeLinaro sources and compares each one with the stock `.ko`.

The M21 kernel itself does not need any of this, because it loads the stock modules unchanged. The point is to see:

- which parts of the public source match stock exactly;
- where FiiO/Quectel changed Qualcomm code;
- a verified starting point for any future change to a Qualcomm driver.

Chinese version with full detail: [README.md](README.md)

## Usage

```bash
scripts/fetch-vendor-sources.sh      # pinned commits from vendor/SOURCES into ~/m21-vendor (VWORK), ~1.5 GB
scripts/build-vendor-modules.sh      # [kernel|techpacks|all]; ~12 min in-tree + ~25 min techpacks on 16 cores
scripts/compare-vendor-modules.py ~/m21-vendor/stock ~/m21-vendor/dist --only vendor/modules/qcom-techpack.list
```

The build needs clang r450784e at `~/toolchains/clang-r450784e` and `patch`. `stock/` is the stock modules from vendor_boot, vendor_dlkm and system_dlkm, merged into one directory.

The compare script checks:

- function symbols and their sizes;
- exports;
- undefined imports;
- `.rodata` strings.

Compiler-generated `.cfi` and `.llvm.N` symbols are ignored.

Verdicts:

- **IDENTICAL**: everything above matches.
- **CLOSE**: at least 95% of functions have the same size, and exports and imports are equal.
- **DIFFERENT**: anything else that was built.
- **MISSING**: present in stock but not built.

## Sources

**msm-kernel**: the base is `KERNEL.PLATFORM.2.0.r12-01500-kernel.0` (5fc62c1db11e, 5.15.41, 2023-03-29, branch `kernel.lnx.5.15.r16-rel`).

- The stock vermagic `5.15.41-gf7d0984aa5e2-dirty` names a non-public FiiO/Quectel commit.
- The tag was chosen by building candidate tags and counting identical in-tree modules (206/218).
- `patches/kernel_platform/msm-kernel/` holds 6 upstream commits stock carries beyond the tag (gcc-khaje SDCC floor ops, 4 WALT commits, f4a82a39 dwc3 core as syscore device), and 9 FiiO/Quectel changes reconstructed from the stock modules. `build-vendor-modules.sh` applies them, and skips them when already applied.

**Techpacks** come from manifest `AU_LINUX_ANDROID_LA.VENDOR.13.2.0.R1.11.00.00.925.127`.

- Projects that manifest does not list (audio, graphics, display, camera, video) use the newest commit before 2023-02-01 on the branch named in `SOURCES`.
- qcacld-3.0 and qca-wifi-host-cmn: stock reports version `2.0.9.15N`, which is "Release 2.0.9.15N" (cabc600, 2023-01-17) on `wlan-cld3.driver.lnx.2.0.9.r1-rel`; cmn is the last commit before it on `wlan-cmn.driver.lnx.2.0.9.r1-rel` (509b149).
- graphics is pinned to `aa5e1f9` (2023-01-16), chosen by scoring the branch history on symbols stock has and lacks.
- `patches/vendor/qcom/opensource/<techpack>/` reverts upstream commits stock lacks, cherry-picks ones it has (original authors kept), and adds the reconstructed FiiO changes.

**Build-host adjustments** (none change module code):

- `bengal-m21.config` adds `CONFIG_FPGA=m` and turns off module signing and `TRIM_UNUSED_KSYMS`.
- `DEBUG_INFO_BTF` must stay on. `DEBUG_INFO_BTF_MODULES` adds `btf_data`/`btf_data_size` to `struct module`. Without them about 8,800 KMI CRCs change, including `module_layout`, and the modules no longer load on the M21 kernel.
- pahole ≥1.22 emits BTF kinds that 5.15.41's `resolve_btfids` cannot load. The script wraps pahole with `--skip_encoding_btf_enum64 --skip_encoding_btf_decl_tag --skip_encoding_btf_type_tag`.
- `KCFLAGS=-D__ANDROID_COMMON_KERNEL__`, as in Qualcomm's `build.config.common`.
- Techpack Kbuild files are re-indented and their bare `export` lines dropped, so GNU make 4.4 can parse them.
- `mmrm_test_module` is not built because its source is not public.
- qcacld is built with `TARGET_BUILD_VARIANT=user` and `WLAN_CTRL_NAME=wlan`, like stock: the user build drops crash inject, pktlog and TX descriptor dup detection, and `WLAN_CTRL_NAME` adds the `qcwlanstate` char device the framework uses to switch Wi-Fi on and off.
- msm_drm imports `fiio_device_register` and `fiio_device_get_data_by_name` from the FiiO private module `fiio_primary_modules`. `symvers/fiio-private.symvers` holds their export CRCs (ABI metadata only) and is passed in `KBUILD_EXTRA_SYMBOLS`, so msm_drm records the same CRCs as stock.
- The `techpacks` step ends by building `fiio-modules`, rewritten from Ghidra decompilation and the stock DWARF: 23 modules in one Kbuild pass (shared headers in `include/fiio/`), so the others resolve their imports from `fiio_primary_modules` directly:
  - `fiio_primary_modules` (9 .c files);
  - the 4 debug/test modules (`fiio_adc_debug_module`, `fiio_gpio_debug_module`, `fiio_uart_debug_module`, `spi_test_driver`);
  - extcon-quec, fiio_snd_soc_dummy_dai, fiio_gpio_key, fiio_sw_hold, fiio_externel_key, lcd_id, ts_core, fiio_wakeup, led, fiio_underlay_modules and fiio_config.
  - the audio chain: gw-fpga (Gowin FPGA loader), fpga_interface, fpga_audio_interface, dac_core_module, cs43198, anolog_board_m21 and fiio_headset_jack.
- gw-fpga embeds the FPGA bitstream. It is not in this repo: `scripts/extract-gw-fpga-firmware.py` copies it at build time from the stock `gw-fpga.ko` into a generated header, so the build needs the stock module.
- All export CRCs of the reconstructed modules match stock (46 in `fiio_primary_modules`, plus ts_core, fiio_sw_hold, fiio_underlay_modules, fiio_config and others). genksyms CRCs depend on which headers each .c includes, so the `#include` lists were tuned against the stock CRCs (for example `dac_dai.h` uses `sound/asound.h`, not `sound/pcm.h`).

## Results

| Set | Modules | IDENTICAL | CLOSE | DIFFERENT | MISSING |
|---|---:|---:|---:|---:|---:|
| msm-kernel in-tree | 218 | 216 | 2 | 0 | 0 |
| techpacks | 57 | 49 | 6 | 1 | 1 |
| Qualcomm total | 275 | 265 (96%) | 8 | 1 | 1 |
| FiiO/Quectel reverse-engineered (`fiio-modules/`) | 23 | 20 | 3 | 0 | 0 |
| FiiO/Quectel private (rest) | 13 | no public source | | | |

**In-tree FiiO/Quectel changes, reconstructed** (patches 0007–0015):

| Module | Change | Rebuilt |
|---|---|---|
| phy_msm_snps_hs | sysfs `ctrl` for PHY tuning overrides; 1000 mA vbus draw | IDENTICAL |
| socinfo | `quec_get_image_*_version` field parsing | CLOSE (one function schedules differently) |
| sdhci_msm | `MMC_CAP_AGGRESSIVE_PM` only for mmc0 | IDENTICAL |
| msm_geni_serial | 115200 baud on every UART except ttyHS0 | IDENTICAL |
| qpnp_power_on | always log key events | IDENTICAL |
| pinctrl_spmi_gpio | 100 ms delay at end of probe | IDENTICAL |
| qcom_spmi_adc5 | PM6125 AMUX1/AMUX3 voltage channels | IDENTICAL |
| snd_usb_audio_qmi | 480-frame × 8 periods; set altsetting before endpoint setup | IDENTICAL |
| dwc3_msm | on suspend, `disable_irq()` the power event irq right after `disable_irq_nosync()` | IDENTICAL |

The other two CLOSE modules: socinfo (error branches of `quec_get_image_version` laid out in a different order, same logic) and slimbus (all functions identical, only string merging differs).

**Techpack changes, reconstructed** (`patches/vendor/qcom/opensource/`), from Ghidra decompilation of the stock modules (which carry DWARF) checked against disassembly and strings:

| Techpack | Patches | Rebuilt |
|---|---|---|
| audio-kernel | revert 82ba6ff (snd card standby state) and 92a2717 (ion free lock); FiiO: on an APM close-all timeout, drop the fd list with `msm_audio_ion_free_fd_data()` instead of unmapping buffers the ADSP may still use; bengal machine driver routing (PRI MI2S RX to `fiio_codec`, SoC-driven I2S clocks and MCLK_2 with `qcom,mi2s-master`, `qcom,wcd-disabled`, external speaker GPIO and `Ext_Spk_Switch`) | all 26 IDENTICAL |
| securemsm-kernel | revert b5f2395 (qseecom 32-bit compat) | all IDENTICAL |
| graphics-kernel | upstream 4d87657 (deferred_destroy NULL fix), revert 085a050 | msm_kgsl CLOSE (CFI jump-table numbering only) |
| qcacld-3.0 | upstream 5a1d5c8 (ignore CSA to an invalid channel) and f0b7c74 (11BE AP connect fix); FiiO/Quectel: stop the RSO stop timer only on HO_FAILED or a successful RSO_STATUS (notif_params 0), with a log; log notif_params in roam events; a roam start in ROAMING_IN_PROG is not an error; `read_mac_addr_from_mac_file` defaults to 1 (MAC address from the mac file) | qca_cld3_wlan CLOSE (CFI jump-table numbering only) |
| display-drivers | revert 5d6cec2 (stock keeps these error logs), revert a551760 (stock's rotator does not set `DMA_ATTR_SKIP_CPU_SYNC`); FiiO: in-cell touch reset `qcom,platform-tp-reset-gpio`, keep reset/disp_en on at power off during gesture wake-up, look up the touch driver's `ts module` data through `fiio_device_register` | msm_drm CLOSE (register allocation in `dsi_panel_power_on`) |

**Not reconstructed:**

- **camera:** stock compiles Quectel's private aw95016/CCI driver code into camera.ko (about 60 functions, importing `quectel_*`). That is private-driver code, so it is left alone.

Each remaining CLOSE module was checked by disassembly:

- ipam, msm_kgsl, qca_cld3_wlan: only the numbers in CFI jump-table names (`name.NNNN_jt`) differ; all sizes match.
- rmnet_core: all function sizes match; the count comes from how the compare tool handles same-name static functions.
- bt_fm_slim: stock has one extra empty function, `btfm_slim_get_hwep_details` (just `ret`, not exported), that the public source lacks.
- msm_drm: `dsi_panel_power_on` differs only in register allocation.

**CRC and load-order check** (`results/crc-verify.txt`): the 297 rebuilt modules (274 Qualcomm + 23 reconstructed FiiO) were swapped into the full stock partition trees (466 files) and checked with `scripts/verify-all-modules.py` against the 5.15.220-m21 `vmlinux.symvers`.

- Stock passes.
- With the rebuilt modules there are **0 CRC mismatches** (23,075 kernel and 2,431 inter-module references resolved; no import lacks a version record).
- There are **0 errors**, load order included. The 4 qca_cld3_wlan load-order errors (`qcom_va_md_*` imports of the newer qcacld) went away with the stock-matching pin.

This is a static ABI and metadata check. It does not test probing or behaviour on hardware.

Do **not** install the rebuilt modules on the device. The FiiO changes are rewritten from decompilation and untested on hardware, camera still differs from stock, and these builds are for comparison only.
