#!/usr/bin/env python3
"""Stage 10 Build & Provisioning Pipeline: Genuine Apple 21H461 SystemOS Cryptex Mount + dyld_shared_cache_arm64.
Integrates:
- Recompiled XNU core (kern_exec.o, bsd_init.o, ubc_subr.o, IOKitBSDInit.cpo, vfs_conf.o) with Stage 10 definitions:
  * XNU_OWNER_CACHE_MOUNT=1, XNU_OWNER_CACHE_STARTUP=1, XNU_OWNER_BOOT_MANIFEST=1, XNU_OWNER_UNION_SYSTEMOS=1
  * Setting commpage dyld_flags bit 3 (dyld_flags |= 8)
- Linked & Normalized Mach-O kernel (Pass A & Pass B rebasing with all hardware drivers: UFS, PMGR, Display, Touch, AES, VirtIO, VerifiedRoot)
- Prelinked kernel with 30 Apple 21H461 kexts (apfs.kext, AppleImage4, AMFI, Sandbox, IOStorageFamily, etc.)
- userdata-root.img assembling: SHNGROOT manifest (1 MiB) + OS Window (4.9 GB) + SystemOS Window (3.15 GB)
- Fastboot RAM boot image packaging (umi-xnu-stage10-systemos-boot.img)
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time

os.environ["PATH"] = "/home/omer/.local/bin:" + os.environ.get("PATH", "")

ROOT = Path("/mnt/c/MyData/Umi2/iOS/umi-work")
TREE = ROOT / "out/umi-xnu/umi/tree"
WORK = TREE / ".cache/xnu-work"
OBJ = TREE / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
KERNEL_DIR = ROOT / "out/umi-xnu/umi/kernel"
IPSW_DIR = ROOT / "out/umi-xnu/ipsw/21H461"
STAGE10_DIR = ROOT / "out/umi/stage10"
USERDATA_DIR = ROOT / "out/umi/userdata"

STAGE10_DIR.mkdir(parents=True, exist_ok=True)
USERDATA_DIR.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(ROOT / "sources/UniXNU/kits/NativePrelink"))
from native_prelink import macho, backed_offset


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while chunk := f.read(16 * 1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


STAGE10_DEFS = [
    "-DXNU_OWNER_CACHE_MOUNT=1",
    "-DXNU_OWNER_CACHE_STARTUP=1",
    "-DXNU_OWNER_BOOT_MANIFEST=1",
    "-DXNU_OWNER_UNION_SYSTEMOS=1",
    "-DXNU_NATIVE_CS_HASH_CACHE=1",
    "-DXNU_OWNER_ROOT_AUDIT=1",
    "-DXNU_OWNER_ROOT_TRUST=1",
    "-DXNU_TRUST_CACHE_RUNTIME=1",
    "-DXNU_NATIVE_AES=1",
    "-DXNU_NATIVE_VIRTIO_BLOCK=1",
    "-DXNU_VERIFIED_ROOT_PROBE=1",
    "-DXNU_NATIVE_LEGACY_DISPLAY=1",
    "-DUMI_FULL_BOOT_PROGRESS=1",
    "-DUMI_EARLY_TRACE=1",
    "-DXNU_NATIVE_UFS=1",
    "-DUMI_PMGR=1",
]

PREFIXES = (
    "XNU_OWNER_", "XNU_NATIVE_CS", "XNU_VERIFIED_ROOT", "XNU_NATIVE_LEGACY",
    "XNU_NATIVE_AES", "XNU_NATIVE_VIRTIO", "XNU_TRUST_CACHE", "XNU_NATIVE_UFS",
    "XNU_NATIVE_TRUST_CACHE", "MOCKFS"
)


def build_boot_progress():
    print("  Building libBootProgress.a with Stage 10 provider set...")
    (WORK / "osfmk/arm64/umi_ufs_config.h").write_text("#define UMI_UFS_PROBE 0\n")
    (WORK / "osfmk/arm64/umi_platform_power_config.h").write_text("#define UMI_PMGR 1\n")
    (WORK / "osfmk/arm64/umi_early_trace_config.h").write_text("#define UMI_EARLY_TRACE 1\n")
    env = dict(os.environ)
    env.update({
        "UMI_FULL_BOOT_PROGRESS": "1",
        "UMI_EARLY_TRACE": "1",
        "XNU_NATIVE_UFS": "1",
        "XNU_NATIVE_LEGACY_DISPLAY": "1",
        "UMI_FTS_TOUCH": "1",
    })
    subprocess.run(
        [sys.executable, str(TREE / "scripts/build-boot-progress.py")],
        cwd=TREE, env=env, check=True
    )


def compile_object(rel: str, extra_defs: list[str] = None):
    target = OBJ / rel
    rec = json.loads((OBJ / (rel + ".json")).read_text().strip().rstrip(","))
    args = list(rec["arguments"])
    out_args = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o" and i + 1 < len(args):
            out_args += ["-o", str(target)]
            i += 2
            continue
        if a == "-D" and i + 1 < len(args) and any(args[i + 1].startswith(p) for p in PREFIXES):
            i += 2
            continue
        if any(a.startswith("-D" + p) for p in PREFIXES) or any(a.startswith("-U" + p) for p in PREFIXES):
            i += 1
            continue
        out_args.append(a)
        i += 1
    if extra_defs:
        out_args.extend(extra_defs)
    print(f"  CC {rel}...")
    subprocess.run(out_args, cwd=rec["directory"], check=True)


def build_iig_stubs() -> Path:
    stubs_s = OBJ / "iig_prelink_stubs.s"
    stubs_o = OBJ / "iig_prelink_stubs.o"
    syms = [
        "__ZN12IOUserClient8DispatchE5IORPC",
        "__ZN8OSObject8DispatchE5IORPC",
        "__ZN9IOService9_DispatchEPS_5IORPC",
        "__ZN12IODMACommand8DispatchE5IORPC",
        "__ZN9IOService4StopEPS_PFiP15OSMetaClassBase5IORPCE",
        "__ZN9IOService12Start_InvokeE5IORPCP15OSMetaClassBasePFiS2_PS_E",
        "__ZN24IOBufferMemoryDescriptor8DispatchE5IORPC",
        "__ZN9IOService5StartEPS_PFiP15OSMetaClassBase5IORPCE",
        "__ZN8OSAction9_DispatchEPS_5IORPC",
        "__ZN18IOMemoryDescriptor8DispatchE5IORPC",
        "__ZN9IOService20SetProperties_InvokeE5IORPCP15OSMetaClassBasePFiS2_P12OSDictionaryE",
        "__ZN9IOService11Stop_InvokeE5IORPCP15OSMetaClassBasePFiS2_PS_E",
    ]
    lines = [".text", ".align 2"]
    for idx, s in enumerate(syms):
        lines.append(f".globl {s}")
        lines.append(f"{s}:")
        lines.append(f"  mov w0, #{0x2c7 + idx}")
        lines.append("  movk w0, #0xe000, lsl #16")
        lines.append("  ret")
    stubs_s.write_text("\n".join(lines) + "\n")
    subprocess.run(
        ["/usr/lib/llvm-18/bin/clang", "-target", "arm64-apple-ios17.0", "-c", str(stubs_s), "-o", str(stubs_o)],
        check=True,
    )
    return stubs_o


def make_rebased_archive(archive_path: Path, prefix_hex: str, tag: str, bindings_s: Path = None) -> str:
    build_dir = archive_path.parent
    if bindings_s is None:
        bindings_files = list(build_dir.glob("*bindings*.S"))
        if not bindings_files:
            return f"-Wl,-force_load,{archive_path}"
        bindings_s = bindings_files[0]

    tmp_dir = Path(f"/tmp/ar_{archive_path.stem}_{tag}")
    if tmp_dir.exists():
        shutil.rmtree(tmp_dir)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run(["/usr/lib/llvm-18/bin/llvm-ar", "x", str(archive_path)], cwd=tmp_dir, check=True)

    rebased_s = tmp_dir / bindings_s.name
    text = bindings_s.read_text().replace("0xfffffff0", prefix_hex)
    rebased_s.write_text(text)

    rebased_o = tmp_dir / (bindings_s.stem + ".o")
    if not rebased_o.exists():
        for f in tmp_dir.glob("*bindings*.o"):
            rebased_o = f
            break

    subprocess.run(
        ["/usr/lib/llvm-18/bin/clang", "-target", "arm64-apple-ios17.0", "-c", str(rebased_s), "-o", str(rebased_o)],
        check=True,
    )

    rebased_a = Path(f"/tmp/{archive_path.stem}-{tag}.a")
    rebased_a.unlink(missing_ok=True)
    objs = sorted(tmp_dir.glob("*.o"))
    subprocess.run(["/usr/lib/llvm-18/bin/llvm-ar", "rcs", str(rebased_a), *[str(p) for p in objs]], check=True)
    return f"-Wl,-force_load,{rebased_a}"


def link_stage10(rebased_specs: list, direct_archives: list, out_path: Path):
    stubs_o = str(build_iig_stubs())
    ldflags = shlex.split((OBJ / ".LDFLAGS").read_text())

    crypto_a = str(TREE / "build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a")
    armdt_a = str(TREE / "build/armdt-platform/libArmDTPlatform.a")
    armcpu_a = str(TREE / "build/armcpu/libArmCpu.a")
    pthread_a = str(TREE / "build/libpthread-kernel/libpthread_kernel.a")
    boot_progress_a = str(TREE / "build/boot-progress/libBootProgress.a")

    common = [
        stubs_o,
        crypto_a,
        f"-Wl,-force_load,{armdt_a}",
        f"-Wl,-force_load,{armcpu_a}",
        f"-Wl,-force_load,{pthread_a}",
        f"-Wl,-force_load,{boot_progress_a}",
    ]
    for da in direct_archives:
        common.append(f"-Wl,-force_load,{da}")

    tail = [
        "-filelist",
        "link.filelist",
        "lastkerneldataconst.o",
        "lastkernelconstructor.o",
        "nonlto.o",
        "version.o",
        "-lfirehose_kernel",
    ]

    extra_a = [make_rebased_archive(ar, "0x00000000", "a", bs) for ar, bs in rebased_specs]
    extra_b = [make_rebased_archive(ar, "0x7ffffff0", "b", bs) for ar, bs in rebased_specs]

    cmd_a = ldflags + common + extra_a + tail + ["-Wl,-pagezero_size,0x7004000", "-o", "/tmp/kern_a.macho"]
    cmd_b = ldflags + common + extra_b + tail + ["-Wl,-pagezero_size,0x7ffffff007004000", "-o", "/tmp/kern_b.macho"]

    print("  Linking Pass A (/tmp/kern_a.macho)...")
    subprocess.run(cmd_a, cwd=OBJ, check=True)
    print("  Linking Pass B (/tmp/kern_b.macho)...")
    subprocess.run(cmd_b, cwd=OBJ, check=True)

    print("  Normalizing Mach-O kernel...")
    sys.path.insert(0, str(ROOT / "tools"))
    from normalize_kernel_macho import build_normalized
    build_normalized()

    kernel_macho = OBJ / "mach.development.bcm2837.unstripped.noctf"
    shutil.copyfile(kernel_macho, out_path)
    print(f"  SUCCESS: Stage 10 source kernel -> {out_path} ({out_path.stat().st_size} bytes)")


def prelink_stage10(source_kernel: Path, out_prelinked: Path):
    for ext in (".macho", ".json", ".board.toml", ".source-build.json", ".provenance.json"):
        out_prelinked.with_suffix(ext).unlink(missing_ok=True)

    status_path = ROOT / "out/umi-xnu/umi/prelink-source-build-status.json"
    base_status = json.loads((ROOT / "out/umi-xnu/umi/source-build-status.json").read_text()) if (ROOT / "out/umi-xnu/umi/source-build-status.json").exists() else {}

    systemos_inputs = [
        ROOT / "devices/umi/patches/umi-xnu/zzzzzzzzzzzzz-umi-systemos-trace.patch",
        ROOT / "sources/UniXNU/patches/xnu/offline-owner-systemos-mount.patch",
        ROOT / "sources/UniXNU/patches/xnu/offline-owner-boot-manifest.patch",
        ROOT / "sources/UniXNU/patches/xnu/native-4k-signature-hash-cache.patch",
    ]
    systemos_sources = {str(p.relative_to(ROOT)): sha256_file(p) for p in systemos_inputs}

    base_status.update({
        "stage": "kernel",
        "prelinked": False,
        "hardware_tested": False,
        "early_trace": True,
        "profile": "phone",
        "crypto_bootstrap": "prelinked",
        "pmgr": True,
        "gic_diagnostics": True,
        "screen_diagnostics": True,
        "ufs_probe": False,
        "native_ufs": True,
        "owner_root": "trust",
        "trust_cache_runtime": True,
        "native_pan": True,
        "systemos": True,
        "systemos_sources": systemos_sources,
        "data_ramdisk": None,
        "legacy_display": True,
        "fts_touch": True,
        "persistent_data": False,
        "kernel": str(source_kernel),
        "bytes": source_kernel.stat().st_size,
        "sha256": sha256_file(source_kernel),
        "armcpu_build": json.loads((TREE / "build/armcpu/build-report.json").read_text()),
        "screen_build": json.loads((TREE / "build/boot-progress/build-report.json").read_text()),
        "pmgr_build": json.loads((TREE / "build/umi-platform-power/build-report.json").read_text()),
        "native_ufs_build": json.loads((TREE / "build/native-ufs/build-report.json").read_text()),
        "root_policy_build": json.loads((TREE / "build/umi-root-policy/build-report.json").read_text()),
        "storage_providers_build": json.loads((TREE / "build/umi-storage-providers/build-report.json").read_text()),
        "legacy_display_build": json.loads((TREE / "build/native-legacy-display/build-report.json").read_text()),
        "touch_build": json.loads((TREE / "build/umi-touch/build-report.json").read_text()),
        "success": True,
    })
    status_path.write_text(json.dumps(base_status, indent=2) + "\n")

    print("  Running build_umi_prelink.py for Stage 10 with storage & early trace...")
    subprocess.run([
        sys.executable, str(ROOT / "tools/build_umi_prelink.py"),
        "--profile", "phone",
        "--early-trace",
        "--storage",
        "--output", str(out_prelinked),
    ], cwd=ROOT, check=True)
    print(f"  SUCCESS: Stage 10 prelinked kernel -> {out_prelinked} ({out_prelinked.stat().st_size} bytes)")


def assemble_userdata_image(os_dmg: Path, sys_dmg: Path, out_img: Path, out_json: Path):
    """Assemble SHNGROOT manifest and aligned windows for UFS userdata."""
    MIB = 1 << 20
    CHUNK = 16 * MIB

    print("=== Assembling userdata-root.img for Xiaomi Mi 10 UFS ===")
    os_size = os_dmg.stat().st_size
    sys_size = sys_dmg.stat().st_size

    # Layout:
    # 0x000000: Manifest (4096B padded to 1 MiB)
    # 0x100000: OS window
    # offset_sys: SystemOS window (1 MiB aligned)
    os_offset = MIB
    sys_offset = os_offset + ((os_size + MIB - 1) // MIB) * MIB
    total_bytes = sys_offset + ((sys_size + MIB - 1) // MIB) * MIB

    print(f"  OS Window:       offset={os_offset:#x}, size={os_size} bytes ({os_size / (1024**3):.2f} GiB)")
    print(f"  SystemOS Window: offset={sys_offset:#x}, size={sys_size} bytes ({sys_size / (1024**3):.2f} GiB)")
    print(f"  Total Image:     {total_bytes} bytes ({total_bytes / (1024**3):.2f} GiB)")

    entries = []
    # Write image
    with open(out_img, "wb") as target:
        target.truncate(total_bytes)

        # Write OS Window
        print("  Writing OS Window...")
        target.seek(os_offset)
        os_digest = hashlib.sha256()
        with open(os_dmg, "rb") as src:
            while chunk := src.read(CHUNK):
                os_digest.update(chunk)
                target.write(chunk)
        entries.append(("OS", os_offset, os_size, os_digest.digest()))
        print(f"  OS Window SHA256: {os_digest.hexdigest()}")

        # Write SystemOS Window
        print("  Writing SystemOS Window...")
        target.seek(sys_offset)
        sys_digest = hashlib.sha256()
        with open(sys_dmg, "rb") as src:
            while chunk := src.read(CHUNK):
                sys_digest.update(chunk)
                target.write(chunk)
        entries.append(("SystemOS", sys_offset, sys_size, sys_digest.digest()))
        print(f"  SystemOS Window SHA256: {sys_digest.hexdigest()}")

        # Build 4096B SHNGROOT Manifest
        header = bytearray(4096)
        header[0:8] = b"SHNGROOT"
        struct.pack_into("<IIQ", header, 8, 1, len(entries), total_bytes)
        for index, (name, offset, size, digest) in enumerate(entries):
            struct.pack_into("<32sQQII32s40s", header, 24 + 128 * index, name.encode(), offset, size, 512, 1, digest, bytes(40))
        header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()

        target.seek(0)
        target.write(header)

    manifest_hash = hashlib.sha256(bytes(header)).hexdigest()
    image_hash = sha256_file(out_img)
    meta = {
        "image_bytes": total_bytes,
        "image_sha256": image_hash,
        "manifest_sha256": manifest_hash,
        "windows": [
            {"name": n, "offset": o, "size": s, "sha256": d.hex()} for n, o, s, d in entries
        ],
        "flash_command": f"fastboot flash userdata {out_img}",
    }
    out_json.write_text(json.dumps(meta, indent=2) + "\n")
    print(f"SUCCESS: userdata-root.img written: {out_img} ({total_bytes} bytes, sha256={image_hash})")
    print(f"Flash Command: {meta['flash_command']}")


def package_stage10_boot_image(kernel_path: Path, ramdisk_path: Path, trustcache_path: Path, checked: dict):
    sys.path.insert(0, str(ROOT / "devices/umi/scripts"))
    from umi_adt import decode, u64, string, apply_ipad_profile, apply_smp_profile, load_hardware

    adt_baseline = json.loads((ROOT / "out/umi/adt-baseline.json").read_text())
    adt_src = ROOT / adt_baseline["output"]
    tree = decode(adt_src.read_bytes())
    apply_ipad_profile(tree)
    hardware = load_hardware()
    apply_smp_profile(tree, hardware)

    chosen = tree.at("/chosen").properties
    chosen["random-seed"] = os.urandom(64)
    # Stage 10 full Apple userspace: allocate 1024 MiB (1 GiB) window
    owned = 1024 << 20
    chosen["darwinboot-memory-size"] = u64(owned)

    memory_map = tree.at("/chosen/memory-map").properties
    memory_map["RAMDisk"] = bytes(16)
    memory_map["TrustCache"] = bytes(16)

    out_payload = ROOT / "out/umi/payload"
    generated = out_payload / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    adt_out = generated / "kernel.macho.adt"
    adt_out.write_bytes(tree.encode())
    adt_out.chmod(0o600)

    config_template = ROOT / "devices/umi/package/unixnu-xnu-prelink.toml"
    config_text = config_template.read_text()
    marker = 'path = "Payloads/kernel.macho"\n'
    if marker not in config_text:
        raise ValueError("Missing kernel.macho marker in unixnu-xnu-prelink.toml")
    config_text = config_text.replace(
        marker,
        marker + 'initrd = "Payloads/kernel.macho.ramdisk"\n',
        1,
    )
    config_text = config_text.replace(
        'args = "-v ',
        'args = "rd=md0 launchdsuffix=release -v ',
        1,
    )
    config_text = config_text.replace("umi-build-tag=umi-80", "umi-build-tag=umi-s10")
    config_out = generated / "unixnu.toml"
    config_out.write_text(config_text)

    inputs = {
        "kernel.macho": kernel_path,
        "kernel.macho.adt": adt_out,
        "kernel.macho.ramdisk": ramdisk_path,
        "kernel.macho.trustcache": trustcache_path,
    }

    size_mib = 64
    esp_image = out_payload / "umi-ram-fat.img"
    env = dict(os.environ, TZ="UTC")
    with tempfile.TemporaryDirectory(prefix="stage10-", dir=out_payload) as tmp:
        stage = Path(tmp)
        payloads = stage / "EFI/UniXNU/Payloads"
        payloads.mkdir(parents=True)
        for name, src in inputs.items():
            shutil.copyfile(src, payloads / name)
        (payloads.parent / "unixnu.toml").write_bytes(config_out.read_bytes())
        for p in sorted(stage.rglob("*"), reverse=True):
            os.utime(p, (315532800, 315532800))
        os.utime(stage, (315532800, 315532800))

        esp_image.write_bytes(b"\0" * (size_mib * 1024 * 1024))
        subprocess.run([
            "mformat", "-i", str(esp_image), "-T", str(size_mib * 2048), "-c", "1", "-F",
            "-v", "UMIESP", "-N", "554d4931", "::"
        ], env=env, check=True)
        subprocess.run([
            "mcopy", "-i", str(esp_image), "-s", "-Q", "-m", str(stage / "EFI"), "::/"
        ], env=env, check=True)

    dtb = ROOT / "out/umi/dtb/umi-merged.dtb"
    uefi = ROOT / "sources/uefi-umi"
    driver = ROOT / "sources/UniXNU/unixnu"
    menu_img = ROOT / "out/umi/boot/umi-uefi-menu.img"
    subprocess.run([
        sys.executable, str(driver), "bootimg",
        "-D", str(ROOT / "devices/umi"),
        "--out", str(ROOT / "out/unixnu"),
        "--uefi", str(uefi),
        "--target", "DEBUG",
        "--dtb", str(dtb),
        "--ramdisk", str(esp_image),
        "-o", str(menu_img),
    ], cwd=ROOT, env=env, check=True)

    stage10_boot_img = ROOT / "out/umi/boot/umi-xnu-stage10-systemos-boot.img"
    boot_img = ROOT / "out/umi/boot/umi-xnu-boot.img"
    shutil.copyfile(menu_img, stage10_boot_img)
    shutil.copyfile(menu_img, boot_img)
    print(f"SUCCESS: Stage 10 Boot Image -> {stage10_boot_img} ({stage10_boot_img.stat().st_size} bytes)")


def main():
    print("=== Step 1: Checking Required Apple 21H461 Disk Images ===")
    os_dmg = IPSW_DIR / "044-76040-038.raw"
    sys_dmg = IPSW_DIR / "044-76034-038.dmg"

    if not os_dmg.exists():
        print(f"Waiting for OS DMG {os_dmg} download to finish...")
        while not os_dmg.exists():
            time.sleep(2)
    print(f"  OS DMG:       {os_dmg} ({os_dmg.stat().st_size} bytes)")
    print(f"  SystemOS DMG: {sys_dmg} ({sys_dmg.stat().st_size} bytes)")

    print("=== Step 2: Building BootProgress Observer & Diagnostics ===")
    build_boot_progress()

    print("=== Step 2B: Recompiling Kernel Objects with Stage 10 (SystemOS + Cache Mount) Flags ===")
    compile_object("libsa/DEVELOPMENT/bootstrap.cpo")
    compile_object("osfmk/DEVELOPMENT/arm_vm_init.o")
    compile_object("osfmk/DEVELOPMENT/startup.o", extra_defs=["-DXNU_NATIVE_TRUST_CACHE=1"])
    compile_object("bsd/DEVELOPMENT/vfs_conf.o", extra_defs=["-DMOCKFS=1"])
    compile_object("bsd/DEVELOPMENT/kern_exec.o", extra_defs=STAGE10_DEFS)
    compile_object("bsd/DEVELOPMENT/bsd_init.o", extra_defs=STAGE10_DEFS)
    compile_object("bsd/DEVELOPMENT/ubc_subr.o", extra_defs=STAGE10_DEFS)
    compile_object("iokit/DEVELOPMENT/IOKitBSDInit.cpo", extra_defs=STAGE10_DEFS)
    compile_object("iokit/DEVELOPMENT/KernelConfigTables.cpo", extra_defs=["-DXNU_NATIVE_UFS=1"])

    print("=== Step 3: Linking Stage 10 Source Kernel ===")
    stage10_kernel = KERNEL_DIR / "stage10-source-kernel.macho"
    rebased_specs = [
        (TREE / "build/umi-platform-power/libUmiPlatformPower.a", TREE / "build/umi-platform-power/original-bindings.S"),
        (TREE / "build/native-ufs/libNativeUfs.a", TREE / "build/native-ufs/original-bindings.S"),
        (TREE / "build/native-legacy-display/libNativeLegacyDisplay.late.a", TREE / "build/native-legacy-display/original-bindings.S"),
        (TREE / "build/umi-touch/libUmiTouch.a", TREE / "build/native-hid-input/original-bindings.S"),
        (TREE / "build/umi-storage-providers/libNativeAES.a", TREE / "build/umi-storage-providers/aes-original-bindings.S"),
    ]
    direct_archives = [
        TREE / "build/umi-root-policy/libVerifiedRootProbe.a",
        TREE / "build/umi-storage-providers/libNativeVirtioBlock.a",
        TREE / "build/mockfs-fixture/libmockfs_fixture.a",
    ]
    link_stage10(rebased_specs, direct_archives, stage10_kernel)

    print("=== Step 4: Prelinking Stage 10 Kernel with 30 Apple 21H461 Kexts ===")
    stage10_prelinked = KERNEL_DIR / "stage10-prelinked-kernel.macho"
    prelink_stage10(stage10_kernel, stage10_prelinked)

    print("=== Step 5: Assembling Userdata Raw Partition Image (SHNGROOT) ===")
    userdata_img = USERDATA_DIR / "userdata-root.img"
    userdata_json = USERDATA_DIR / "userdata-root.json"
    if userdata_img.exists() and userdata_json.exists():
        print(f"  Existing userdata-root.img found ({userdata_img.stat().st_size} bytes). Skipping assembly.")
    else:
        assemble_userdata_image(os_dmg, sys_dmg, userdata_img, userdata_json)

    print("=== Step 6: Auditing Stage 10 Prelinked Kernel & Packaging UEFI Boot Image ===")
    from verify_umi_prelink import audit
    rep = audit(stage10_prelinked)
    (ROOT / "out/umi/xnu-prelink-report.json").write_text(json.dumps(rep, indent=2) + "\n")
    (ROOT / "out/umi/xnu-entry-baseline.json").write_text(json.dumps({"kernel": rep}, indent=2) + "\n")
    print(f"Audit PASSED: status={rep['status']}, sha256={rep['sha256']}, bytes={rep['bytes']}, entry={rep['entry']}, components={len(rep['components'])}")

    subprocess.run([
        sys.executable, str(ROOT / "tools/build_umi_uefi.py"),
        "--firmware", "/mnt/c/MyData/Umi2/iOS/UniXNU-umi/bringup/out/firmware/umi",
        "--evidence", "/mnt/c/MyData/Umi2/iOS/UniXNU-umi/bringup/out/device/umi/20261110T154238235361Z",
        "--pack-only",
        "--display-profile", "inherited",
        "--input-profile", "buttons",
        "--payload-profile", "ram",
        "--ram-check", "xnu-prelink",
        "--loader-profile", "darwinboot",
    ], cwd=ROOT, check=True)

    menu_img = ROOT / "out/umi/boot/umi-uefi-menu.img"
    stage10_boot_img = ROOT / "out/umi/boot/umi-xnu-stage10-systemos-boot.img"
    boot_img = ROOT / "out/umi/boot/umi-xnu-boot.img"
    shutil.copyfile(menu_img, stage10_boot_img)
    shutil.copyfile(menu_img, boot_img)
    print(f"SUCCESS: Stage 10 Boot Image -> {stage10_boot_img} ({stage10_boot_img.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
