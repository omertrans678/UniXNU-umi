# UniXNU-umi — Xiaomi Mi 10

English | [简体中文](README.zh-CN.md)

An independent UniXNU device port for **Xiaomi Mi 10 (`umi`, SM8250)**. The tested
12 GiB / 256 GB handset reaches **iPadOS 17.7.11 Hello / Setup Assistant**.
Physical ST FTS touch under XNU was confirmed working by the device owner in
attempt 80. Setup completion and the home screen remain unverified. This is a
development source snapshot, not a ready-to-install release.

| Repository | Role |
| --- | --- |
| [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | Board, XNU integration, drivers, diagnostics and host tools |
| [uefi-umi](https://github.com/reAAAq/uefi-umi) | Mu-Silicium firmware port, derived from uefi-sheng |
| [UniXNU](https://github.com/UniXNU/UniXNU) | Upstream build driver, loader, generic drivers and patches |

## Hardware status

Status reflects the tested handset through attempt 80 (2026-10-23).

| Feature | State | Evidence / limitation |
| --- | --- | --- |
| UEFI menu and RAM payload | Working | Menu, file verification and DarwinBoot handoff |
| Volume selection / power confirmation | Working in UEFI | Restart action also verified; not an iOS button claim |
| XNU and SMP | Working | Eight CPUs online; per-core timer and IPI activity recorded |
| UFS / OS / SystemOS | Working for startup | Root and cryptex mounted; launchd and system services run |
| Writable Data | Working for startup | Persistent store across reboots on the test partition layout |
| Display | Working | Apple logo and Hello/Setup via CPU copies to inherited scanout |
| ST FTS touch | Working under XNU | Device owner confirmed usable physical touch in attempt 80; UEFI menu touch is not qualified |
| GPU acceleration | Not implemented | CPU presentation only |
| Wi-Fi, Bluetooth, cellular, audio, camera, sensors | Not implemented / not qualified | No complete driver validation |
| USB under XNU, battery, suspend/resume | Not qualified | Recovery ADB is separate from XNU USB support |

[Validation summary](docs/validation.json) contains selected non-identifying
measurements and artifact hashes. Original device dumps and console logs remain
local. Attempt 80 enables the native always-on PM8150B GPIO5 board rail before
FTS startup, after the preceding run recorded the pin output-low. The owner
confirmed working touch after this change. The runtime coordinate/HID counts
for attempt 80 have not yet been collected; prior zero-event traces are historical.

## Sources and build

Follow [BUILD.md](docs/BUILD.md) for pins, workspace setup, host checks and the
remaining build-input requirements. `source-lock.json` pins the tested framework
and firmware, and `feeds.toml` / `unixnu.lock` pin public source dependencies.
The default `board.toml` and `umi_defconfig` remain the minimal source-only
baseline. The full phone profile is assembled by the tools under `bringup/`.
A plain generic build is not equivalent to the tested Hello image.

Donor: **iPad7,1 / iPadOS 17.7.11 / 21H461 / j120ap**. Source kernel:
**XNU 10063.141.1 / Darwin 23.6.0**. The donor's binary kernel is a different
version, recorded in `ipsw-inputs.json`. No Apple or vendor binary inputs are
included in this repository.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml`, `configs/`, `feeds.toml` | Board description and source baseline |
| `scripts/`, `patches/` | ADT generation, kernel builds and platform/diagnostic patches |
| `package/` | PMGR service, UFS/SMMU, persistent Data, display, diagnostics and ST FTS/HID |
| `overlay/`, `menu/` | Firmware/menu staging sources used by the development builder |
| `bringup/tools/`, `bringup/tests/` | Host build, decoding and fault-injection tools |
| `docs/` | Build limitations and current hardware observation summary |

The UEFI repository contains the staged inherited-display/buttons/RAM-payload
profile. `overlay/` is the input template used to regenerate that profile; they
are deliberately not byte-identical. Firmware staging may update the UEFI tree.

## Current work

The ST FTM5 path uses native GPI DMA I²C, the three-reset startup sequence,
read-only factory identity acquisition and HID confidence/collection dispatch.
The board-boost fix is in [138d6cb](https://github.com/reAAAq/UniXNU-umi/commit/138d6cb).
[Native driver audit](docs/TOUCH_NATIVE_AUDIT.md) records the source comparison
and test history. Multi-touch, suspend/resume and completing setup still need
separate validation; working XNU touch does not qualify UEFI touch.

The board geometry and high-memory window were measured on one handset. Do not
reuse them on a different model or RAM/storage variant without revalidation.
The tested installation replaces Android userdata; there is no general installer
or partition-write tool in this repository. Preserve Recovery and backups.

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
- **Standalone ST FTS521 Touch Integration & Hardware Verification:** [omertrans678](https://github.com/omertrans678) ([UniXNU-umi](https://github.com/omertrans678/UniXNU-umi))
