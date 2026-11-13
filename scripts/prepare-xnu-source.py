#!/usr/bin/env python3
"""Restore the upstream recipe's omitted headers from pinned Git objects."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    ("xnu-driverkit", "2c2f96dc2b9a4408a43d3150ae9c105355ca3daa",
     "xnu-10063.121.3", "iokit/DriverKit"),
    ("xnu-trustcache", "aca3beaa3dfbd42498b42c5e5ce20a938e6554e5",
     "xnu-8796.101.5", "EXTERNAL_HEADERS/TrustCache"),
)


def main():
    # This hook runs after the upstream patch series. Keep our optional-driver
    # fix separate, so the pinned upstream patch module is not replaced.
    work = ROOT / ".cache/xnu-work"
    for patch in sorted((ROOT / "patches/umi-xnu").glob("*.patch")):
        reverse = subprocess.run(["git", "-C", work, "apply", "--reverse", "--check", patch],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if reverse.returncode:
            subprocess.run(["git", "-C", work, "apply", "--check", patch], check=True)
            subprocess.run(["git", "-C", work, "apply", patch], check=True)
    trace = os.environ.get("UMI_EARLY_TRACE", "0")
    if trace not in ("0", "1"):
        raise ValueError("UMI_EARLY_TRACE must be 0 or 1")
    pmgr = os.environ.get("UMI_PMGR", "0")
    if pmgr not in ("0", "1"):
        raise ValueError("UMI_PMGR must be 0 or 1")
    ufs = os.environ.get("UMI_UFS_PROBE", "0")
    if ufs not in ("0", "1"):
        raise ValueError("UMI_UFS_PROBE must be 0 or 1")
    trace_files = {
        "umi_early_trace_config.h": f"#define UMI_EARLY_TRACE {trace}\n".encode(),
        "umi_platform_power_config.h": f"#define UMI_PMGR {pmgr}\n".encode(),
        "umi_ufs_config.h": f"#define UMI_UFS_PROBE {ufs}\n".encode(),
        "umi-early-trace.inc": (ROOT / "scripts/umi-early-trace.inc").read_bytes(),
        "umi-root-trace.h": (ROOT / "scripts/umi-root-trace.h").read_bytes(),
        "umi-lock-trace.h": (ROOT / "scripts/umi-lock-trace.h").read_bytes(),
        "umi-exec-trace.h": (ROOT / "scripts/umi-exec-trace.h").read_bytes(),
        "umi-userspace-api.h": (ROOT / "scripts/umi-userspace-api.h").read_bytes(),
    }
    for name, data in trace_files.items():
        target = work / "osfmk/arm64" / name
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    records = []
    for name, commit, tag, prefix in SOURCES:
        source = ROOT / "refer" / name
        head = subprocess.check_output(
            ["git", "-C", source, "rev-parse", "HEAD"], text=True).strip()
        if head != commit:
            raise SystemExit(f"Unexpected header source revision: {name}: {head}")
        headers = sorted(source.rglob("*.h"))
        if not headers:
            raise SystemExit(f"Missing pinned headers: {source}")
        for header in headers:
            relative = header.relative_to(source)
            git_path = f"{prefix}/{relative.as_posix()}"
            data = subprocess.check_output(["git", "-C", source, "show", f"HEAD:{git_path}"])
            if header.read_bytes() != data:
                raise SystemExit(f"Modified reference header: {header}")
            target = ROOT / ".cache/xnu-work" / git_path
            existed = target.exists()
            if not existed:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            # Like upstream, retain any header already present in the XNU work tree.
            records.append({
                "name": git_path, "source_tag": tag, "source_commit": commit,
                "git_blob": hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest(),
                "sha256": hashlib.sha256(data).hexdigest(),
                "installed_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "already_present": existed,
            })
    out = ROOT / ".cache/sources/xnu-restored-headers.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(records, indent=2) + "\n")
    print(f"Prepared {len(records)} pinned public headers through Git; no GitHub API required")

    # Regenerate DriverKit IIG stubs and headers (idempotent via _write_if_changed)
    regen_script = Path("/mnt/c/MyData/Umi2/iOS/umi-work/tools/regen_iig.py")
    if regen_script.exists():
        import sys
        subprocess.run([sys.executable, str(regen_script)], check=True)

    # Enforce strict DAG timestamp hierarchy for incremental Make builds:
    # T1 (1794300000): .cache/xnu-work sources
    # T2 (1794340000): SETUP generated .c/.h
    # T3 (1794345000): SETUP .o files
    # T4 (1794350000): SETUP host binaries (config, installfile, replacecontents, decomment, etc.)
    # T5 (1794360000): EXPORT_HDRS, .cache/fakeroot, ios-obj/bsd, ios-obj/san, and non-object files in DEVELOPMENT_ARM64_BCM2837
    # T6 (>= 1794400000): compiled kernel .o / .cpo / .filelist objects in DEVELOPMENT_ARM64_BCM2837
    T1_NS = 1794300000 * 1_000_000_000
    T2_NS = 1794340000 * 1_000_000_000
    T3_NS = 1794345000 * 1_000_000_000
    T4_NS = 1794350000 * 1_000_000_000
    T5_NS = 1794360000 * 1_000_000_000

    for root_dir, dirs, files in os.walk(work):
        if ".git" in dirs:
            dirs.remove(".git")
        for fname in files:
            fpath = os.path.join(root_dir, fname)
            try:
                st = os.stat(fpath, follow_symlinks=False)
                if st.st_mtime_ns != T1_NS:
                    os.utime(fpath, ns=(T1_NS, T1_NS), follow_symlinks=False)
            except OSError:
                pass

    setup_dir = ROOT / "build/xnu/ios-obj/SETUP"
    if setup_dir.exists():
        for root_dir, _, files in os.walk(setup_dir, topdown=False):
            for fname in files:
                fpath = os.path.join(root_dir, fname)
                if fname.endswith((".c", ".h")):
                    ts = T2_NS
                elif fname.endswith(".o"):
                    ts = T3_NS
                else:
                    ts = T4_NS
                try:
                    st = os.stat(fpath, follow_symlinks=False)
                    if st.st_mtime_ns != ts:
                        os.utime(fpath, ns=(ts, ts), follow_symlinks=False)
                except OSError:
                    pass
            try:
                os.utime(root_dir, ns=(T4_NS, T4_NS), follow_symlinks=False)
            except OSError:
                pass

    for t5_file in (
        ROOT / "build/xnu/owner-data-volume.h",
        ROOT / "build/xnu/ios-obj/xnuVersion",
        ROOT / "build/xnu/ios-obj/.mach_kernel.timestamp",
        ROOT / "build/xnu/ios-obj/.symbolset.timestamp",
    ):
        if t5_file.exists():
            os.utime(t5_file, ns=(T5_NS, T5_NS))
    fakeroot_dir = ROOT / ".cache/fakeroot"
    if fakeroot_dir.exists():
        for kalloc_h in fakeroot_dir.rglob("kalloc.h"):
            txt = kalloc_h.read_text()
            if "__has_builtin(__builtin_xnu_type_summary)" not in txt and "#define KT_SUMMARY_GRANULES(type)" in txt:
                guard = (
                    "#ifndef __has_builtin\n#define __has_builtin(x) 0\n#endif\n"
                    "#if !__has_builtin(__builtin_xnu_type_summary)\n#define __builtin_xnu_type_summary(type) 0UL\n#endif\n"
                    "#if !__has_builtin(__builtin_xnu_types_compatible)\n#define __builtin_xnu_types_compatible(t1, t2) 1\n#endif\n"
                    '#if !__has_builtin(__builtin_xnu_type_signature)\n#define __builtin_xnu_type_signature(type) ""\n#endif\n\n'
                )
                kalloc_h.chmod(0o644)
                kalloc_h.write_text(txt.replace("#define KT_SUMMARY_GRANULES(type)", guard + "#define KT_SUMMARY_GRANULES(type)"))

    for t5_dir in (
        ROOT / "build/xnu/ios-obj/EXPORT_HDRS",
        ROOT / "build/xnu/ios-obj/bsd",
        ROOT / "build/xnu/ios-obj/san",
        ROOT / "build/xnu/ios-obj/cscope.genhdrs",
        ROOT / ".cache/fakeroot",
    ):
        if t5_dir.exists():
            for root_dir, _, files in os.walk(t5_dir):
                for fname in files:
                    fpath = os.path.join(root_dir, fname)
                    try:
                        st = os.stat(fpath, follow_symlinks=False)
                        if st.st_mtime_ns != T5_NS:
                            os.utime(fpath, ns=(T5_NS, T5_NS), follow_symlinks=False)
                    except OSError:
                        pass

    T6_NS = 1794400000 * 1_000_000_000
    obj_dir = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
    if obj_dir.exists():
        for root_dir, _, files in os.walk(obj_dir):
            for fname in files:
                fpath = os.path.join(root_dir, fname)
                if fname.endswith((".o", ".cpo")) and fname != "genassym.o":
                    try:
                        st = os.stat(fpath, follow_symlinks=False)
                        if st.st_size < 64:
                            os.unlink(fpath)
                            continue
                        if st.st_mtime_ns < T6_NS:
                            os.utime(fpath, ns=(T6_NS, T6_NS), follow_symlinks=False)
                    except OSError:
                        pass
                    continue
                if fname.endswith((".filelist", ".noctf", ".d", ".cpd")):
                    continue
                try:
                    st = os.stat(fpath, follow_symlinks=False)
                    if st.st_mtime_ns != T5_NS:
                        os.utime(fpath, ns=(T5_NS, T5_NS), follow_symlinks=False)
                except OSError:
                    pass
    print("Aligned strict 6-tier incremental build mtimes successfully")


if __name__ == "__main__":
    main()
