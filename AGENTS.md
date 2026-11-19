# AGENTS.md — UniXNU-umi (Xiaomi Mi 10)

Board scope, status, prerequisites and build commands are in [README.md](README.md).
Workspace setup and pins are in [docs/BUILD.md](docs/BUILD.md).

## Required reading

- UniXNU: [port a device](https://unixnu.com/porting/steps),
  [device repository](https://unixnu.com/porting/device-repository),
  [porting records](https://unixnu.com/porting/records)
- Drivers: [UFS](https://unixnu.com/porting/ufs), [display](https://unixnu.com/porting/display),
  [input](https://unixnu.com/porting/input), [GPU and Metal](https://unixnu.com/porting/gpu)
- Userland: [Data volume](https://unixnu.com/guide/userdata#data-volume),
  [Skip Setup](https://unixnu.com/guide/skip-setup), [Building](https://unixnu.com/building),
  [log capture](https://unixnu.com/porting/log-capture)

Use the UniXNU repository and the unixnu.com pages as the primary sources.

## Rules

- Build the phone image with `python3 tools/build_umi_native_ufs.py --profile phone && python3 tools/build_stage10_pipeline.py`
  in the build workspace. Do not run `build_umi_uefi.py` on its own.
- Flash only after a `SUCCESS: Stage 10 Boot Image` line and a new image timestamp.
- Kernel arguments (`package/unixnu-xnu-prelink.toml`) stay under about 960 characters;
  `umi-ar` (auto-reset to recovery) stays set. Bump `umi-build-tag` for every flashed build.
- The touch library is not rebuilt by the image build: run the touch checks and
  `build_umi_touch.py` after changing `package/touch/`.
- Do not commit vendor or Apple binaries; `package/umi-adreno/UmiA650Firmware.c` is generated and git-ignored.
- Record host results and device results separately; read the boot log rows
  (`GPU:`, `UFS*`, `SYSPROF`, `PANIC`) before changing a driver.
- Board geometry and the memory window were measured on one handset; revalidate
  them on any other model or RAM/storage variant.
