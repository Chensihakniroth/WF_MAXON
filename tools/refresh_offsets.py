#!/usr/bin/env python3
"""Refresh WF_MAXON's current-build code RVAs from Warframe.x64.exe.

This tool only emits offsets after a signature has exactly one hit in the
current executable.  Zero-hit and multi-hit signatures are reported and kept
unresolved; they never fall back to a stale RVA.  The generated header is
consumed by WF_MAXON/src/engine.cpp and is safe to regenerate after a patch.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Any

TOOL_VERSION = "1.0.0"
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
DEFAULT_SIGNATURES = SCRIPT_DIR / "offset_signatures.json"
DEFAULT_HEADER = PROJECT_ROOT / "src" / "generated" / "warframe_offsets.h"
DEFAULT_REPORT_DIR = PROJECT_ROOT / "offsets"
DEFAULT_BINARY_CANDIDATES = (
    Path(r"D:\SteamLibrary\steamapps\common\Warframe\Warframe.x64.exe"),
    PROJECT_ROOT.parent / "bin" / "Warframe.x64.exe",
)


def load_pe(path: Path):
    try:
        import pefile
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise SystemExit(
            "ERROR: pefile is not installed. Run: python -m pip install pefile"
        ) from exc

    if not path.is_file():
        raise SystemExit(f"ERROR: Warframe binary not found: {path}")
    raw = path.read_bytes()
    pe = pefile.PE(data=raw, fast_load=False)
    pe.parse_data_directories()
    if pe.FILE_HEADER.Machine != 0x8664:
        raise SystemExit(
            f"ERROR: expected PE32+ x64 (0x8664), got 0x{pe.FILE_HEADER.Machine:X}"
        )
    timestamp = int(pe.FILE_HEADER.TimeDateStamp)
    try:
        timestamp_utc = dt.datetime.fromtimestamp(
            timestamp, dt.timezone.utc
        ).isoformat()
    except (OverflowError, OSError, ValueError):
        timestamp_utc = str(timestamp)
    sha256 = hashlib.sha256(raw).hexdigest()
    meta = {
        "path": str(path.resolve()),
        "size": len(raw),
        "sha256": sha256,
        "sha256_short": sha256[:16],
        "machine": hex(int(pe.FILE_HEADER.Machine)),
        "pe_timestamp": timestamp,
        "pe_timestamp_hex": hex(timestamp),
        "pe_timestamp_utc": timestamp_utc,
        "image_base": hex(int(pe.OPTIONAL_HEADER.ImageBase)),
        "size_of_image": int(pe.OPTIONAL_HEADER.SizeOfImage),
        "size_of_image_hex": hex(int(pe.OPTIONAL_HEADER.SizeOfImage)),
    }
    return pe, raw, meta


def section_name(section: Any) -> str:
    return section.Name.rstrip(b"\0").decode("latin-1", "replace")


def section_ranges(pe: Any, raw_size: int, names: list[str]):
    wanted = {name.lower() for name in names}
    for section in pe.sections:
        name = section_name(section)
        if name.lower() not in wanted:
            continue
        start = int(section.PointerToRawData)
        end = min(raw_size, start + int(section.SizeOfRawData))
        if end > start:
            yield name, start, end


def pattern_regex(pattern: str) -> tuple[bytes, int]:
    tokens = pattern.split()
    if not tokens:
        raise ValueError("empty pattern")
    output = bytearray()
    for token in tokens:
        if token in ("?", "??"):
            output.extend(b".")
        elif re.fullmatch(r"[0-9A-Fa-f]{2}", token):
            # Escape every literal byte. Values such as 0x28/0x29 are regex
            # metacharacters and must not become accidental regex syntax.
            output.extend(re.escape(bytes([int(token, 16)])))
        else:
            raise ValueError(f"invalid pattern token {token!r}")
    return bytes(output), len(tokens)


def find_pattern(raw: bytes, pattern: str, ranges) -> list[dict[str, Any]]:
    regex, _ = pattern_regex(pattern)
    hits: list[dict[str, Any]] = []
    for name, start, end in ranges:
        for match in re.finditer(regex, raw[start:end], re.DOTALL):
            hits.append({"file_offset": start + match.start(), "section": name})
    hits.sort(key=lambda item: item["file_offset"])
    return hits


def file_to_rva(pe: Any, file_offset: int) -> int | None:
    for section in pe.sections:
        start = int(section.PointerToRawData)
        end = start + int(section.SizeOfRawData)
        if start <= file_offset < end:
            return int(section.VirtualAddress) + file_offset - start
    return None


def parse_int(value: Any, field_name: str, default: int = 0) -> int:
    if value is None:
        return default
    try:
        if isinstance(value, str):
            return int(value, 0)
        return int(value)
    except (TypeError, ValueError) as exc:
        raise SystemExit(f"ERROR: invalid {field_name}: {value!r}") from exc


def branch_target_from_patch(definition: dict[str, Any], target_rva: int,
                             patch_bytes: bytes) -> int:
    spec = definition.get("branch_target_from_patch")
    if not spec:
        return parse_int(definition.get("branch_target_rva"), "branch_target_rva")
    if not isinstance(spec, dict):
        raise SystemExit("ERROR: branch_target_from_patch must be an object")
    offset = parse_int(spec.get("offset"), "branch_target_from_patch.offset")
    kind = str(spec.get("kind", "rel8")).lower()
    if offset < 0 or offset >= len(patch_bytes):
        raise SystemExit("ERROR: branch_target_from_patch offset is outside patch bytes")
    opcode = patch_bytes[offset]
    expected_opcode = spec.get("opcode")
    if expected_opcode is not None and opcode != parse_int(expected_opcode, "branch_target_from_patch.opcode"):
        raise SystemExit(
            "ERROR: branch_target_from_patch opcode mismatch "
            f"(expected 0x{parse_int(expected_opcode, 'branch_target_from_patch.opcode'):02X}, "
            f"got 0x{opcode:02X})"
        )
    if kind == "rel8":
        if offset + 2 > len(patch_bytes):
            raise SystemExit("ERROR: rel8 branch displacement is outside patch bytes")
        displacement = int.from_bytes(patch_bytes[offset + 1:offset + 2], "little", signed=True)
        return target_rva + offset + 2 + displacement
    if kind == "rel32":
        if offset + 5 > len(patch_bytes):
            raise SystemExit("ERROR: rel32 branch displacement is outside patch bytes")
        displacement = int.from_bytes(patch_bytes[offset + 1:offset + 5], "little", signed=True)
        return target_rva + offset + 5 + displacement
    raise SystemExit(f"ERROR: unsupported branch displacement kind: {kind}")


def c_suffix(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def byte_list(data: bytes) -> str:
    return ", ".join(f"0x{value:02X}" for value in data)


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def make_header(meta: dict[str, Any], results: list[dict[str, Any]]) -> str:
    lines = [
        "// ============================================================================",
        "// WF_MAXON current-build offset manifest (AUTO-GENERATED)",
        f"// Tool: refresh_offsets.py v{TOOL_VERSION}",
        f"// Generated: {dt.datetime.now().isoformat(timespec='seconds')}",
        f"// Binary: {meta['path']}",
        f"// SHA-256: {meta['sha256']}",
        f"// PE timestamp: {meta['pe_timestamp_hex']}",
        f"// SizeOfImage: {meta['size_of_image_hex']}",
        "// Do not hand-edit. Re-run WF_MAXON\\tools\\refresh_offsets.py after a patch.",
        "// An unresolved entry is deliberately fail-closed at runtime.",
        "// ============================================================================",
        "#pragma once",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace wfmaxon_offsets {",
        f"inline constexpr std::uintptr_t kExpectedImageSize = 0x{meta['size_of_image']:X}ULL;",
        f"inline constexpr std::uint32_t kExpectedPeTimestamp = 0x{meta['pe_timestamp']:08X}U;",
        f"inline constexpr char kBinarySha256[] = \"{meta['sha256']}\";",
        f"inline constexpr char kBinaryPath[] = \"{meta['path'].replace(chr(92), '/') }\";",
        "",
    ]

    for result in results:
        suffix = c_suffix(result["name"])
        patch_length = int(result["patch_length"])
        rva = result.get("rva") or 0
        branch_target_rva = result.get("branch_target_rva") or 0
        resolved = result["status"] == "resolved_unique"
        data = bytes.fromhex(result["patch_bytes_hex"]) if resolved else bytes(patch_length)
        if len(data) != patch_length:
            data = (data + bytes(patch_length))[:patch_length]
        lines.extend(
            [
                f"// {result['name']}: {result['status']} ({result['hits']} hit(s)); {result['notes']}",
                f"inline constexpr std::uintptr_t k{suffix}Rva = 0x{int(rva):X}ULL;",
                f"inline constexpr std::uintptr_t k{suffix}BranchTargetRva = 0x{int(branch_target_rva):X}ULL;",
                f"inline constexpr bool k{suffix}Resolved = {'true' if resolved else 'false'};",
                f"inline constexpr std::size_t k{suffix}PatchLength = {patch_length};",
                f"inline constexpr std::array<std::uint8_t, {patch_length}> k{suffix}Prologue = {{{byte_list(data)}}};",
                "",
            ]
        )

    lines.extend(["} // namespace wfmaxon_offsets", ""])
    return "\n".join(lines)


def choose_binary(argument: str | None) -> Path:
    if argument:
        return Path(argument)
    for candidate in DEFAULT_BINARY_CANDIDATES:
        if candidate.is_file():
            return candidate
    candidates = ", ".join(str(item) for item in DEFAULT_BINARY_CANDIDATES)
    raise SystemExit(f"ERROR: no Warframe binary found; checked: {candidates}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Resolve WF_MAXON code offsets against the exact current Warframe binary"
    )
    parser.add_argument("--binary", help="Warframe.x64.exe to scan")
    parser.add_argument("--signatures", default=str(DEFAULT_SIGNATURES))
    parser.add_argument("--header", default=str(DEFAULT_HEADER))
    parser.add_argument("--report-dir", default=str(DEFAULT_REPORT_DIR))
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="scan and report without replacing the generated header",
    )
    args = parser.parse_args(argv)

    binary = choose_binary(args.binary)
    signature_path = Path(args.signatures)
    if not signature_path.is_file():
        raise SystemExit(f"ERROR: signature database not found: {signature_path}")
    library = json.loads(signature_path.read_text(encoding="utf-8"))
    if not isinstance(library.get("signatures"), list):
        raise SystemExit("ERROR: signature database must contain a signatures list")

    pe, raw, meta = load_pe(binary)
    results: list[dict[str, Any]] = []
    required_failures: list[str] = []
    for definition in library["signatures"]:
        name = str(definition["name"])
        pattern = str(definition["pattern"])
        patch_length = int(definition["patch_length"])
        if patch_length <= 0:
            raise SystemExit(f"ERROR: {name}: patch_length must be positive")
        try:
            _, pattern_length = pattern_regex(pattern)
        except ValueError as exc:
            raise SystemExit(f"ERROR: {name}: {exc}") from exc
        if patch_length > pattern_length and not definition.get("allow_patch_tail", True):
            raise SystemExit(
                f"ERROR: {name}: patch_length exceeds signature length and tail is disabled"
            )
        ranges = list(
            section_ranges(
                pe,
                len(raw),
                [str(item) for item in definition.get("sections", [".text"])],
            )
        )
        hits = find_pattern(raw, pattern, ranges)
        target_adjust = parse_int(definition.get("target_adjust"), "target_adjust")
        hit_items = []
        for hit in hits:
            pattern_file_offset = int(hit["file_offset"])
            target_file_offset = pattern_file_offset + target_adjust
            target_rva = (
                file_to_rva(pe, target_file_offset)
                if 0 <= target_file_offset < len(raw)
                else None
            )
            patch_bytes = raw[target_file_offset:target_file_offset + patch_length]
            target_valid = (
                target_rva is not None and
                len(patch_bytes) == patch_length
            )
            hit_items.append(
                {
                    "file_offset": hex(pattern_file_offset),
                    "rva": hex(target_rva) if target_rva is not None else None,
                    "target_file_offset": hex(target_file_offset),
                    "target_adjust": target_adjust,
                    "section": hit["section"],
                    "target_valid": target_valid,
                    "patch_bytes_hex": patch_bytes.hex(" "),
                }
            )
        status = (
            "resolved_unique"
            if len(hit_items) == 1 and hit_items[0]["target_valid"]
            else "dead"
            if len(hit_items) == 0
            else "invalid_target"
            if len(hit_items) == 1
            else "ambiguous"
        )
        branch_target_rva = 0
        if status == "resolved_unique":
            target_rva = int(hit_items[0]["rva"], 16)
            patch_bytes = bytes.fromhex(hit_items[0]["patch_bytes_hex"])
            branch_target_rva = branch_target_from_patch(definition, target_rva, patch_bytes)
        result = {
            "name": name,
            "pattern": pattern,
            "target_adjust": target_adjust,
            "patch_length": patch_length,
            "required": bool(definition.get("required", False)),
            "notes": str(definition.get("notes", "")),
            "hits": len(hit_items),
            "status": status,
            "candidates": hit_items[:50],
            "rva": int(hit_items[0]["rva"], 16) if status == "resolved_unique" else None,
            "branch_target_rva": branch_target_rva,
            "patch_bytes_hex": hit_items[0]["patch_bytes_hex"] if status == "resolved_unique" else "",
        }
        results.append(result)
        if result["required"] and status != "resolved_unique":
            required_failures.append(f"{name}={status} ({len(hit_items)} hits)")

    report = {
        "schema_version": 1,
        "tool_version": TOOL_VERSION,
        "generated": dt.datetime.now().isoformat(timespec="seconds"),
        "binary": meta,
        "signature_database": {
            "path": str(signature_path.resolve()),
            "sha256": hashlib.sha256(signature_path.read_bytes()).hexdigest(),
        },
        "signatures": results,
        "required_failures": required_failures,
        "header_replaced": not bool(required_failures) and not args.check_only,
    }

    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    atomic_write(report_dir / f"wf_maxon_offsets_{stamp}.json", serialized)
    atomic_write(report_dir / "wf_maxon_offsets_latest.json", serialized)

    print("=" * 86)
    print("WF_MAXON offset refresh")
    print("=" * 86)
    print(f"Binary: {meta['path']}")
    print(f"SHA-256: {meta['sha256']}")
    print(f"PE: timestamp={meta['pe_timestamp_hex']} SizeOfImage={meta['size_of_image_hex']}")
    print()
    print(f"{'name':<22} {'hits':>5}  {'status':<18}  {'rva':>12}")
    print("-" * 86)
    for result in results:
        rva = f"0x{result['rva']:X}" if result["rva"] is not None else "-"
        print(
            f"{result['name']:<22} {result['hits']:>5}  "
            f"{result['status']:<18}  {rva:>12}"
        )
    print(f"\nReports: {report_dir}")

    if required_failures:
        print("\nERROR: required signatures did not resolve uniquely:")
        for failure in required_failures:
            print(f"  - {failure}")
        print("Header was left unchanged. No stale RVA was promoted.")
        return 2

    if args.check_only:
        print("\n[OK] Check-only scan passed; generated header was not changed.")
    else:
        header = Path(args.header)
        atomic_write(header, make_header(meta, results))
        print(f"[OK] Generated header: {header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
