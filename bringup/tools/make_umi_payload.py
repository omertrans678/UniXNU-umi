#!/usr/bin/env python3
"""Create and round-trip check the umi RAM FAT volume; no device operations."""
import argparse
import hashlib
import json
import os
import random
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def make_payload(output_dir=None, diagnostic="payload"):
    if diagnostic not in ("payload", "memory", "payload64", "xnu", "xnu-entry", "xnu-prelink"):
        raise ValueError("unknown diagnostic")
    out = Path(output_dir) if output_dir is not None else ROOT / "out/umi/payload"
    out.mkdir(parents=True, exist_ok=True)
    configs = {"payload": "unixnu.toml", "memory": "unixnu-memory.toml",
               "payload64": "unixnu-payload64.toml", "xnu": "unixnu-xnu.toml",
               "xnu-entry": "unixnu-xnu-entry.toml",
               "xnu-prelink": "unixnu-xnu-prelink.toml"}
    applications = {"payload": "UmiPayloadCheck.efi", "memory": "UmiMemoryDiag.efi",
                    "payload64": "UmiPayload64Check.efi"}
    large = diagnostic in ("payload64", "xnu", "xnu-entry", "xnu-prelink")
    size_mib = 64 if large else 8
    config = ROOT / "devices/umi/package" / configs[diagnostic]
    config_template = config
    config_bytes = config.read_bytes()
    inputs = {}
    trust_input = None
    data_input = None
    smp_profile = None
    if diagnostic in ("xnu", "xnu-entry", "xnu-prelink"):
        if diagnostic == "xnu-prelink":
            from verify_umi_prelink import audit
        else:
            from verify_umi_xnu import audit
        baseline_path = "out/umi/xnu-entry-baseline.json" if diagnostic != "xnu" else "out/umi/xnu-source-baseline.json"
        baseline = json.loads((ROOT / baseline_path).read_text())
        kernel = Path(baseline["kernel"]["kernel"])
        checked = audit(kernel)
        if sha(kernel) != checked["sha256"]:
            raise ValueError("Kernel report digest differs from the actual payload bytes")
        size_mib = checked.get("fat_volume_bytes", 64 << 20) >> 20
        owned = checked.get("owned_memory_bytes", 256 << 20)
        if checked.get("screen_diagnostics"):
            import tomllib
            parsed = tomllib.loads(config_bytes.decode())
            entries = [e for e in parsed.get("entry", []) if e.get("type") == "xnu"]
            if len(entries) != 1 or "boot-progress=2" not in entries[0].get("args", "").split():
                raise ValueError("Qualified screen diagnostic requires continuous boot-progress=2")
        if checked["sha256"] != baseline["kernel"]["sha256"]:
            raise ValueError("source kernel differs from the qualified host baseline")
        adt_baseline = json.loads((ROOT / "out/umi/adt-baseline.json").read_text())
        adt = ROOT / adt_baseline["output"]
        if sha(adt) != adt_baseline["adt_sha256"]:
            raise ValueError("ADT differs from the host-validated baseline")
        adt_origin_sha = sha(adt)
        if diagnostic == "xnu-prelink":
            # Preserve the qualified hardware and software identity; refresh
            # only boot entropy. Never print the seed or identity fields.
            sys.path.insert(0, str(ROOT / "devices/umi/scripts"))
            from umi_adt import decode, u32, u64, string, apply_ipad_profile, apply_smp_profile, load_hardware
            original = adt.read_bytes()
            tree = decode(original)
            if checked.get("legacy_display"):
                # Same pinned iPad7,1/21H461 as sheng. Keep the native umi
                # hardware tree and opaque platform seed; change only the
                # explicit original userspace-compatibility properties.
                apply_ipad_profile(tree)
                original = tree.encode()
            import tomllib
            entries = tomllib.loads(config_bytes.decode()).get("entry", [])
            xnu_entries = [entry for entry in entries if entry.get("type") == "xnu"]
            if len(xnu_entries) != 1:
                raise ValueError("Expected one XNU entry for explicit SMP policy")
            args = xnu_entries[0].get("args", "").split()
            if "sheng-smp=1" in args:
                if "armcpu-self-ipi=1" not in args or any(a.startswith("armcpu-spi-ipi=") and a != "armcpu-spi-ipi=0" for a in args):
                    raise ValueError("umi SMP requires SGI routing and the original self-IPI startup")
                hardware = load_hardware()
                smp_profile = {"affinities": apply_smp_profile(tree, hardware),
                               "psci_method": hardware["psci"]["method"],
                               "ipi": "ICC_SGI1R_EL1 / SGI 0", "hardware_tested": False}
                original = tree.encode()
            chosen = tree.at("/chosen").properties
            old_seed = chosen["random-seed"]
            if len(old_seed) != 64:
                raise ValueError("unexpected ADT seed length")
            chosen["random-seed"] = os.urandom(64)
            changed_properties = {}
            old_trust_binding = None
            added_packed_binding = False
            if checked.get("trust_cache_runtime"):
                trust = json.loads((ROOT / "out/umi/trustcache/inputs.json").read_text())
                trust_input = Path(trust["bundle"])
                if sha(trust_input) != trust["bundle_sha256"] or trust["bundle_sha256"] != "7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7":
                    raise ValueError("Original static/System trust bundle changed")
                memory_map = tree.at("/chosen/memory-map").properties
                old_trust_binding = memory_map.get("TrustCache")
                memory_map["TrustCache"] = bytes(16)
            if checked.get("data_ramdisk"):
                data_spec = checked["data_ramdisk"]
                data_input = Path(data_spec["path"])
                if sha(data_input) != data_spec["encoded_sha256"]:
                    raise ValueError("Packed Data seed changed after the verified build")
                memory_map = tree.at("/chosen/memory-map").properties
                if "RAMDisk" in memory_map or "PackedRAMDisk" in memory_map:
                    raise ValueError("Unexpected existing Data range in baseline ADT")
                memory_map["PackedRAMDisk"] = bytes(16)
                added_packed_binding = True
                changed_properties["ephemeral-storage"] = chosen.get("ephemeral-storage")
                chosen["ephemeral-storage"] = u32(1)
            if checked.get("native_ufs"):
                policy = json.loads((ROOT / "out/umi/root-policy/OS/verification.json").read_text())
                if not policy["real_c_signature_and_block_verification"]:
                    raise ValueError("Root UUID requires the verified image policy")
                for name in ("darwinboot-memory-size", "root-matching"):
                    changed_properties[name] = chosen.get(name)
                # Stage 10 full Apple userspace requires 1024 MiB (1 GiB) window
                chosen["darwinboot-memory-size"] = u64(1024 << 20)
                chosen["root-matching"] = string(
                    "<dict><key>IOProviderClass</key><string>IOMedia</string>"
                    "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
                    + policy["volume_uuid"] + "</string></dict></dict>")
            fresh = tree.encode()
            check = decode(fresh)
            if check.at("/chosen").properties["random-seed"] == old_seed:
                raise ValueError("ADT entropy did not change")
            check.at("/chosen").properties["random-seed"] = old_seed
            if trust_input:
                memory_map = check.at("/chosen/memory-map").properties
                if memory_map["TrustCache"] != bytes(16):
                    raise ValueError("TrustCache must be an unbound loader-owned range")
                if old_trust_binding is None:
                    del memory_map["TrustCache"]
                else:
                    memory_map["TrustCache"] = old_trust_binding
            if added_packed_binding:
                memory_map = check.at("/chosen/memory-map").properties
                if memory_map.pop("PackedRAMDisk") != bytes(16):
                    raise ValueError("PackedRAMDisk must be an unbound loader-owned range")
            for name, old in changed_properties.items():
                if old is None:
                    del check.at("/chosen").properties[name]
                else:
                    check.at("/chosen").properties[name] = old
            if check.encode() != original:
                raise ValueError("hardware/identity ADT fields changed")
            generated = out / "generated"
            generated.mkdir(exist_ok=True)
            adt = generated / "kernel.macho.adt"
            adt.write_bytes(fresh)
            adt.chmod(0o600)
        inputs = {"kernel.macho": kernel, "kernel.macho.adt": adt}
        if trust_input:
            inputs["kernel.macho.trustcache"] = trust_input
        if data_input:
            inputs["kernel.macho.ramdisk"] = data_input
            marker = b'path = "Payloads/kernel.macho"\n'
            if config_bytes.count(marker) != 1 or b'initrd =' in config_bytes:
                raise ValueError("Expected one explicit XNU entry without a conflicting initrd")
            config_bytes = config_bytes.replace(marker, marker + b'initrd = "Payloads/kernel.macho.ramdisk"\n')
            # Archive the exact generated menu that explicitly supplies initrd.
            config = out / "generated/unixnu.toml"
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_bytes(config_bytes)
    else:
        app_name = applications[diagnostic]
        app = ROOT / "sources/uefi-umi/Build/umiPkg/DEBUG_CLANGPDB/AARCH64" / app_name
        if not app.is_file():
            raise ValueError(f"payload application not built: {app}")
        from import_umi_uefi import image_machine
        if image_machine(app.read_bytes(), "PE32 image") != 0xAA64:
            raise ValueError("payload application is not AArch64")
        inputs = {app_name: app}
    image = out / "umi-ram-fat.img"
    env = dict(os.environ, TZ="UTC")
    with tempfile.TemporaryDirectory(prefix="stage-", dir=out) as tmp:
        stage = Path(tmp)
        payloads = stage / "EFI/UniXNU/Payloads"
        payloads.mkdir(parents=True)
        for name, path in inputs.items():
            shutil.copyfile(path, payloads / name)
            if sha(payloads / name) != sha(path):
                raise ValueError(f"staged payload differs from source: {name}")
        if diagnostic == "payload":
            (payloads / "proof.bin").write_bytes(bytes((i * 73 + 19) & 255 for i in range(16384)))
        if diagnostic == "payload64":
            # Fixed seed, distinct data across old 16 MiB and FAT cluster boundaries.
            proof = payloads / "proof-large.bin"
            proof.write_bytes(random.Random(0x554D4932).randbytes(60 * 1024 * 1024))
            (payloads / "proof-large.sha256").write_text(sha(proof), encoding="ascii")
        (payloads.parent / "unixnu.toml").write_bytes(config_bytes)
        for path in sorted(stage.rglob("*"), reverse=True):
            os.utime(path, (315532800, 315532800))
        os.utime(stage, (315532800, 315532800))
        # FAT32 at 64 MiB matches the planned XNU volume; legacy 8 MiB is FAT16.
        # Both are superfloppies and require no partition driver.
        image.write_bytes(b"\0" * (size_mib * 1024 * 1024))
        subprocess.run(["mformat", "-i", str(image), "-T", str(size_mib * 2048), "-c", "1",
                        *(["-F"] if large else []),
                        "-v", "UMIESP", "-N", "554d4931", "::"], env=env, check=True)
        subprocess.run(["mcopy", "-i", str(image), "-s", "-Q", "-m", str(stage / "EFI"), "::/"],
                       env=env, check=True)
        files = {str(p.relative_to(stage)): sha(p) for p in sorted(stage.rglob("*")) if p.is_file()}
        extract = stage / "roundtrip"
        extract.mkdir()
        subprocess.run(["mcopy", "-i", str(image), "-s", "::/EFI", str(extract)], env=env, check=True)
        actual = {str(p.relative_to(extract)): sha(p) for p in sorted(extract.rglob("*")) if p.is_file()}
        if actual != files:
            raise ValueError("FAT file extraction differs from staged payload")
        # Retain readable files without changing the already formatted volume.
        saved = out / "files"
        if saved.exists():
            shutil.rmtree(saved)
        shutil.copytree(stage / "EFI", saved / "EFI")
    report = {"format": "FAT32-superfloppy" if large else "FAT16-superfloppy",
              "label": "UMIESP", "bytes": image.stat().st_size,
              "sha256": sha(image), "files": files, "roundtrip_verified": True,
              "config_source": str(config),
              "config_template_source": str(config_template),
              "config_template_sha256": sha(config_template),
              "application_not_in_firmware": True, "diagnostic": diagnostic}
    if diagnostic in ("xnu", "xnu-entry", "xnu-prelink"):
        report["xnu"] = {"kernel": checked, "adt_sha256": sha(adt),
                         "adt_original_sha256": adt_origin_sha,
                         "adt_only_entropy_refreshed": diagnostic == "xnu-prelink" and not checked.get("native_ufs"),
                         "adt_storage_delta": (["darwinboot-memory-size", "root-matching"] if checked.get("native_ufs") else [])
                             + (["memory-map/TrustCache"] if trust_input else [])
                             + (["memory-map/PackedRAMDisk", "ephemeral-storage"] if data_input else []),
                         "owned_memory_bytes": owned, "hardware_tested": False,
                         "userspace_product_profile": "iPad7,1/21H461-sheng" if checked.get("legacy_display") else "native-umi",
                         "trustcache": bool(trust_input),
                         "trustcache_sha256": sha(trust_input) if trust_input else None,
                         "data_ramdisk": checked.get("data_ramdisk") or False}
        report["xnu"]["smp_profile"] = smp_profile
    (out / "payload.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return image


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        help="isolated host output; default out/umi/payload")
    parser.add_argument("--diagnostic", choices=("payload", "memory", "payload64", "xnu", "xnu-entry", "xnu-prelink"), default="payload")
    args = parser.parse_args()
    make_payload(args.output_dir, args.diagnostic)
