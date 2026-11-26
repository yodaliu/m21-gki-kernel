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

**msm-kernel**: the base is `KERNEL.PLATFORM.2.0.r1-08500-kernel.0` (e4ae393050a4, 5.15.41).

- The stock vermagic `5.15.41-gf7d0984aa5e2-dirty` names a non-public FiiO/Quectel commit.
- The 08500 tag was identified from the clock-mux order in the stock `debugcc-khaje.ko`.

**Techpacks** come from manifest `AU_LINUX_ANDROID_LA.VENDOR.13.2.0.R1.11.00.00.925.127`.

- Projects that manifest does not list (audio, graphics, display, camera, video) use the newest commit before 2023-02-01 on the branch named in `SOURCES`.

**Build-host adjustments** (none change module code):

- `bengal-m21.config` adds `CONFIG_FPGA=m` and turns off module signing and `TRIM_UNUSED_KSYMS`.
- `DEBUG_INFO_BTF` must stay on. `DEBUG_INFO_BTF_MODULES` adds `btf_data`/`btf_data_size` to `struct module`. Without them about 8,800 KMI CRCs change, including `module_layout`, and the modules no longer load on the M21 kernel.
- pahole ≥1.22 emits BTF kinds that 5.15.41's `resolve_btfids` cannot load. The script wraps pahole with `--skip_encoding_btf_enum64 --skip_encoding_btf_decl_tag --skip_encoding_btf_type_tag`.
- `KCFLAGS=-D__ANDROID_COMMON_KERNEL__`, as in Qualcomm's `build.config.common`.
- Techpack Kbuild files are re-indented and their bare `export` lines dropped, so GNU make 4.4 can parse them.
- `mmrm_test_module` is not built because its source is not public.

## Results

| Set | Modules | IDENTICAL | CLOSE | DIFFERENT | MISSING |
|---|---:|---:|---:|---:|---:|
| msm-kernel in-tree | 218 | 192 | 13 | 13 | 0 |
| techpacks | 57 | 42 | 3 | 11 | 1 |
| Qualcomm total | 275 | 234 (85%) | 16 | 24 | 1 |
| FiiO/Quectel private | 36 | no public source | | | |

**FiiO/Quectel changes found in Qualcomm drivers:**

| Module | Stock-only code |
|---|---|
| socinfo | `quec_get_image_version` |
| phy_msm_snps_hs | `msm_hsphy_ctrl` sysfs |
| msm_geni_serial | `set_manual_flow` |
| sdhci_msm | `set_caps` |
| rtc_pm8xxx | `us_val` instead of `ms_val` |
| slimbus | `slim_vote_for_suspend` |
| machine_dlkm | `ext_spk_switch_*` (external speaker switch GPIO) |
| msm_drm | `dsi_panel_fiio_load_work`; imports `fiio_device_*` |

**Revision drift:** FiiO's BSP mixes techpack snapshots.

- **audio:** stock has `msm_audio_ion_free_fd_data` but no `snd_event_master_register_v2`.
- **kgsl:** stock is newer than the pinned commit (it has hw-fence and syncobj tracepoints).
- **qcacld:** stock is older than the pinned commit (it still uses `hdd_bus_bw_*`).

The remaining CLOSE modules differ only in inlining or symbol numbering.

**CRC and load-order check** (`results/crc-verify.txt`): the 274 rebuilt modules were swapped into the full stock partition trees (420 files) and checked with `scripts/verify-all-modules.py` against the 5.15.220-m21 `vmlinux.symvers`.

- Stock passes.
- With the rebuilt modules there are **0 CRC mismatches** (23,036 kernel and 2,423 inter-module references resolved).
- The only errors are 4 load-order errors in qca_cld3_wlan. The pinned qcacld is newer than stock and imports `qcom_va_md_*`, whose provider the stock load order does not guarantee.

This is a static ABI and metadata check. It does not test probing or behaviour on hardware.

Do **not** install the rebuilt modules on the device. The modules with FiiO changes would lose those features, and these builds are for comparison only.
