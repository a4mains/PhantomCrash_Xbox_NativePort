#!/usr/bin/env python3
"""CLI converter between binary Xbox save slots and port JSON save files."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from phantomcrash_research.xbox_save import quest_to_xbox_slot, xbox_slot_to_quest  # noqa: E402


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Phantom Crash Xbox <-> port JSON save converter")
    sub = parser.add_subparsers(dest="command", required=True)

    p_json = sub.add_parser("to-json", help="Convert binary save slot to port JSON")
    p_json.add_argument("input", type=pathlib.Path)
    p_json.add_argument("output", type=pathlib.Path)
    p_json.add_argument("--no-verify", action="store_true")

    p_xbox = sub.add_parser("to-xbox", help="Convert port JSON save to binary save slot")
    p_xbox.add_argument("input", type=pathlib.Path)
    p_xbox.add_argument("output", type=pathlib.Path)

    p_info = sub.add_parser("inspect", help="Print decoded JSON summary of a binary save slot")
    p_info.add_argument("input", type=pathlib.Path)

    args = parser.parse_args(list(argv) if argv is not None else None)
    if args.command == "to-json":
        decoded = xbox_slot_to_quest(args.input.read_bytes(), verify_checksum=not args.no_verify)
        args.output.write_text(json.dumps(decoded, indent=2) + "\n", encoding="utf-8")
        return 0
    if args.command == "to-xbox":
        payload = json.loads(args.input.read_text(encoding="utf-8"))
        args.output.write_bytes(quest_to_xbox_slot(payload))
        return 0
    if args.command == "inspect":
        decoded = xbox_slot_to_quest(args.input.read_bytes())
        print(json.dumps(decoded, indent=2))
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
