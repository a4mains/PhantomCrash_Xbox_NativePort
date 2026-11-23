"""Per-release XBE address profiles for multi-region asset extraction."""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path

_DEFAULT_PROFILES_JSON = Path(__file__).resolve().parents[2] / "data" / "xbe_profiles.json"


@dataclass(frozen=True)
class AddressRange:
    start: int
    end: int
    delta: int


@dataclass(frozen=True)
class XbeProfile:
    id: str
    label: str
    sha256: str
    artifacts_dir: str
    table_ranges: tuple[AddressRange, ...] = ()
    table_overrides: dict[int, int] = field(default_factory=dict)
    text_ranges: tuple[AddressRange, ...] = ()
    text_overrides: dict[int, int] = field(default_factory=dict)

    def table(self, canonical_va: int) -> int:
        if canonical_va in self.table_overrides:
            return self.table_overrides[canonical_va]
        for r in self.table_ranges:
            if r.start <= canonical_va < r.end:
                return canonical_va + r.delta
        return canonical_va

    def text(self, canonical_va: int) -> int:
        if canonical_va in self.text_overrides:
            return self.text_overrides[canonical_va]
        for r in self.text_ranges:
            if r.start <= canonical_va < r.end:
                return canonical_va + r.delta
        return canonical_va


EUROPE_PROFILE = XbeProfile(
    id="europe",
    label="Phantom Crash (Europe / PAL)",
    sha256="bdbe2c3eb2361395babfe6095d1a2da72f6581dccb34812b8d45e084c6c903c6",
    artifacts_dir="europe",
    table_ranges=(AddressRange(0x000000, 0xFFFFFFFF, 0),),
    text_ranges=(AddressRange(0x000000, 0xFFFFFFFF, 0),),
)


def _parse_profile(key: str, raw: dict) -> XbeProfile:
    return XbeProfile(
        id=raw.get("id", key),
        label=raw.get("label", key),
        sha256=raw["sha256"].lower(),
        artifacts_dir=raw.get("artifacts_dir", key),
        table_ranges=tuple(
            AddressRange(int(start, 16), int(end, 16), int(delta))
            for start, end, delta in raw.get("table_ranges", ())
        ),
        table_overrides={
            int(k, 16): int(v, 16) for k, v in raw.get("table_overrides", {}).items()
        },
        text_ranges=tuple(
            AddressRange(int(start, 16), int(end, 16), int(delta))
            for start, end, delta in raw.get("text_ranges", ())
        ),
        text_overrides={
            int(k, 16): int(v, 16) for k, v in raw.get("text_overrides", {}).items()
        },
    )


def load_profiles(profiles_json: Path | None = None) -> dict[str, XbeProfile]:
    path = profiles_json or _DEFAULT_PROFILES_JSON
    if not path.is_file():
        return {"europe": EUROPE_PROFILE}
    doc = json.loads(path.read_text(encoding="utf-8"))
    return {
        key: _parse_profile(key, raw) for key, raw in doc.get("profiles", {}).items()
    }


def match_profile(data: bytes, profiles_json: Path | None = None) -> XbeProfile | None:
    digest = hashlib.sha256(data).hexdigest().lower()
    for profile in load_profiles(profiles_json).values():
        if profile.sha256 == digest:
            return profile
    return None


def require_profile(data: bytes, profiles_json: Path | None = None) -> XbeProfile:
    profile = match_profile(data, profiles_json)
    if profile is not None:
        return profile
    digest = hashlib.sha256(data).hexdigest().lower()
    raise ValueError(
        f"Unsupported default.xbe sha256 {digest}; expected European (PAL) or USA (NTSC-U) retail build"
    )
