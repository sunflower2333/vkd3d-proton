#!/usr/bin/env python3
"""Package existing Meson Windows UMD outputs; never build or execute them."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys


MACHINES = {"x86": 0x14c, "x64": 0x8664, "arm64": 0xaa64}
UMD_EXPORTS = {
    "OpenAdapter12", "VioGpuD3D12BridgeCreate", "VioGpuD3D12BridgeDestroy",
    "VioGpuD3D12BridgeStatus", "VioGpuD3D12BridgeObjectSize",
    "VioGpuD3D12BridgeGetTables", "VioGpuD3D12BridgeBindObject",
    "VioGpuD3D12BridgeUnbindObject",
}
ICD_EXPORTS = {
    "vk_icdGetInstanceProcAddr", "vk_icdGetPhysicalDeviceProcAddr",
    "vk_icdNegotiateLoaderICDInterfaceVersion",
}
PROBES = (
    "vkd3d-umd-ddi-abi-test", "vkd3d-umd-ddi-descriptor-test",
    "vkd3d-umd-gpu-probe", "vkd3d-umd-runtime-test",
    "vkd3d-umd-shared-gpu-probe", "vkd3d-system-d3d12-probe",
    "vkd3d-umd-fence-test",
)
CPU_SCRIPTS = (
    "test-windows-umd-runtime.ps1", "test-windows-umd-descriptors.ps1",
    "test-system-d3d12-probe.ps1",
)
BOUNDARY = (
    "Existing compiled artifacts were copied and their PE architecture, DLL exports "
    "and file hashes checked locally. Packaging performs no build, installation, "
    "registration, executable or PowerShell execution, device access, or VM action. "
    "The source hashes and dirty patch describe the workspace at packaging time; "
    "they do not prove that every binary was compiled from that exact snapshot. "
    "Meson metadata is retained as recorded build context. The caller supplies the "
    "paired ICD; matching architecture and exports alone do not prove its private "
    "runtime ABI or KMD compatibility. Native Windows x86/WOW64 loading, adapter "
    "selection, D3DKMT calls, private ICD negotiation, GPU execution, AIO DX12, "
    "physical pixels, cadence, zero-copy and performance remain unvalidated by "
    "this package. CPU fixtures and WARP validate their respective harnesses only."
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def pin(path):
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": sha256(path)}


def pe_info(path, architecture, dll):
    """Read PE headers/tables without loading the image or requiring a toolchain."""
    data = path.read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError(f"{path}: truncated PE structure")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError(f"{path}: missing DOS signature")
    pe = unpack("<I", 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: missing PE signature")
    machine, section_count = unpack("<HH", pe + 4)
    optional_size, characteristics = unpack("<HH", pe + 20)
    if machine != MACHINES[architecture]:
        raise ValueError(f"{path}: PE architecture {machine:#06x}, expected {architecture}")
    if bool(characteristics & 0x2000) != dll or not characteristics & 2:
        raise ValueError(f"{path}: incorrect DLL/executable characteristics")
    optional = pe + 24
    magic = unpack("<H", optional)[0]
    expected_magic = 0x10b if architecture == "x86" else 0x20b
    if magic != expected_magic:
        raise ValueError(f"{path}: incorrect PE optional-header format")
    directories = optional + (96 if magic == 0x10b else 112)
    if optional_size < directories - optional + 16:
        raise ValueError(f"{path}: missing PE data directories")
    sections = []
    for i in range(section_count):
        row = optional + optional_size + i * 40
        virtual_size, rva, raw_size, raw = unpack("<IIII", row + 8)
        sections.append((rva, virtual_size, raw_size, raw))

    def offset(rva, size=1):
        for base, virtual, raw_size, raw in sections:
            if base <= rva and rva + size <= base + max(virtual, raw_size):
                result = raw + rva - base
                if rva + size > base + raw_size or result + size > len(data):
                    break
                return result
        raise ValueError(f"{path}: PE RVA outside file-backed sections")

    def string(rva):
        start = offset(rva)
        end = data.find(b"\0", start, min(start + 4096, len(data)))
        if end < 0:
            raise ValueError(f"{path}: unterminated PE string")
        return data[start:end].decode("ascii")

    exports = []
    export_rva, export_size = unpack("<II", directories)
    if export_rva:
        table = offset(export_rva, 40)
        count = unpack("<I", table + 24)[0]
        names = unpack("<I", table + 32)[0]
        name_table = offset(names, count * 4) if count else 0
        exports = [string(unpack("<I", name_table + i * 4)[0]) for i in range(count)]
    imports = []
    import_rva, import_size = unpack("<II", directories + 8)
    if import_rva:
        for index in range(import_size // 20):
            row = unpack("<IIIII", offset(import_rva + index * 20, 20))
            if not any(row):
                break
            imports.append(string(row[3]))
        else:
            raise ValueError(f"{path}: unterminated import table")
    return {"machine": f"0x{machine:04x}", "architecture": architecture,
            "dll": dll, "exports": exports, "imports": imports}


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args])


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--architecture", choices=MACHINES, required=True)
    parser.add_argument("--paired-icd", type=Path, required=True)
    parser.add_argument("--runtime-test", type=Path,
                        help="optional isolated runtime-test executable")
    parser.add_argument("--runtime-test-build-record", type=Path,
                        help="required compile/link provenance for an isolated runtime-test")
    parser.add_argument("--output", type=Path, required=True,
                        help="fresh package directory; existing paths are rejected")
    parser.add_argument("--review-notes", type=Path,
                        help="optional source/ICD review to include verbatim")
    args = parser.parse_args()
    if bool(args.runtime_test) != bool(args.runtime_test_build_record):
        parser.error("--runtime-test and --runtime-test-build-record must be supplied together")
    build = args.build_dir.resolve(strict=True)
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    metadata = build / "meson-info/meson-info.json"
    source = Path(json.loads(metadata.read_text())["directories"]["source"]).resolve(strict=True)
    binaries = build / "libs/vkd3d-umd"
    icd_name = f"viogpud3d12_vk_{args.architecture}.dll"
    copies = {"viogpud3d12.dll": binaries / "viogpud3d12.dll",
              icd_name: args.paired_icd.resolve(strict=True)}
    importlibs = [binaries / name for name in ("viogpud3d12.dll.a", "viogpud3d12.lib")
                  if (binaries / name).is_file()]
    if len(importlibs) != 1:
        raise ValueError("expected exactly one GNU or MSVC UMD import library")
    copies[importlibs[0].name] = importlibs[0]
    copies.update({name + ".exe": binaries / (name + ".exe") for name in PROBES})
    if args.runtime_test:
        runtime_test = args.runtime_test.resolve(strict=True)
        runtime_record = args.runtime_test_build_record.resolve(strict=True)
        record = json.loads(runtime_record.read_text(encoding="utf-8"))
        recorded_runtime = record.get("files", {}).get(str(runtime_test), {})
        if (recorded_runtime.get("sha256") != sha256(runtime_test) or
                recorded_runtime.get("bytes") != runtime_test.stat().st_size):
            raise ValueError("isolated runtime-test build record does not match executable")
        copies["vkd3d-umd-runtime-test.exe"] = runtime_test
        copies["RUNTIME-TEST-BUILD.json"] = runtime_record
    copies.update({name: source / "tools" / name for name in CPU_SCRIPTS})
    copies["README.md"] = source / "libs/vkd3d-umd/README.md"
    if args.review_notes:
        copies["SOURCE-REVIEW.md"] = args.review_notes.resolve(strict=True)
    if (binaries / "viogpud3d12.pdb").is_file():
        copies["viogpud3d12.pdb"] = binaries / "viogpud3d12.pdb"
    inputs = {name: pin(path) for name, path in copies.items()}
    pe = {}
    for name, path in copies.items():
        suffix = Path(name).suffix.lower()
        if suffix in (".dll", ".exe"):
            pe[name] = pe_info(path, args.architecture, suffix == ".dll")
    for name, expected in (("viogpud3d12.dll", UMD_EXPORTS), (icd_name, ICD_EXPORTS)):
        if set(pe[name]["exports"]) != expected:
            raise ValueError(f"{name}: unexpected PE exports: {pe[name]['exports']}")
    if icd_name.encode("utf-16le") not in copies["viogpud3d12.dll"].read_bytes():
        raise ValueError(f"UMD does not contain the expected paired ICD basename: {icd_name}")
    # The ordinary System32 D3D12 probe must not statically import a private engine.
    private = ("viogpu", "vkd3d", "vulkan", "dxvk")
    if any(any(tag in name.lower() for tag in private)
           for name in pe["vkd3d-system-d3d12-probe.exe"]["imports"]):
        raise ValueError("system D3D12 probe imports a private graphics engine")

    head = git(source, "rev-parse", "HEAD").decode().strip()
    status = git(source, "status", "--porcelain=v1", "--untracked-files=all")
    patch = git(source, "diff", "--binary", "HEAD")
    submodules = git(source, "submodule", "status", "--recursive").decode()
    paths = git(source, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0")
    source_hashes = {}
    for raw in paths:
        if raw:
            name = raw.decode("utf-8", errors="surrogateescape")
            path = source / name
            if path.is_file():
                source_hashes[name] = {"bytes": path.stat().st_size, "sha256": sha256(path)}

    output.mkdir(parents=True, exist_ok=False)
    provenance = output / "provenance"
    provenance.mkdir()
    for name, path in copies.items():
        shutil.copyfile(path, output / name)
        if sha256(output / name) != inputs[name]["sha256"]:
            raise ValueError(f"input changed while copying: {path}")
    (provenance / "workspace-status.txt").write_bytes(status)
    (provenance / "workspace.patch").write_bytes(patch)
    (provenance / "submodules.txt").write_text(submodules, encoding="utf-8")
    save_json(provenance / "source-hashes.json", source_hashes)
    build_files = [metadata, build / "meson-private/cmd_line.txt", build / "build.ninja",
                   build / "compile_commands.json", build / ".ninja_log"]
    build_files += sorted((build / "meson-info").glob("intro-*.json"))
    recorded = {}
    for path in build_files:
        if path.is_file():
            name = path.relative_to(build).as_posix()
            target = provenance / "build" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            recorded[name] = pin(target)
    shutil.copyfile(Path(__file__), provenance / "package-windows-umd.py")
    save_json(output / "pe-inspection.json", pe)
    save_json(output / "source.json", {
        "schema": "vkd3d-windows-umd-local-package-v1",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "architecture": args.architecture, "source_directory": str(source),
        "source_head": head, "source_dirty": bool(status), "build_directory": str(build),
        "inputs": inputs, "build_metadata": recorded,
        "paired_icd_runtime_basename": icd_name,
        "native_runtime_validated": False, "build_performed": False,
        "boundary": BOUNDARY,
    })
    (output / "VALIDATION-BOUNDARY.txt").write_text(BOUNDARY + "\n", encoding="utf-8")
    # Recheck copied inputs after provenance capture; a concurrent rebuild must fail.
    for name, path in copies.items():
        if sha256(path) != inputs[name]["sha256"]:
            raise ValueError(f"input changed during packaging: {path}")
    files = sorted(path for path in output.rglob("*") if path.is_file())
    (output / "SHA256SUMS").write_text("".join(
        f"{sha256(path)}  {path.relative_to(output).as_posix()}\n" for path in files), encoding="utf-8")
    print(json.dumps({"output": str(output), "architecture": args.architecture,
                      "files": len(files) + 1, "dll": inputs["viogpud3d12.dll"],
                      "paired_icd": inputs[icd_name],
                      "sha256sums": pin(output / "SHA256SUMS"),
                      "native_runtime_validated": False}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as exc:
        print(f"package-windows-umd: {exc}", file=sys.stderr)
        sys.exit(1)
