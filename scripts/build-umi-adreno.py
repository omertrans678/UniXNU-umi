#!/usr/bin/env python3
"""Build libUmiAdreno.a using the exact kernel compiler flags from boot-progress."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get("UMI_XNU_TREE", "/mnt/c/MyData/Umi2/iOS/umi-work/out/umi-xnu/umi/tree"))
UNIXNU = Path("/mnt/c/MyData/Umi2/iOS/UniXNU")
UNIXNU_UMI = Path("/mnt/c/MyData/Umi2/iOS/UniXNU-umi")
ADRENO_PKG = UNIXNU_UMI / "package/umi-adreno"
# Kits as rui12366B/UniXNU-gts7xlwifi carries them for its SM8250 Adreno 650 driver (drivers/Adreno650 = its copy).
KITS = ADRENO_PKG / "kits"
FW_DIR = Path("/mnt/c/MyData/Umi2/iOS/umi-work/out/umi/firmware")
OUT = ROOT / "build/umi-adreno"
OUT.mkdir(parents=True, exist_ok=True)

cc_json = ROOT / "build/boot-progress/compile-commands.json"
if not cc_json.exists():
    raise SystemExit(f"Missing {cc_json}; build boot-progress first")
base_cmd = json.loads(cc_json.read_text())["BootProgress"]

# Extract flags (drop compiler, -xc, src, -o, obj)
flags = []
i = 1
while i < len(base_cmd):
    tok = base_cmd[i]
    if tok in ("-xc", "-xc++"):
        i += 2  # skip source file too
        continue
    if tok == "-o":
        i += 2
        continue
    flags.append(tok)
    i += 1

inc_flags = [
    "-I", str(KITS / "ArmSmmu4K"),
    "-I", str(KITS / "AdrenoFirmware"),
    "-I", str(KITS / "AdrenoPM4"),
    "-I", str(KITS / "pipa"),
    "-I", str(ADRENO_PKG / "Adreno650"),
    "-I", str(ADRENO_PKG / "AdrenoMetal"),
    "-I", str(ADRENO_PKG),
    "-I", str(ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT"),
    "-I", str(ROOT / ".cache/xnu-work/iokit"),
    "-I", str(ROOT / ".cache/xnu-work/osfmk"),
    "-I", str(ROOT / ".cache/xnu-work/bsd"),
    "-I", str(ROOT / ".cache/xnu-work/libkern"),
    "-I", str(ROOT / ".cache/xnu-work/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/EXPORT_HDRS/iokit"),
    "-I", str(ROOT / ".cache/xnu-work/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/EXPORT_HDRS/osfmk"),
    "-I", str(ROOT / ".cache/xnu-work/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/EXPORT_HDRS/bsd"),
    "-I", str(ROOT / ".cache/xnu-work/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/EXPORT_HDRS/libkern"),
    "-I", str(ROOT / ".cache/xnu-work/EXTERNAL_HEADERS"),
    "-D__KERNEL__", "-DKERNEL", "-DKERNEL_PRIVATE", "-DAPPLE",
]

# Generate UmiA650Firmware.c
fw_c = ADRENO_PKG / "UmiA650Firmware.c"
def emit_array(path: Path, name: str) -> str:
    data = path.read_bytes()
    lines = [f"const uint8_t {name}[] = {{"]
    for off in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[off:off+16]) + ",")
    lines.append("};")
    lines.append(f"const uint32_t {name}_size = {len(data)}u;")
    return "\n".join(lines) + "\n"

fw_c.write_text(
    "#include <stdint.h>\n" +
    emit_array(FW_DIR / "a650_gmu.bin", "native_gpu_gmu_firmware") +
    emit_array(FW_DIR / "a650_sqe.fw", "native_gpu_sqe_firmware") +
    emit_array(FW_DIR / "a650_zap.elf", "native_gpu_zap_firmware")
)

c_sources = sorted((KITS / "ArmSmmu4K").glob("ArmSmmu4K*.c")) + [
    KITS / "AdrenoFirmware/AdrenoFirmware.c",
    KITS / "AdrenoFirmware/AdrenoHfi.c",
    KITS / "AdrenoFirmware/AdrenoHfiStartup.c",
    KITS / "AdrenoFirmware/AdrenoHfiVotes.c",
    KITS / "AdrenoPM4/AdrenoPM4.c",
    KITS / "pipa/A650Gmu.c",
    fw_c,
]

cpp_sources = [
    ADRENO_PKG / "Adreno650/NativeAdrenoGpuContext.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoSmmuContext.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoSmmuMemory.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoMemory.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoCpu.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoSha256.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoVm.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoRing.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoSubmit.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoTimer.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoUserClient.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoPublish.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoCpInit.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoIrq.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoPreempt.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoGmuContext.cpp",
    ADRENO_PKG / "Adreno650/NativeAdrenoGmuMemory.cpp",
    ADRENO_PKG / "UmiAdrenoBringup.cpp",
]

def make_cpp_cmd(src: Path, obj: Path, extra: list[str] = None) -> list[str]:
    cmd = [base_cmd[0], "-xc++", str(src), "-o", str(obj)] + flags + inc_flags
    cpp_flags = []
    for t in cmd:
        if t in ("-DMACH_KERNEL_PRIVATE", "-DMACH_KERNEL") or t.startswith("-std=") or t == "-Werror":
            continue
        cpp_flags.append(t)
    filtered = []
    j = 0
    while j < len(cpp_flags):
        if cpp_flags[j] == "-D" and j + 1 < len(cpp_flags) and cpp_flags[j + 1] in ("MACH_KERNEL_PRIVATE", "MACH_KERNEL"):
            j += 2
            continue
        filtered.append(cpp_flags[j])
        j += 1
    return filtered + ["-std=gnu++20", "-fno-exceptions", "-fno-rtti"] + (extra or [])

# Clean old objects in OUT
for old_o in OUT.glob("*.o"):
    old_o.unlink(missing_ok=True)

objs = []
workdir = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT"
for src in c_sources + cpp_sources:
    obj = OUT / (src.stem + ".o")
    is_cpp = src.suffix == ".cpp"
    if is_cpp:
        cmd = make_cpp_cmd(src, obj)
    else:
        cmd = [t for t in ([base_cmd[0], "-xc", str(src), "-o", str(obj)] + flags + inc_flags) if t != "-Werror"]
    subprocess.run(cmd, cwd=workdir, check=True)
    objs.append(obj)

# Build staged AGXAccelerator (NativeAdrenoAccelerator.late.o) + IOAcceleratorES original-bindings.o
accel_src = ADRENO_PKG / "Adreno650/NativeAdrenoAccelerator.cpp"
accel_s = OUT / "NativeAdrenoAccelerator.s"
accel_late = OUT / "NativeAdrenoAccelerator.late.o"
accel_cmd = make_cpp_cmd(accel_src, accel_s, [
    "-S",
    "-DNA_AC_STAGE_CLASS=1",
    "-DNA_AC_ORIGINAL_VPTR=0xfffffff005d439a0ULL",
])
subprocess.run(accel_cmd, cwd=workdir, check=True)
s_lines = accel_s.read_text().splitlines()
out_s_lines = []
in_mod_init = False
for line in s_lines:
    stripped = line.strip()
    if stripped.startswith(".section") and "__mod_init_func" in stripped:
        in_mod_init = True
        continue
    if in_mod_init:
        if stripped.startswith(".section") or stripped.startswith(".subsections_via_symbols"):
            in_mod_init = False
        else:
            continue
    if stripped.startswith("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp:"):
        out_s_lines.append("\t.globl\t__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp")
    out_s_lines.append(line)
accel_s.write_text("\n".join(out_s_lines) + "\n")
subprocess.run(
    ["/usr/lib/llvm-18/bin/clang", "-target", "arm64-apple-ios17.0", "-c", str(accel_s), "-o", str(accel_late)],
    check=True,
)
objs.append(accel_late)

bindings_s = OUT / "original-bindings.S"
bindings_o = OUT / "original-bindings.o"
bindings_s.write_text(
    "// Generated by build-umi-adreno.py for IOAcceleratorES in 21H461 IOGPUFamily\n"
    ".text\n"
    ".globl __ZN15IOAcceleratorESC2EPK11OSMetaClass\n"
    ".set __ZN15IOAcceleratorESC2EPK11OSMetaClass, 0xfffffff004bd3b20\n"
    ".globl __ZN15IOAcceleratorESD2Ev\n"
    ".set __ZN15IOAcceleratorESD2Ev, 0xfffffff004bd3b64\n"
    ".globl __ZN15IOAcceleratorES10gMetaClassE\n"
    ".set __ZN15IOAcceleratorES10gMetaClassE, 0xfffffff006aeb688\n"
)
subprocess.run(
    ["/usr/lib/llvm-18/bin/clang", "-target", "arm64-apple-ios17.0", "-c", str(bindings_s), "-o", str(bindings_o)],
    check=True,
)
objs.append(bindings_o)

addon_s = ADRENO_PKG / "UmiGpuAddon.S"
if addon_s.exists():
    addon_o = OUT / "UmiGpuAddon.o"
    subprocess.run(
        ["/usr/lib/llvm-18/bin/clang", "-target", "arm64-apple-ios17.0", "-c", str(addon_s), "-o", str(addon_o)],
        check=True,
    )
    objs.append(addon_o)

archive = OUT / "libUmiAdreno.a"
archive.unlink(missing_ok=True)
subprocess.run(["/usr/lib/llvm-18/bin/llvm-ar", "rcs", str(archive), *[str(o) for o in objs]], check=True)
print(f"Built {archive} ({archive.stat().st_size} bytes, {len(objs)} objects)")
