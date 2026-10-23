#!/usr/bin/env python3
"""
build_unixnu_kernel.py
Compiles the native STMicro FTS521 touchscreen driver and integrates it cleanly into
UniXNU kernel.macho and prepares unixnu_run_payload.bin.
"""
import os
import struct
import subprocess
import sys
from pathlib import Path

BASE_DIR = Path("C:/MyData/Umi/UniXNU-umi")
WSL_BASE = "/mnt/c/MyData/Umi/UniXNU-umi"
KERNEL_IN = Path("C:/MyData/Umi/UniXNU-umi/package/base_kernel.macho")
KERNEL_OUT = Path("C:/MyData/Umi/UniXNU-umi/build/kernel.macho")
PAYLOAD_OUT = Path("C:/MyData/Umi/UniXNU-umi/unixnu_run_payload.bin")
ROOT_APFS = Path("C:/MyData/Umi/root.apfs")

def run(cmd):
    print(f">> {cmd}")
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAILED: {res.stderr}")
        sys.exit(1)
    if res.stdout.strip():
        print(res.stdout.strip())
    return res.stdout

def main():
    print("=== 1. Compiling native UniXNU STMicro FTS521 Touch Driver ===")
    run(f"wsl clang --target=aarch64-linux-gnu -Os -fno-stack-protector -fno-builtin -mgeneral-regs-only -c {WSL_BASE}/package/touch_umi.c -o /tmp/touch_umi.o")

    print("=== 2. Linking at 0xfffffff0074c2770 ===")
    KERNEL_OUT.parent.mkdir(parents=True, exist_ok=True)
    ld_script = """SECTIONS
{
  . = 0xfffffff0074c2770;
  .text : {
    *(.text.umi_touch_thread)
    *(.text*)
    *(.rodata*)
  }
  /DISCARD/ : {
    *(.eh_frame*)
    *(.comment*)
    *(.note*)
    *(.bss*)
  }
}"""
    Path("C:/MyData/Umi/UniXNU-umi/build/touch.ld").write_text(ld_script)
    run("wsl ld.lld -T /mnt/c/MyData/Umi/UniXNU-umi/build/touch.ld /tmp/touch_umi.o -o /tmp/touch_umi.elf")

    print("=== 3. Extracting binary ===")
    run("wsl llvm-objcopy -O binary --only-section=.text /tmp/touch_umi.elf /tmp/touch_umi.bin")
    bin_size = int(run("wsl stat -c%s /tmp/touch_umi.bin").strip())
    print(f"Touch driver binary size: {bin_size} bytes (limit 6288)")
    assert bin_size <= 6288, f"Binary too large: {bin_size} > 6288"

    print("=== 4. Patching UniXNU kernel.macho ===")
    # Read original UniXNU base kernel (15,623,056 bytes)
    src_kernel = KERNEL_IN
    k = bytearray(src_kernel.read_bytes())

    # 4a. Patch redwood_goodix_init branch at file offset 0x4be728 (va 0xfffffff0074c2728) to nop
    k[0x4be728:0x4be72c] = bytes.fromhex("1f2003d5")
    print("Patched _redwood_goodix_init branch at file offset 0x4be728 -> nop")

    # 4b. Write touch driver binary at file offset 0x4be770 (va 0xfffffff0074c2770)
    drv_bin = Path("C:/MyData/Umi/UniXNU-umi/build/touch_umi.bin")
    run("wsl cp /tmp/touch_umi.bin /mnt/c/MyData/Umi/UniXNU-umi/build/touch_umi.bin")
    drv_data = drv_bin.read_bytes()
    k[0x4be770:0x4be770 + len(drv_data)] = drv_data
    print(f"Wrote {len(drv_data)} bytes of STMicro FTS521 driver at file offset 0x4be770 (va 0xfffffff0074c2770)")

    KERNEL_OUT.parent.mkdir(parents=True, exist_ok=True)
    KERNEL_OUT.write_bytes(k)
    print(f"Kernel successfully written to {KERNEL_OUT} ({len(k)} bytes)")

    print("=== 5. Packaging unixnu_run_payload.bin ===")
    mkpayload = BASE_DIR / "scripts/mkpayload.py"
    cmdline = "debug=0x8 bootcpu=7 cpus=8 serial=3 rd=md0 loader_return_after=900 epoch=1792750775 sb_delay=8 gpu=1 adreno_stream1=0 touchscreen=1 cpufreq=1"
    run(f'python "{mkpayload}" -o "{PAYLOAD_OUT}" kernel="{KERNEL_OUT}" ramdisk="{ROOT_APFS}" --cmdline "{cmdline}"')
    print("=== BUILD COMPLETE ===")

if __name__ == "__main__":
    main()
