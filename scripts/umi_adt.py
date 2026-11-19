"""Apple device-tree encoding and the umi early-kernel tree.

CHRP layout follows UniXNU-sheng make-sheng-adt.py at a599a1a8a92093b5077543e071b72130769eb325.
Copyright (c) 2026, UniXNU contributors.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import struct
import uuid
import zlib


def u32(value):
    return struct.pack("<I", value)


def u64(value):
    return struct.pack("<Q", value)


def string(value):
    if "\0" in value:
        raise ValueError("embedded NUL in ADT string")
    return value.encode("ascii") + b"\0"


@dataclass
class Node:
    name: str
    properties: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

    def at(self, path):
        current = self
        for part in path.strip("/").split("/") if path.strip("/") else []:
            current = next(n for n in current.children if n.name == part)
        return current

    def encode(self):
        props = {"name": string(self.name), **self.properties}
        if "name" in self.properties or len({n.name for n in self.children}) != len(self.children):
            raise ValueError("duplicate name property or child")
        data = bytearray(struct.pack("<II", len(props), len(self.children)))
        for key, value in props.items():
            key = string(key)
            if len(key) > 32 or not isinstance(value, bytes):
                raise ValueError("invalid property name or value")
            data += struct.pack("<32sI", key, len(value))
            data += value + bytes(-len(value) % 4)
        for child in self.children:
            data += child.encode()
        return bytes(data)


def decode(blob):
    """Strict host decoder: reject duplicate nodes/properties and trailing data."""
    def read(offset, depth):
        if depth > 64 or len(blob) - offset < 8:
            raise ValueError("truncated or excessively deep ADT")
        count, children = struct.unpack_from("<II", blob, offset)
        offset += 8
        props = {}
        for _ in range(count):
            if len(blob) - offset < 36:
                raise ValueError("truncated property header")
            key_raw, size = struct.unpack_from("<32sI", blob, offset)
            if b"\0" not in key_raw:
                raise ValueError("unterminated property name")
            key = key_raw.split(b"\0", 1)[0].decode("ascii")
            offset += 36
            end = offset + size
            padded = (end + 3) & ~3
            if not key or key in props or padded > len(blob):
                raise ValueError("duplicate/empty property or truncated value")
            if any(blob[end:padded]):
                raise ValueError("nonzero property padding")
            props[key] = blob[offset:end]
            offset = padded
        name = props.pop("name", b"")
        if not name.endswith(b"\0") or b"\0" in name[:-1] or not name[:-1]:
            raise ValueError("invalid node name")
        child_nodes = []
        for _ in range(children):
            child, offset = read(offset, depth + 1)
            if child.name in {n.name for n in child_nodes}:
                raise ValueError("duplicate child node")
            child_nodes.append(child)
        return Node(name[:-1].decode("ascii"), props, child_nodes), offset

    tree, end = read(0, 0)
    if end != len(blob) or tree.name != "device-tree":
        raise ValueError("invalid root or trailing bytes")
    return tree


def empty_nvram_proxy():
    def header(name, size):
        raw = bytearray(struct.pack("<BBH12s", 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b"nvram", 32)
    image[32:48] = header(b"common", 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]))
    return {
        "nvram-bank-size": u32(len(image)), "nvram-bank-count": u32(1),
        "nvram-current-bank": u32(0), "nvram-proxy-data": bytes(image),
    }


def hardware_path():
    root = Path(__file__).resolve().parents[1]
    # Source device repository, or the official assembled build tree.
    for relative in ("package/umi-hardware.json", "kits/umi/umi-hardware.json"):
        candidate = root / relative
        if candidate.is_file():
            return candidate
    raise ValueError("umi hardware profile missing from package or tree/kits/umi")


def load_hardware():
    hardware = json.loads(hardware_path().read_text())
    if (hardware["schema"], hardware["device"], hardware["soc"]) != (1, "umi", "SM8250"):
        raise ValueError("not the qualified umi hardware profile")
    banks = sorted(hardware["memory_banks"], key=lambda b: b["base"])
    end = 0
    for bank in banks:
        base, size = bank["base"], bank["size"]
        if base < end or size <= 0 or base + size >= 1 << 64:
            raise ValueError("overlapping or invalid physical DRAM banks")
        end = base + size
    if hardware["psci"]["method"] != "smc":
        raise ValueError("umi PSCI conduit must be smc")
    return hardware


def apply_ipad_profile(tree):
    """The sheng iPad7,1/21H461 userspace profile, with umi hardware retained.

    These are compatibility inputs for this offline port, not claims about
    Apple hardware, fuses, key material or display timing.
    """
    tree.properties.update({"model": string("iPad7,1"), "target-type": string("J120")})
    tree.at("/product").properties.update({
        "product-name": string("iPad Pro (12.9-inch) (2nd generation)"),
        "product-description": string("iPad Pro (12.9-inch) (2nd generation)"),
        "artwork-device-idiom": string("pad"), "artwork-device-subtype": u32(2732),
        "artwork-scale-factor": u32(2), "artwork-display-gamut": string("sRGB"),   # umi-147: BGRA8 composition for DPU direct scanout (unixnu.com/porting/display; gts7xlwifi record)
        "compatible-device-fallback": string("iPad6,7"), "thin-bezel": u32(1),
    })
    tree.at("/chosen").properties.update({
        "certificate-production-status": u32(0), "effective-production-status-ap": u32(0),
        "certificate-security-mode": u32(1), "effective-security-mode-sep": u32(1),
    })


def apply_smp_profile(tree, hardware):
    """Add the observed umi cores and PSCI conduit to the qualified tree.

    Preserve CPU0, GIC, platform identity, entropy and every non-SMP node.
    The caller separately opts in to the original ArmCpu sheng-smp path.
    """
    affinities = [cpu["affinity"] for cpu in hardware["cpus"]]
    if affinities != [i << 8 for i in range(8)] or hardware["psci"]["method"] != "smc":
        raise ValueError("SMP requires the eight observed umi affinities and SMC")
    canonical = build_tree(hardware, dram_size=sum(b["size"] for b in hardware["memory_banks"]),
                           cpu_regs=affinities)
    if (tree.at("/arm-io").properties != canonical.at("/arm-io").properties or
            tree.at("/arm-io/intc").encode() != canonical.at("/arm-io/intc").encode()):
        raise ValueError("SMP must retain umi GIC addresses and redistributor geometry")
    cpus = tree.at("/cpus")
    if cpus.encode() == canonical.at("/cpus").encode():
        if tree.at("/psci").encode() != canonical.at("/psci").encode():
            raise ValueError("unexpected existing PSCI conduit")
        return affinities
    if cpus.encode() != Node("cpus", children=[canonical.at("/cpus/cpu0")]).encode():
        raise ValueError("expected the qualified CPU0-only tree before SMP expansion")
    if any(node.name == "psci" for node in tree.children):
        raise ValueError("unexpected PSCI node in CPU0 baseline")
    original = tree.encode()
    cpus.children.extend(canonical.at("/cpus").children[1:])
    tree.children.append(canonical.at("/psci"))
    check = decode(tree.encode())
    check.at("/cpus").children = check.at("/cpus").children[:1]
    check.children = [node for node in check.children if node.name != "psci"]
    if check.encode() != original:
        raise ValueError("non-SMP ADT fields changed")
    return affinities


def build_tree(hardware, *, dram_size, cpu_regs=(0,), psci_method="smc",
               owned_memory_size=None, unique_chip_id=None, chip_id=None,
               root_match_volume_uuid=None, data_ramdisk=False,
               packed_data_ramdisk=False, trust_cache=False, ipad_identity=False):
    banks = hardware["memory_banks"]
    if dram_size != sum(bank["size"] for bank in banks):
        raise ValueError("--dram-size must match the observed umi bank total, 0x2f9900000")
    observed_cpus = {cpu["affinity"]: cpu for cpu in hardware["cpus"]}
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs)
            or any(cpu not in observed_cpus for cpu in cpu_regs)):
        raise ValueError("CPU affinities must be distinct observed cores, with CPU 0 first")
    if psci_method != hardware["psci"]["method"]:
        raise ValueError("umi uses the observed PSCI smc conduit")
    if owned_memory_size is not None and (
            not 0x10000000 <= owned_memory_size <= min(0x200000000, dram_size)
            or owned_memory_size % 0x200000):
        raise ValueError("owned memory request must be 256 MiB..8 GiB in 2 MiB steps")
    if (unique_chip_id is None) != (chip_id is None):
        raise ValueError("--unique-chip-id and --chip-id must be supplied together")
    if unique_chip_id is not None and not (
            0 < unique_chip_id < 1 << 64 and 0 < chip_id < 1 << 32):
        raise ValueError("identity seeds must be nonzero, fitting 64 and 32 bits")
    if data_ramdisk and packed_data_ramdisk:
        raise ValueError("dense and packed Data placeholders are mutually exclusive")
    chosen = {
        "random-seed": os.urandom(64),
        "osenvironment": string("main"),
        # Informational physical bank total, never a free/contiguous-memory map.
        "dram-base": u64(min(bank["base"] for bank in banks)),
        "dram-size": u64(dram_size),
        # Policy/algorithm selectors from the generic bring-up tree; not fuse claims.
        "crypto-hash-method": string("sha2-384"),
        "amfi-only-platform-code": u32(1),
        **empty_nvram_proxy(),
    }
    if unique_chip_id is not None:
        chosen.update({"unique-chip-id": u64(unique_chip_id), "chip-id": u32(chip_id)})
    if owned_memory_size is not None:
        chosen["darwinboot-memory-size"] = u64(owned_memory_size)
    if root_match_volume_uuid is not None:
        volume = uuid.UUID(root_match_volume_uuid)
        if not volume.int:
            raise ValueError("a zero UUID is not a qualified root volume")
        chosen["root-matching"] = string(
            "<dict><key>IOProviderClass</key><string>IOMedia</string>"
            "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
            + str(volume).upper() + "</string></dict></dict>")
    handoff = {}
    if data_ramdisk or packed_data_ramdisk:
        chosen["ephemeral-storage"] = u32(1)
        handoff["PackedRAMDisk" if packed_data_ramdisk else "RAMDisk"] = bytes(16)
    if trust_cache:
        handoff["TrustCache"] = bytes(16)
    cpus = [Node(f"cpu{index}", {
        "state": string("running" if index == 0 else "waiting"),
        "reg": u32(affinity), "device_type": string("cpu"),
        "compatible": b"".join(string(c) for c in observed_cpus[affinity]["compatible"]),
        "timebase-frequency": u32(hardware["timer"]["frequency_hz"]),
    }) for index, affinity in enumerate(cpu_regs)]
    gic = hardware["gic"]
    dist, redist = gic["distributor"], gic["redistributor"]
    # A logical translation window enclosing only the published GIC regions.
    # It does not reserve physical RAM or initialize the peripheral.
    bus_base = min(dist["base"], redist["base"])
    bus_size = max(dist["base"] + dist["size"], redist["base"] + redist["size"]) - bus_base
    children = [
        Node("chosen", chosen, [Node("memory-map", handoff), Node("manifest-properties")]),
        Node("cpus", children=cpus),
        Node("defaults", {"sleep-disabled": u32(1)}),
        Node("options"),
        Node("product", {"product-name": string(hardware["product_name"]),
                         "model": string(hardware["model"])}),
        Node("buttons", {"compatible": string("buttons"), "device_type": string("buttons"),
                         "button-names": b"hold\0voldown\0volup\0",
                         "home-button-type": u32(2)}),
        Node("arm-io", {
            "device_type": string("arm-io"), "#address-cells": u32(2), "#size-cells": u32(2),
            "ranges": struct.pack("<QQQ", 0, bus_base, bus_size),
        }, [Node("intc", {
            "compatible": string(gic["compatible"]),
            "reg": struct.pack("<QQQQ", dist["base"] - bus_base, dist["size"],
                               redist["base"] - bus_base, redist["size"]),
            "redistributor-stride": u64(gic["redistributor_stride"]),
        })]),
    ]
    if len(cpus) > 1:
        children.append(Node("psci", {"compatible": string(hardware["psci"]["compatible"]),
                                      "method": string(psci_method)}))
    tree = Node("device-tree", {
        "model": string(hardware["model"]),
        "compatible": b"".join(string(c) for c in hardware["compatible"]),
        "#address-cells": u32(2), "#size-cells": u32(2),
    }, children)
    if ipad_identity:
        apply_ipad_profile(tree)
    return tree


def summary(tree):
    chosen = tree.at("/chosen").properties
    soc = tree.at("/arm-io").properties
    gic = tree.at("/arm-io/intc").properties
    bus = struct.unpack("<QQQ", soc["ranges"])
    regions = struct.unpack("<QQQQ", gic["reg"])
    return {
        "cpu_affinities": [int.from_bytes(n.properties["reg"], "little")
                           for n in tree.at("/cpus").children],
        "timebase_hz": int.from_bytes(tree.at("/cpus/cpu0").properties["timebase-frequency"], "little"),
        "gic_distributor": hex(bus[1] + regions[0] - bus[0]),
        "gic_redistributor": hex(bus[1] + regions[2] - bus[0]),
        "gic_redistributor_stride": hex(int.from_bytes(gic["redistributor-stride"], "little")),
        "dram_base": hex(int.from_bytes(chosen["dram-base"], "little")),
        "dram_bank_total": hex(int.from_bytes(chosen["dram-size"], "little")),
        "loader_window_requested": hex(int.from_bytes(chosen["darwinboot-memory-size"], "little"))
        if "darwinboot-memory-size" in chosen else "loader default (256 MiB)",
        "random_seed_bytes": len(chosen["random-seed"]),
        "identity_seed_present": "unique-chip-id" in chosen,
        "nvram_proxy_bytes": len(chosen["nvram-proxy-data"]),
        "memory_map_placeholders": {k: len(v) for k, v in tree.at("/chosen/memory-map").properties.items()},
        "root_matching_present": "root-matching" in chosen,
    }
