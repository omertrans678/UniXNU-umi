#!/usr/bin/env python3
"""Build the complete sheng diagnostic against umi's actual XNU compile ABI."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "kits/umi/boot-progress"
OUT = ROOT / "build/boot-progress"
OBJ = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    if os.environ.get("UMI_FULL_BOOT_PROGRESS") != "1":
        raise SystemExit("Explicit full umi diagnostic profile required")
    phone = os.environ.get("UMI_EARLY_TRACE", "0")
    if phone not in ("0", "1"):
        raise SystemExit("Invalid phone trace profile")
    OUT.mkdir(exist_ok=True, parents=True)
    objects, commands = [], {}
    for name in ("BootProgress", "BootFaultProbe", "BootWatchdogPolicy",
                 "BootProcessProbe", "BootUiTraceProbe", "UmiUserspaceProbe"):
        record_path = (OBJ / "bsd/DEVELOPMENT/kern_exec.o.json" if name in ("BootWatchdogPolicy", "UmiUserspaceProbe")
                       else OBJ / "osfmk/DEVELOPMENT/startup.o.json")
        r = json.loads(record_path.read_text().strip().rstrip(","))
        args, command, i = r["arguments"], [], 0
        obj = OUT / (name + ".o")
        while i < len(args):
            arg = args[i]
            if arg == r["file"]:
                command.append(str(SOURCE / (name + ".c")))
            elif arg in ("-o", "-MF", "-MT", "-MJ"):
                if arg == "-o":
                    command += ["-o", str(obj)]
                i += 1
            elif arg == "-Xclang" and args[i+1].startswith("-W"):
                i += 1
            elif arg == "-D" and args[i+1].startswith("XNU_BOOT_PROGRESS="):
                i += 1
            elif not arg.startswith(("-W", "-DXNU_BOOT_PROGRESS=")):
                command.append(arg)
            i += 1
        command += ["-DUMI_FTS_TOUCH=" + os.environ.get("UMI_FTS_TOUCH", "0"), "-DXNU_BOOT_PROGRESS=1", "-DUMI_BOOT_PROGRESS_PHONE=" + phone,
                    "-DUMI_RUNTIME_TRACE=" + str(int(phone == "1" and os.environ.get("XNU_NATIVE_RAM_BLOCK") == "1" and os.environ.get("XNU_NATIVE_LEGACY_DISPLAY") == "1")),
                    "-mgeneral-regs-only", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / ".cache/xnu-work/osfmk")]
        subprocess.run(command, cwd=r["directory"], check=True, timeout=90)
        commands[name] = command
        asm = subprocess.check_output(
            ["/usr/lib/llvm-18/bin/llvm-objdump", "-d", obj], text=True)
        operands = "\n".join(re.findall(
            r"^[ \t]*[0-9a-f]+:[ \t]+[0-9a-f]{8}[ \t]+\w+[ \t]+([^\n]+)", asm, re.M))
        if re.search(r"\b(?:[qds][0-9]+|v[0-9]+(?:\.[0-9a-z]+)?)\b", operands):
            raise SystemExit("FP/SIMD found in " + name)
        if re.search(r"\bmsr\s+(?:TCR_EL1|TTBR[01]_EL1|SCTLR_EL1)", asm):
            raise SystemExit("Translation write found in observer")
        if name == "BootProgress":
            for pattern in (r"mrs\s+x\d+, CNTPCT_EL0", r"mrs\s+x\d+, CNTFRQ_EL0",
                            r"msr\s+DAIFSet, #0x3", r"msr\s+DAIF, x\d+"):
                if not re.search(pattern, asm):
                    raise SystemExit("Missing actual ARM64 instruction: " + pattern)
        (OUT / (name + ".asm")).write_text(asm)
        objects.append(obj)
    # Mach-O static kernel links reject absent weak imports. Preserve the full
    # observer and its guarded views, but make unavailable optional addresses
    # compile-time NULL. This defines no driver symbols or backing counters.
    # Only native_* telemetry may be absent; mandatory kernel imports still
    # go through the ordinary linker. Re-evaluate against real providers on
    # every build, so linking a provider restores its complete diagnostic view.
    providers = [ROOT.parent / "kernel/kernel.macho",
                 ROOT / "build/armcpu/libArmCpu.a", *objects]
    if os.environ.get("XNU_NATIVE_UFS") == "1":
        providers.append(ROOT / "build/native-ufs/libNativeUfs.a")
    if os.environ.get("XNU_NATIVE_AES") == "1":
        providers.append(Path(os.environ["XNU_NATIVE_AES_ARCHIVE"]))
    if os.environ.get("XNU_NATIVE_VIRTIO_BLOCK") == "1":
        providers.append(Path(os.environ["XNU_NATIVE_VIRTIO_BLOCK_ARCHIVE"]))
    if os.environ.get("XNU_NATIVE_RAM_BLOCK") == "1":
        providers.append(ROOT / "build/native-ram-block/libNativeRamBlock.a")
    if os.environ.get("XNU_NATIVE_LEGACY_DISPLAY") == "1":
        providers.append(ROOT / "build/native-legacy-display/libNativeLegacyDisplay.late.a")
    if os.environ.get("XNU_OWNER_ROOT_AUDIT") == "1":
        # These are defined in this build's bsd_init translation unit, which is
        # rebuilt after the observer. They are not optional external drivers.
        providers.append(ROOT / ".cache/xnu-work/bsd/kern/bsd_init.c")
    if os.environ.get("UMI_FTS_TOUCH") == "1":
        providers.append(ROOT / "build/umi-touch/libUmiTouch.a")
    defined = set()
    for path in providers:
        if not path.exists():
            raise SystemExit("Missing actual provider inventory: " + str(path))
        if path.suffix == ".c":
            content = path.read_text()
            for name in re.findall(r"\b(native_owner_root_\w+)\s*=", content):
                defined.add("_" + name)
        else:
            defined.update(subprocess.check_output(
                ["xcrun", "nm", "-g", "-U", "-j", path], text=True).splitlines())
    weak = set()
    for obj in objects:
        symbols = subprocess.check_output(["xcrun", "nm", "-m", obj], text=True)
        weak.update(re.findall(r"\(undefined\) weak external (_\w+)", symbols))
    missing = sorted(weak-defined)
    if any(not n.startswith("_native_") for n in missing):
        raise SystemExit("Unexpected missing kernel observer import: " + str(missing))
    generated = {}
    for name in commands:
        text = (SOURCE / (name + ".c")).read_text()
        found = set()
        def optional(match):
            declaration = match.group()
            names = [symbol[1:] for symbol in missing
                     if re.search(r"\b" + re.escape(symbol[1:]) + r"\b", declaration)]
            repeated = [n for n in names if "_" + n in found]
            found.update("_" + n for n in names)
            if not names:
                return declaration
            return ("\n".join("#undef " + n for n in repeated) + "\n" + declaration +
                    "\n// No linked provider: typed unavailable addresses, not fake objects.\n" +
                    "\n".join(f"#define {n} (*(__typeof__(&{n}))0)" for n in names) + "\n")
        text = re.sub(r"extern\b[^;]*?__attribute__\(\(weak_import\)\)[^;]*;",
                      optional, text, flags=re.S)
        if not found:
            continue
        path = OUT / (name + ".providers.c")
        path.write_text(text)
        command = [str(path) if v == str(SOURCE / (name + ".c")) else v for v in commands[name]]
        command += ["-I", str(SOURCE)]
        subprocess.run(command, cwd=r["directory"], check=True, timeout=90)
        commands[name] = command
        generated[path.name] = sha(path)
        symbols = subprocess.check_output(["xcrun", "nm", "-u", OUT / (name + ".o")], text=True)
        for symbol in found:
            if re.search(r"\b" + re.escape(symbol) + r"\b", symbols):
                raise SystemExit("Unavailable import survived compilation: " + symbol)
    # Inspect final objects too, after optional views have been compiled.
    for obj in objects:
        asm = subprocess.check_output(
            ["/usr/lib/llvm-18/bin/llvm-objdump", "-d", obj], text=True)
        operands = "\n".join(re.findall(
            r"^[ \t]*[0-9a-f]+:[ \t]+[0-9a-f]{8}[ \t]+\w+[ \t]+([^\n]+)", asm, re.M))
        if re.search(r"\b(?:[qds][0-9]+|v[0-9]+(?:\.[0-9a-z]+)?)\b", operands):
            raise SystemExit("FP/SIMD found in final observer " + obj.name)
        if re.search(r"\bmsr\s+(?:TCR_EL1|TTBR[01]_EL1|SCTLR_EL1)", asm):
            raise SystemExit("Translation write found in final observer")
        (OUT / (obj.stem + ".asm")).write_text(asm)
    archive = OUT / "libBootProgress.a"
    archive.unlink(missing_ok=True)
    subprocess.run(["xcrun", "ar", "rcs", archive, *objects], check=True)
    (OUT / "compile-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    (OUT / "build-report.json").write_text(json.dumps({
        "implementation": "complete-sheng-diagnostics-with-umi-provider-adaptation",
        "upstream": json.loads((SOURCE / "upstream.json").read_text()),
        "phone_trace": phone == "1",
        "sources": {p.name: sha(p) for p in sorted(SOURCE.iterdir()) if p.is_file()},
        "objects": {p.name: sha(p) for p in objects}, "archive_sha256": sha(archive),
        "generated_sources": generated, "unavailable_optional_imports": missing,
        "provider_inventory": {str(p): sha(p) for p in providers if p not in objects},
        "actual_arm64_instructions_checked": True, "no_fp_simd": True,
        "hardware_execution": False,
        "storage_provider_required": False, "absent_providers_reported": True,
    }, indent=2) + "\n")
    print("PASS: complete official diagnostic, fault and process observers built for ARM64")

if __name__ == "__main__":
    main()
