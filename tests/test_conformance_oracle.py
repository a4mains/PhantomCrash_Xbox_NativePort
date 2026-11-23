from __future__ import annotations

import os
import pathlib
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

pytest.importorskip("unicorn")

from conformance_oracle import (  # noqa: E402
    X86Oracle,
    inspect_xbe_in_oracle,
    verify_synthetic_suite,
)


def test_synthetic_x86_oracle() -> None:
    result = verify_synthetic_suite()
    assert result["status"] == "ok"
    assert result["synthetic_checks"] == 3


def test_overlapping_map_and_runaway_detection() -> None:
    oracle = X86Oracle()
    oracle.map_range(0x00401000, 0x3000)
    oracle.map_range(0x00402800, 0x3000)
    oracle.write_bytes(0x00404000, b"\xeb\xfe")  # jmp $
    with pytest.raises(RuntimeError, match="did not return"):
        oracle.call_cdecl(0x00404000, max_steps=16, require_return=True)
    res = oracle.call_cdecl(0x00404000, max_steps=16, require_return=False)
    assert res.returned is False


def test_retail_xbe_loads_into_unicorn() -> None:
    xbe_path = os.environ.get("PHANTOM_USA_XBE") or os.environ.get("PHANTOM_EU_XBE")
    if not xbe_path or not pathlib.Path(xbe_path).is_file():
        pytest.skip("Set PHANTOM_USA_XBE or PHANTOM_EU_XBE to run retail XBE oracle checks")
    info = inspect_xbe_in_oracle(pathlib.Path(xbe_path))
    assert info["mapped_magic"] == "XBEH"
    assert info["title_id"] in {"0x504c0001", "0x474b0001"}
    assert info["title_code"] in {"PL-001", "GK-001"}
    assert int(info["text_size"]) > 0x100000
    assert int(info["section_count"]) >= 5
