#!/usr/bin/env python3
"""Regression checks for the WF_MAXON patch-resilient offset refresh."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).resolve().parent / "refresh_offsets.py"
SIGNATURES = Path(__file__).resolve().parent / "offset_signatures.json"
BINARY = Path(r"D:\SteamLibrary\steamapps\common\Warframe\Warframe.x64.exe")


def fail(message: str) -> int:
    print(f"FAIL: {message}")
    return 1


def main() -> int:
    if not BINARY.is_file():
        print(f"SKIP: current installed binary not found: {BINARY}")
        return 0

    with tempfile.TemporaryDirectory(prefix="wf_maxon_refresh_test_") as temp_name:
        temp = Path(temp_name)
        header = temp / "warframe_offsets.h"
        reports = temp / "reports"
        command = [
            sys.executable,
            str(SCRIPT),
            "--binary",
            str(BINARY),
            "--signatures",
            str(SIGNATURES),
            "--header",
            str(header),
            "--report-dir",
            str(reports),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            return fail(f"current-build refresh failed:\n{result.stdout}\n{result.stderr}")
        if not header.is_file():
            return fail("current-build refresh did not write a header")
        report = json.loads((reports / "wf_maxon_offsets_latest.json").read_text(encoding="utf-8"))
        required = [item for item in report["signatures"] if item["required"]]
        if not required or any(item["status"] != "resolved_unique" for item in required):
            return fail("a required current-build signature was not unique")
        damage = next(item for item in report["signatures"] if item["name"] == "damage_result")
        if damage["status"] != "resolved_unique":
            return fail(f"damage_result did not resolve uniquely: {damage['status']} ({damage['hits']} hits)")
        if damage["rva"] != 0x167E9A2:
            return fail(f"damage_result target RVA drifted: {damage['rva']!r}")
        if damage["branch_target_rva"] != 0x167E9CC:
            return fail(f"damage_result branch target drifted: {damage['branch_target_rva']!r}")
        print("PASS: current-build refresh resolves the semantic damage result point and branch target")

        bad_signatures = temp / "bad_signatures.json"
        library = json.loads(SIGNATURES.read_text(encoding="utf-8"))
        library["signatures"][0]["pattern"] = "00 00 00 00 00 00 00 00 00 00 00 00"
        bad_signatures.write_text(json.dumps(library), encoding="utf-8")
        sentinel = "sentinel header must survive a failed refresh\n"
        header.write_text(sentinel, encoding="utf-8")
        bad_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--binary",
                str(BINARY),
                "--signatures",
                str(bad_signatures),
                "--header",
                str(header),
                "--report-dir",
                str(temp / "bad_reports"),
            ],
            capture_output=True,
            text=True,
        )
        if bad_result.returncode != 2:
            return fail(f"expected required-signature failure exit 2, got {bad_result.returncode}")
        if header.read_text(encoding="utf-8") != sentinel:
            return fail("failed refresh replaced the existing generated header")
        print("PASS: required-signature failure leaves the existing header unchanged")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
