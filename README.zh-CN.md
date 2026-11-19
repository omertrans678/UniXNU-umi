# UniXNU-umi — 小米 10

[English](README.md) | 简体中文

面向 **小米 10（`umi` / SM8250 / Adreno 650）** 的独立 UniXNU 移植。已在一台
**12 GiB 内存 / 256 GB 存储**的手机上启动 **iPadOS 17.7.11（21H461）**，跳过设置
后进入 **SpringBoard 主屏幕**，并具备 GPU 加速的 Metal、显示直接扫描输出和可用的
触摸。本仓库发布开发源码，不提供可直接安装的发行镜像。

| 仓库 | 用途 |
| --- | --- |
| [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | 板级配置、XNU 接入、驱动、诊断和主机工具 |
| [uefi-umi](https://github.com/reAAAq/uefi-umi) | Mu-Silicium 固件移植，派生自 uefi-sheng |
| [UniXNU](https://github.com/UniXNU/UniXNU) | 上游构建驱动、加载器、通用驱动和补丁 |

## 范围与状态

以下为测试机在构建 `umi-192`（2026-11-19）时的状态。

| 功能 | 状态 | 证据 / 限制 |
| --- | --- | --- |
| UEFI 菜单和 RAM 载荷 | 可用 | 菜单、文件校验和 DarwinBoot 交接 |
| XNU 与 SMP | 可用 | 八个 CPU；大核集群提升到最高档（`umi-cpufreq=4`），尚无 DCVS |
| UFS / OS / SystemOS | 可用 | 根卷和 cryptex 已挂载；UTRD 槽 2..16 并发读取 |
| 可写 Data | 可用 | 带预读的持久存储；`umi-ar` 复位前同步文件系统 |
| SpringBoard | 可用 | 跳过设置（`umi-skip-setup=1`）；系统内存窗口 4 GiB |
| GPU / Metal | 可用 | Adreno 650 驱动（`package/umi-adreno`），页表模式 2，已验证挂起恢复 |
| 显示 | 可用 | DPU 直接扫描输出（`dpu-scanout=2`），BGRA8 合成（ADT 色域 sRGB） |
| ST FTS 触摸 | 可用 | 解锁、点击和手势；未知事件被跳过，不再停止驱动 |
| 双环 GPU 抢占 | 关闭 | 切换可用，但动画中出现黑色方块（未设置 `gpu-preempt`） |
| 着色器缓存预热 | 未完成 | 需要 macOS |
| Wi-Fi、蓝牙、蜂窝、音频、相机、传感器 | 未实现 | |
| XNU 下的 USB、电池、休眠/唤醒 | 未验证 | Recovery ADB 与 XNU USB 支持无关 |

已知问题：lsd 有时会在启动时重建 LaunchServices 存储，该次启动会停在 Apple
标志；应用启动仍比供体设备慢。[验证摘要](docs/validation.json) 保存部分不含身份
信息的测量值。原始设备转储和日志仅保存在本地。

## 前置条件

- Linux 或 WSL2 主机，并安装 [Linux 工具链](https://unixnu.com/guide/requirements#linux)；
  Data 种子工具和着色器缓存需要 macOS。
- 供体输入：**iPad7,1 / iPadOS 17.7.11 / 21H461 / j120ap**，源码内核
  **XNU 10063.141.1 / Darwin 23.6.0**（`ipsw-inputs.json`）。本仓库不含 Apple 或厂商
  二进制输入；生成的 GPU 固件源文件已被 git 忽略。
- 装有 TWRP Recovery 并已备份所有分区的设备。安装会替换 Android userdata；
  此处没有通用安装器。
- 固定的源码版本：`source-lock.json`、`feeds.toml`、`unixnu.lock`。

## 构建

工作区准备、版本固定和主机检查见 [BUILD.md](docs/BUILD.md)。手机配置由
`bringup/` 下的工具组装：

```bash
python3 tools/build_umi_native_ufs.py --profile phone && python3 tools/build_stage10_pipeline.py
```

在构建工作区（`umi-work/`，其中 `devices/umi` 指向本仓库）中运行。输出为
`out/umi/boot/umi-xnu-stage10-systemos-boot.img`；只有出现
`SUCCESS: Stage 10 Boot Image` 后才在 TWRP 中写入 `boot`。单独构建 GPU 库使用
`scripts/build-umi-adreno.py`。内核参数位于 `package/unixnu-xnu-prelink.toml`，
长度须保持在约 960 个字符以内；保留 `umi-ar`（自动复位到 Recovery）。

## 必读文档

- [移植设备](https://unixnu.com/porting/steps) 和
  [设备仓库](https://unixnu.com/porting/device-repository)
- [UFS 存储](https://unixnu.com/porting/ufs)、[显示](https://unixnu.com/porting/display)、
  [输入](https://unixnu.com/porting/input)、[GPU 与 Metal](https://unixnu.com/porting/gpu)
- [Data 卷](https://unixnu.com/guide/userdata#data-volume)、
  [跳过设置](https://unixnu.com/guide/skip-setup)、[构建](https://unixnu.com/building)
- [日志采集](https://unixnu.com/porting/log-capture) 和
  [移植记录](https://unixnu.com/porting/records)

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `board.toml`、`configs/`、`feeds.toml` | 板级描述和源码基线 |
| `scripts/`、`patches/` | ADT 生成、内核和 GPU 库构建、平台/诊断补丁 |
| `package/` | PMGR、UFS/SMMU、持久 Data、显示、诊断、ST FTS/HID |
| `package/umi-adreno/` | Adreno 650 驱动、GMU/SMMU 组件和板级启动 |
| `overlay/`、`menu/` | 开发构建器使用的固件/菜单暂存源 |
| `bringup/tools/`、`bringup/tests/` | 主机构建、解码和故障注入工具 |
| `docs/` | 构建说明、触摸审计和验证摘要 |

板级几何参数和内存窗口仅在一台手机上测得。用于其他型号或其他内存/存储版本前，
必须重新验证。

## 致谢与许可

基于 [UniXNU](https://github.com/UniXNU/UniXNU)、
[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng)、Mu-Silicium 以及公开的
小米/LineageOS SM8250 硬件资料。原有的逐文件声明和 `upstream.json` 来源信息保持
不变。见 [NOTICE.md](NOTICE.md) 和 [LICENSE](LICENSE)。由
[reAAAq](https://github.com/reAAAq) 维护，独立于 UniXNU 组织。

### 项目致谢与来源
- **UniXNU 基础：** [UniXNU](https://github.com/UniXNU/UniXNU)
- **上游小米 10 移植与第 80 版触摸基线：** [reAAAq](https://github.com/reAAAq)（[UniXNU-umi](https://github.com/reAAAq/UniXNU-umi)）
- **独立 ST FTS521 触摸集成、GPU、显示与存储启动及硬件验证：** [omertrans678](https://github.com/omertrans678)（[UniXNU-umi](https://github.com/omertrans678/UniXNU-umi)）
