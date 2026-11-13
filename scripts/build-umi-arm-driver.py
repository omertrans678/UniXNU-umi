#!/usr/bin/env python3
"""Build ArmDTPlatform/ArmCpu using the source kernel's actual C++ recipe.

Adapted from UniXNU-sheng scripts/build-arm-driver.py at
a599a1a8a92093b5077543e071b72130769eb325; only the generic ARM drivers are used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("component", choices=("ArmDTPlatform", "ArmCpu"))
parser.add_argument("--gic-diagnostics", choices=("phone", "qemu"),
                    help="compile a device-owned observer overlay, leaving upstream intact")
args = parser.parse_args()
if args.gic_diagnostics and args.component != "ArmCpu":
    parser.error("GIC diagnostics apply only to ArmCpu")
recipe = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
record = json.loads(recipe.read_text().strip().rstrip(","))
source = ROOT / "drivers" / args.component / (args.component + ".cpp")
out = ROOT / "build" / {"ArmDTPlatform": "armdt-platform", "ArmCpu": "armcpu"}[args.component]
out.mkdir(parents=True, exist_ok=True)
upstream = source
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
inputs = {"upstream_source": {"path": str(upstream), "sha256": digest(upstream)}}
if args.component == "ArmCpu":
    source = out / "ArmCpu.cpp"
    source.write_bytes(upstream.read_bytes())
    patch = ROOT / "patches/umi-armcpu/darwin-asm-separators.patch"
    subprocess.run(["patch", "--batch", "--fuzz=0", str(source), str(patch)], check=True)
    inputs[patch.name] = {"path": str(patch), "sha256": digest(patch)}
if args.gic_diagnostics:
    patch = ROOT / "patches/umi-armcpu/gic-observer.patch"
    include = ROOT / "kits/umi/UmiArmCpuTrace.inc"
    subprocess.run(["patch", "--batch", "--fuzz=0", str(source), str(patch)], check=True)
    (out / include.name).write_bytes(include.read_bytes())
    inputs.update({p.name: {"path": str(p), "sha256": digest(p)} for p in (patch, include)})
destination = out / (args.component + ".o")
command = []
arguments = record["arguments"]
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record["file"]:
        command.append(str(source))
    elif value == "-o":
        command.extend(["-o", str(destination)])
        index += 1
    elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
        index += 1
    elif not value.startswith("-W"):
        command.append(value)
    index += 1
command.extend([
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
    "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
    "-I", str(ROOT / ".cache/xnu-work/osfmk"),
])
if args.gic_diagnostics:
    command.append(f"-DUMI_ARMCPU_PHONE_TRACE={int(args.gic_diagnostics == 'phone')}")
(out / "compile-command.json").write_text(json.dumps(command, indent=2) + "\n")
result = subprocess.run(command, cwd=record["directory"], capture_output=True, text=True)
(out / "compile.log").write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.stderr[-5000:])
data = destination.read_bytes()
if struct.unpack_from("<4I", data) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit("Expected ordinary ARM64 MH_OBJECT")
offset = 32
deployment = None
for _ in range(struct.unpack_from("<I", data, 16)[0]):
    cmd, length = struct.unpack_from("<II", data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from("<II", data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f"Unexpected deployment: {deployment}")
if args.component == "ArmCpu":
    disassembly = subprocess.check_output(
        ["xcrun", "llvm-objdump", "--macho", "--disassemble", "--no-show-raw-insn", destination],
        text=True)
    (out / "disassembly.txt").write_text(disassembly)
    # Apple ARM64 assembly treats semicolons as comments. A successful compile
    # alone previously lost CNTVCT reads, SGI writes and trailing barriers.
    for pattern in (
        r"\bisb\n[^\n]*\bmrs\s+x\d+, CNTVCT_EL0",
        r"\bmsr\s+ICC_EOIR1_EL1, x\d+\n[^\n]*\bisb",
        r"\bdsb\s+ishst\n[^\n]*\bmsr\s+ICC_SGI1R_EL1, x\d+\n[^\n]*\bisb",
        r"\bmsr\s+CNTV_CTL_EL0, x\d+\n[^\n]*\bisb",
    ):
        if not re.search(pattern, disassembly):
            raise SystemExit(f"Missing actual instruction sequence: {pattern}")
    if args.gic_diagnostics and not re.search(
        r"\bmrs\s+x\d+, DAIF\n[^\n]*\bmsr\s+DAIFSet, #(?:0x)?3\b", disassembly
    ):
        raise SystemExit("Observer did not actually mask IRQ/FIQ")
archive = out / ("lib" + args.component + ".a")
archive.unlink(missing_ok=True)
subprocess.run(["xcrun", "ar", "rcs", archive, destination], check=True)
report = {"component": args.component, "gic_diagnostics": args.gic_diagnostics,
          "inputs": inputs, "compiled_source_sha256": digest(source),
          "object_sha256": digest(destination), "archive_sha256": digest(archive)}
if args.component == "ArmCpu":
    report["actual_instruction_sequences_checked"] = True
    report["disassembly_sha256"] = digest(out / "disassembly.txt")
(out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"{args.component}: ARM64/iOS 17 object audit and -Werror build passed")
