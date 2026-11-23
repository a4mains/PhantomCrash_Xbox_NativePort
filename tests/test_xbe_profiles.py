import os
from pathlib import Path

import pytest

from phantomcrash_research import cli, convert, extract, game_tables, xbe_profile
from fixtures import build_xbe


def test_loads_europe_and_usa_profiles():
    profiles = xbe_profile.load_profiles()
    assert set(profiles) >= {"europe", "usa"}
    eu = profiles["europe"]
    us = profiles["usa"]
    assert eu.table(0x254480) == 0x254480
    assert us.table(0x254480) == 0x254398
    assert us.table(0x19F870) == 0x19F870
    assert us.table(0x1DB4A0) == 0x17CF88
    assert us.table(0x0D0F07) == 0x0D0E67
    assert us.text(0x193448) == 0x18FF00
    assert us.text(0x19B848) == 0x19B848 - 120
    assert us.text(0x1C34A8) == 0x1C34A8


def test_extract_from_source_rejects_missing_or_unsupported(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(FileNotFoundError):
        extract.extract_from_source(empty, tmp_path / "out")

    (empty / "default.xbe").write_bytes(build_xbe())
    (empty / "Media").mkdir()
    with pytest.raises(ValueError, match="Unsupported default.xbe"):
        extract.extract_from_source(empty, tmp_path / "out")


def test_real_usa_xbe_profile_and_text_refs():
    usa_path = os.environ.get("PHANTOM_USA_XBE")
    if not usa_path or not Path(usa_path).is_file():
        pytest.skip("PHANTOM_USA_XBE not set")
    raw = Path(usa_path).read_bytes()
    profile = xbe_profile.require_profile(raw)
    assert profile.id == "usa"
    tables = game_tables.load(raw)
    assert len(tables.resources) == 1927
    assert len(tables.sprites) == 3763
    assert tables.movies[7:9] == ["movie/dolby.sfd", "movie/phantagram.sfd"]
    image = game_tables._Image(raw, profile=profile)
    # Every {xbe:0x...} text reference in data/ resolves cleanly on USA XBE
    refs = set()
    for path in sorted((cli.REPOSITORY_ROOT / "data").rglob("*.json")):
        refs.update(int(m, 16) for m in convert.GAME_TEXT_REFERENCE.findall(path.read_text(encoding="utf-8")))
    assert len(refs) == 600
    decoded = [game_tables._plain(image.raw_string(image.text_va(addr))) for addr in refs]
    assert all(isinstance(s, str) for s in decoded)
