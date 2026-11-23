from __future__ import annotations

import json
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"
TOOLS_DIR = REPO_ROOT / "tools"
for path in (SRC_DIR, TOOLS_DIR):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from convert_xbox_save import main as cli_main  # noqa: E402
from phantomcrash_research.xbox_save import (  # noqa: E402
    OFF_CRC32,
    SLOT_SIZE,
    XboxSaveError,
    quest_to_xbox_slot,
    xbox_slot_to_quest,
)


def test_cleared_save_roundtrip() -> None:
    sample = json.loads((REPO_ROOT / "port" / "tests" / "cleared_save.json").read_text("utf-8"))
    slot = quest_to_xbox_slot(sample)
    assert len(slot) == SLOT_SIZE
    decoded = xbox_slot_to_quest(slot)
    for key in ("player_name", "seen_events", "day", "money", "cleared", "riding", "saved"):
        assert decoded["quest"][key] == sample["quest"][key]


def test_extended_fields_and_tamper_rejection(tmp_path: pathlib.Path) -> None:
    payload = {
        "quest": {
            "player_name": "Pilot01",
            "seen_events": [1, 48, 129, 300],
            "day": 19,
            "money": 150_000_000,  # clamped to 99,999,999
            "cleared": True,
            "riding": "Scoobee",
            "saved": "2026-11-22 12:34:56",
            "spin_paid": 40,
            "total_days": 85,
            "laps": 2,
            "won_contests": [0, 3, 7],
            "records": {"battles": 14, "shots_down": 28, "prize_money": 90000},
        }
    }
    in_json = tmp_path / "in.json"
    out_sav = tmp_path / "slot.sav"
    out_json = tmp_path / "out.json"
    in_json.write_text(json.dumps(payload), encoding="utf-8")

    assert cli_main(["to-xbox", str(in_json), str(out_sav)]) == 0
    assert cli_main(["to-json", str(out_sav), str(out_json)]) == 0
    roundtrip = json.loads(out_json.read_text(encoding="utf-8"))["quest"]
    assert roundtrip["money"] == 99_999_999
    assert roundtrip["cleared"] is True
    assert roundtrip["spin_paid"] == 40
    assert roundtrip["total_days"] == 85
    assert roundtrip["laps"] == 2
    assert roundtrip["won_contests"] == [0, 3, 7]
    assert roundtrip["records"]["battles"] == 14

    corrupt = bytearray(out_sav.read_bytes())
    corrupt[0x2810] ^= 0xFF
    with pytest.raises(XboxSaveError, match="CRC32"):
        xbox_slot_to_quest(bytes(corrupt))
    corrupt[OFF_CRC32] ^= 1
    with pytest.raises(XboxSaveError, match="CRC32"):
        xbox_slot_to_quest(bytes(corrupt))
    assert cli_main(["to-json", str(out_sav), str(out_json), "--no-verify"]) == 0
    bad_ts = dict(payload)
    bad_ts["quest"] = dict(payload["quest"], saved="not-a-date")
    with pytest.raises(XboxSaveError, match="timestamp"):
        quest_to_xbox_slot(bad_ts)
    with pytest.raises(XboxSaveError, match="too small"):
        xbox_slot_to_quest(b"short")
    with pytest.raises(XboxSaveError, match="magic"):
        xbox_slot_to_quest(b"BADMAGIC" + b"\x00" * (SLOT_SIZE - 8))
