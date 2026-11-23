"""Validate ``data/xbe_profiles.json`` against a retail USA default.xbe."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILES = ROOT / "data" / "xbe_profiles.json"

REQUIRED_TABLE_VAS = (
    0x0D0F07,
    0x0D19EA,
    0x11D060,
    0x11E660,
    0x17AC38,
    0x17BEC4,
    0x19F870,
    0x1B5B88,
    0x1C3E98,
    0x1D0E00,
    0x1DB4A0,
    0x1EFD38,
    0x2014C8,
    0x21C788,
    0x2212AC,
    0x254480,
    0x256738,
    0x259524,
    0x276680,
    0x27F114,
    0x285E40,
    0x410400,
)


def parse_va(value: str | int) -> int:
    return value if isinstance(value, int) else int(value, 16)


def resolve_va(profile: dict, canonical_va: int, *, kind: str = "table") -> int:
    overrides = profile[f"{kind}_overrides"]
    key = f"0x{canonical_va:06x}"
    if key in overrides:
        return parse_va(overrides[key])
    for lo_raw, hi_raw, delta in profile[f"{kind}_ranges"]:
        lo, hi = parse_va(lo_raw), parse_va(hi_raw)
        if lo <= canonical_va < hi:
            return canonical_va + int(delta)
    return canonical_va


def load_profiles(path: Path = DEFAULT_PROFILES) -> dict:
    doc = json.loads(path.read_text(encoding="utf-8"))
    profiles = doc.get("profiles", {})
    if "europe" not in profiles or "usa" not in profiles:
        raise ValueError("data/xbe_profiles.json must define both 'europe' and 'usa' profiles")
    for name, profile in profiles.items():
        if not re.fullmatch(r"[0-9a-f]{64}", profile.get("sha256", "")):
            raise ValueError(f"profile {name!r} has invalid sha256")
        if profile.get("id") != name or not profile.get("artifacts_dir"):
            raise ValueError(f"profile {name!r} has missing id/artifacts_dir")
    return profiles


def collect_data_text_vas(data_dir: Path) -> list[int]:
    vas: set[int] = set()
    for path in sorted(data_dir.rglob("*.json")):
        if path.name == "xbe_profiles.json":
            continue
        for match in re.finditer(r"\{xbe:(0x[0-9a-fA-F]+)\}", path.read_text(encoding="utf-8")):
            vas.add(int(match.group(1), 16))
    return sorted(vas)


def verify_xbe(profiles: dict, xbe_path: Path) -> dict[str, int]:
    raw = xbe_path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    matched = [p for p in profiles.values() if p["sha256"] == digest]
    if not matched:
        raise ValueError(f"Unrecognized XBE SHA-256: {digest}")
    profile = matched[0]
    if len(raw) < 0x124 or raw[:4] != b"XBEH":
        raise ValueError("Invalid XBE header")
    base, nsec, shoff_va = (
        struct.unpack_from("<I", raw, 0x104)[0],
        struct.unpack_from("<I", raw, 0x11C)[0],
        struct.unpack_from("<I", raw, 0x120)[0],
    )
    shoff = shoff_va - base
    if shoff < 0 or shoff + nsec * 56 > len(raw):
        raise ValueError("Invalid XBE section header table bounds")
    secs = []
    for i in range(nsec):
        _, va, _, raddr, rsz, _ = struct.unpack_from("<6I", raw, shoff + i * 56)
        secs.append((va, raddr, rsz))

    def va_to_off(va: int) -> int:
        for sva, raddr, rsz in secs:
            if sva <= va < sva + rsz:
                return raddr + (va - sva)
        raise ValueError(f"VA 0x{va:06x} not mapped in raw XBE sections")

    for va in REQUIRED_TABLE_VAS:
        va_to_off(resolve_va(profile, va, kind="table"))

    text_vas = collect_data_text_vas(ROOT / "data")
    for eu_va in text_vas:
        off = va_to_off(resolve_va(profile, eu_va, kind="text"))
        end = raw.find(b"\0", off, off + 4096)
        if end <= off:
            raise ValueError(f"Empty/unterminated string for EU 0x{eu_va:06x}")
        s = raw[off:end].decode("cp932")
        if not s.strip():
            raise ValueError(f"Blank string for EU 0x{eu_va:06x}")
    return {"tables_checked": len(REQUIRED_TABLE_VAS), "text_checked": len(text_vas)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--profiles", type=Path, default=DEFAULT_PROFILES)
    parser.add_argument("--check", action="store_true", help="validate schema only")
    parser.add_argument("--xbe", type=Path, help="verify against an extracted default.xbe")
    args = parser.parse_args()

    profiles = load_profiles(args.profiles)
    if args.xbe:
        stats = verify_xbe(profiles, args.xbe)
        print(f"Verified {args.xbe}: {stats['tables_checked']} tables, {stats['text_checked']} strings")
    else:
        print(f"Schema OK: {', '.join(sorted(profiles.keys()))} ({len(collect_data_text_vas(ROOT / 'data'))} canonical text refs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
