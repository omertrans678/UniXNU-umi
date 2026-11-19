# UniXNU-umi — Xiaomi Mi 10

English | [简体中文](README.zh-CN.md)

An independent UniXNU device port for **Xiaomi Mi 10 (`umi`, SM8250, Adreno 650)**.
The tested 12 GiB / 256 GB handset boots **iPadOS 17.7.11 (21H461)** to the
**SpringBoard home screen** with Setup skipped, with GPU-accelerated Metal,
direct display scanout and working touch. This is a development source
snapshot, not a ready-to-install release.

| Repository | Role |
| --- | --- |
| [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | Board, XNU integration, drivers, diagnostics and host tools |
| [uefi-umi](https://github.com/reAAAq/uefi-umi) | Mu-Silicium firmware port, derived from uefi-sheng |
| [UniXNU](https://github.com/UniXNU/UniXNU) | Upstream build driver, loader, generic drivers and patches |

## Scope and status

Status of the tested handset at build `umi-192` (2026-11-19).

| Feature | State | Evidence / limitation |
| --- | --- | --- |
| UEFI menu and RAM payload | Working | Menu, file verification and DarwinBoot handoff |
| XNU and SMP | Working | Eight CPUs; big clusters raised to their top level (`umi-cpufreq=4`), no DCVS yet |
| UFS / OS / SystemOS | Working | Root and cryptex mounted; concurrent reads on UTRD slots 2..16 |
| Writable Data | Working | Persistent store with read-ahead; file systems synced before the `umi-ar` reset |
| SpringBoard | Working | Setup skipped (`umi-skip-setup=1`); 4 GiB OS memory window |
| GPU / Metal | Working | Adreno 650 driver (`package/umi-adreno`), table mode 2, hang recovery verified |
| Display | Working | Direct DPU scanout (`dpu-scanout=2`), BGRA8 composition (ADT gamut sRGB) |
| ST FTS touch | Working | Unlock, taps and gestures; unknown events are skipped instead of stopping the driver |
| Two-ring GPU preemption | Off | Switches work, but animations show black tile dots (`gpu-preempt` left unset) |
| Shader cache warm-up | Not done | Needs macOS |
| Wi-Fi, Bluetooth, cellular, audio, camera, sensors | Not implemented | |
| USB under XNU, battery, suspend/resume | Not qualified | Recovery ADB is separate from XNU USB support |

Known issues: lsd sometimes rebuilds the LaunchServices store at boot and holds
SpringBoard at the Apple logo for that boot; app launches are still slower than
on the donor. [Validation summary](docs/validation.json) keeps selected
non-identifying measurements. Original device dumps and logs stay local.

## Prerequisites

- A Linux or WSL2 host with the [Linux toolchain](https://unixnu.com/guide/requirements#linux);
  macOS for Data-seed tools and the shader cache.
- The donor inputs: **iPad7,1 / iPadOS 17.7.11 / 21H461 / j120ap**, source kernel
  **XNU 10063.141.1 / Darwin 23.6.0** (`ipsw-inputs.json`). No Apple or vendor
  binary inputs are included; the generated GPU firmware source is git-ignored.
- A device with TWRP recovery and a backup of every partition. The install
  replaces Android userdata; there is no general installer here.
- Pinned sources: `source-lock.json`, `feeds.toml`, `unixnu.lock`.

## Build

Workspace setup, pins and host checks are in [BUILD.md](docs/BUILD.md). The
phone profile is assembled by the tools under `bringup/`:

```bash
python3 tools/build_umi_native_ufs.py --profile phone && python3 tools/build_stage10_pipeline.py
```

Run it from the build workspace (`umi-work/`, where `devices/umi` points to this
repository). The output is `out/umi/boot/umi-xnu-stage10-systemos-boot.img`;
flash it from TWRP to `boot` only after a `SUCCESS: Stage 10 Boot Image` line.
The GPU library alone builds with `scripts/build-umi-adreno.py`. Kernel
arguments live in `package/unixnu-xnu-prelink.toml` and must stay under about
960 characters; keep `umi-ar` (auto-reset to recovery) set.

## Required reading

- [Port a device](https://unixnu.com/porting/steps) and
  [device repository](https://unixnu.com/porting/device-repository)
- [UFS storage](https://unixnu.com/porting/ufs), [display](https://unixnu.com/porting/display),
  [input](https://unixnu.com/porting/input), [GPU and Metal](https://unixnu.com/porting/gpu)
- [Data volume](https://unixnu.com/guide/userdata#data-volume),
  [Skip Setup](https://unixnu.com/guide/skip-setup), [Building](https://unixnu.com/building)
- [Log capture](https://unixnu.com/porting/log-capture) and
  [porting records](https://unixnu.com/porting/records)

## Layout

| Path | Contents |
| --- | --- |
| `board.toml`, `configs/`, `feeds.toml` | Board description and source baseline |
| `scripts/`, `patches/` | ADT generation, kernel and GPU library builds, platform/diagnostic patches |
| `package/` | PMGR, UFS/SMMU, persistent Data, display, diagnostics, ST FTS/HID |
| `package/umi-adreno/` | Adreno 650 driver, GMU/SMMU kits and the board bring-up |
| `overlay/`, `menu/` | Firmware/menu staging sources used by the development builder |
| `bringup/tools/`, `bringup/tests/` | Host build, decoding and fault-injection tools |
| `docs/` | Build notes, touch audit and validation summary |

The board geometry and memory window were measured on one handset. Do not reuse
them on a different model or RAM/storage variant without revalidation.

## Credits and license

Based on [UniXNU](https://github.com/UniXNU/UniXNU),
[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng), Mu-Silicium and the public
Xiaomi/LineageOS SM8250 hardware references. Existing per-file notices and
`upstream.json` provenance remain in place. See [NOTICE.md](NOTICE.md) and
[LICENSE](LICENSE). Maintained by [reAAAq](https://github.com/reAAAq), independently
of the UniXNU organization.

### Project Credits & Provenance
- **UniXNU Base:** [UniXNU](https://github.com/UniXNU/UniXNU)
- **Upstream Mi 10 Port & Attempt 80 Touch Baseline:** [reAAAq](https://github.com/reAAAq) ([UniXNU-umi](https://github.com/reAAAq/UniXNU-umi))
- **Standalone ST FTS521 Touch Integration, GPU, Display and Storage Bring-up & Hardware Verification:** [omertrans678](https://github.com/omertrans678) ([UniXNU-umi](https://github.com/omertrans678/UniXNU-umi))
