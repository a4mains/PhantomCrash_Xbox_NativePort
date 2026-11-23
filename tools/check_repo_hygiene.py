#!/usr/bin/env python3
"""Repository hygiene checker for PhantomCrash_Xbox_NativePort."""

from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys
from typing import Iterable

MAX_CPP_LINES = 400
LEGACY_CPP_LINE_CAPS = {
    "port/src/chip_shop.cpp": 403,
    "port/src/online/state_aggregate.hpp": 537,
}
GENERATED_CPP_EXEMPT = {
    "port/src/desktop_smaa_shaders.hpp",
    "port/src/desktop_world_shaders_msl.hpp",
    "port/src/desktop_world_shaders_spv.hpp",
    "port/src/extract/text_cp932_table.hpp",
}
FORBIDDEN_EXTENSIONS = {".xbe", ".iso", ".xiso", ".xmv", ".wma", ".afs"}
FORBIDDEN_NAMES = {"save.dat", "phantomcrash.sav"}


def check_cpp_line_budgets(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    for base in (root / "port" / "src", root / "port" / "include"):
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            rel = path.relative_to(root).as_posix()
            if "third_party/" in rel or rel in GENERATED_CPP_EXEMPT:
                continue
            limit = LEGACY_CPP_LINE_CAPS.get(rel, MAX_CPP_LINES)
            lines = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
            if lines > limit:
                errors.append(f"{rel}: {lines} lines exceeds limit ({limit})")
    return errors


def check_forbidden_tracked_assets(root: pathlib.Path, files: Iterable[str] | None = None) -> list[str]:
    errors: list[str] = []
    if files is None:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files"],
            capture_output=True,
            text=True,
        )
        if proc.returncode == 0:
            files = proc.stdout.splitlines()
        else:
            files = [p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()]
    for rel in files:
        p = pathlib.PurePosixPath(rel)
        if p.suffix.lower() in FORBIDDEN_EXTENSIONS or p.name.lower() in FORBIDDEN_NAMES:
            errors.append(f"Forbidden copyrighted/mutable asset tracked: {rel}")
    return errors


def check_registries_and_json(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    approx = root / "config" / "approximations.json"
    if not approx.is_file():
        errors.append("Missing config/approximations.json")
    else:
        try:
            data = json.loads(approx.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            errors.append(f"config/approximations.json: invalid JSON ({exc})")
            data = {}
        entries = data.get("approximations") if isinstance(data, dict) else None
        if not isinstance(entries, list):
            errors.append("config/approximations.json must contain an 'approximations' list")
        else:
            seen_ids: set[str] = set()
            for idx, entry in enumerate(entries):
                if not isinstance(entry, dict):
                    errors.append(f"config/approximations.json entry {idx} must be an object")
                    continue
                for key in ("id", "kind", "subsystem", "status", "title"):
                    if not isinstance(entry.get(key), str) or not entry[key].strip():
                        errors.append(f"config/approximations.json entry {idx} missing {key}")
                eid = entry.get("id")
                if isinstance(eid, str) and eid in seen_ids:
                    errors.append(f"config/approximations.json duplicate id {eid}")
                if isinstance(eid, str):
                    seen_ids.add(eid)

    symbols = root / "config" / "symbols.tsv"
    if not symbols.is_file():
        errors.append("Missing config/symbols.tsv")
    else:
        rows = symbols.read_text(encoding="utf-8").splitlines()
        if not rows or rows[0].split("\t") != ["eu_va", "kind", "subsystem", "symbol", "notes"]:
            errors.append("config/symbols.tsv has invalid header")
        seen_syms: set[str] = set()
        for idx, line in enumerate(rows[1:], start=2):
            if not line:
                continue
            cols = line.split("\t")
            if len(cols) != 5:
                errors.append(f"config/symbols.tsv line {idx} does not have 5 tab columns")
                continue
            if not re.fullmatch(r"0x[0-9a-fA-F]+", cols[0]):
                errors.append(f"config/symbols.tsv line {idx}: invalid hex eu_va {cols[0]!r}")
            if not cols[3].strip():
                errors.append(f"config/symbols.tsv line {idx}: empty symbol")
            elif cols[3] in seen_syms:
                errors.append(f"config/symbols.tsv line {idx}: duplicate symbol {cols[3]!r}")
            seen_syms.add(cols[3])

    for json_file in sorted((root / "data").rglob("*.json")) if (root / "data").exists() else []:
        try:
            json.loads(json_file.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            errors.append(f"{json_file.relative_to(root)}: invalid JSON ({exc})")
    return errors


def run_all_checks(root: pathlib.Path) -> list[str]:
    return (
        check_cpp_line_budgets(root)
        + check_forbidden_tracked_assets(root)
        + check_registries_and_json(root)
    )


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    errors = run_all_checks(root)
    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        return 1
    print("Repo hygiene OK: C++ line budgets, JSON registries, and asset rules verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
