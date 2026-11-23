"""Bidirectional converter between binary save slots and port JSON save files.

This binary format mirrors the documented slot field layout in
``port/include/phantomcrash/settings.hpp`` with a trailing CRC32 checksum for
round-trip interchange (distinct from signed Xbox HDD ``phantomcrash.sav``).
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import struct
from typing import Any
import zlib

SLOT_MAGIC = b"PCSAV001"
SLOT_SIZE = 0x12000
MONEY_LIMIT = 99_999_999
CLEARED_MONEY_BIT = 0x8000_0000
FILETIME_EPOCH = datetime(1601, 1, 1, tzinfo=timezone.utc)

# Documented slot offsets from NativePort port/include/phantomcrash/settings.hpp:
OFF_MAGIC = 0x0000
OFF_FILETIME = 0x0008
OFF_MONEY_PACKED = 0x0010
OFF_RIDING = 0x0022
LEN_RIDING = 18
OFF_SEEN_BITS = 0x0100
LEN_SEEN_BITS = 64  # up to 512 story scene IDs
OFF_WON_CONTESTS = 0x0990
LEN_WON_CONTESTS = 64
OFF_DAY = 0x2808
OFF_PLAYER_NAME = 0x280C
LEN_PLAYER_NAME = 16  # 8 CP932 characters max + NUL
OFF_RECORDS = 0x5FE0
RECORD_NAMES = ("battles", "shots_down", "prize_money", "saves", "play_time")
OFF_TOTAL_DAYS = 0x5FFC
OFF_LAPS = 0x604C
OFF_SPIN_PAID = 0x113C8
OFF_CRC32 = SLOT_SIZE - 4


class XboxSaveError(ValueError):
    """Raised when a binary save slot or JSON quest payload is invalid."""


def _unwrap_quest(payload: dict[str, Any]) -> dict[str, Any]:
    if "quest" in payload and isinstance(payload["quest"], dict):
        return payload["quest"]
    return payload


def _parse_saved_timestamp(text: str) -> int:
    cleaned = text.strip()
    if not cleaned:
        return 0
    try:
        dt = datetime.strptime(cleaned, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    except ValueError as exc:
        raise XboxSaveError(f"Invalid saved timestamp {text!r} (expected YYYY-MM-DD HH:MM:SS)") from exc
    delta = dt - FILETIME_EPOCH
    return int(delta.total_seconds() * 10_000_000)


def _format_saved_timestamp(filetime: int) -> str:
    if filetime <= 0:
        return ""
    dt = FILETIME_EPOCH + timedelta(microseconds=filetime // 10)
    return dt.strftime("%Y-%m-%d %H:%M:%S")


def quest_to_xbox_slot(payload: dict[str, Any]) -> bytes:
    """Serialize a NativePort quest/settings JSON object into an Xbox save slot."""
    quest = _unwrap_quest(payload)
    buf = bytearray(SLOT_SIZE)
    buf[OFF_MAGIC : OFF_MAGIC + len(SLOT_MAGIC)] = SLOT_MAGIC

    filetime = _parse_saved_timestamp(str(quest.get("saved", "")))
    struct.pack_into("<Q", buf, OFF_FILETIME, filetime)

    money = max(0, min(MONEY_LIMIT, int(quest.get("money", 0))))
    cleared = bool(quest.get("cleared", False))
    packed_money = money | (CLEARED_MONEY_BIT if cleared else 0)
    struct.pack_into("<I", buf, OFF_MONEY_PACKED, packed_money)

    riding = str(quest.get("riding", "")).encode("cp932", errors="replace")[: LEN_RIDING - 1]
    buf[OFF_RIDING : OFF_RIDING + len(riding)] = riding

    for scene in quest.get("seen_events", []):
        idx = int(scene)
        if 0 <= idx < LEN_SEEN_BITS * 8:
            buf[OFF_SEEN_BITS + (idx >> 3)] |= 1 << (idx & 7)

    for contest in quest.get("won_contests", []):
        idx = int(contest)
        if 0 <= idx < LEN_WON_CONTESTS:
            buf[OFF_WON_CONTESTS + idx] = 1

    day = max(0, min(0xFFFF, int(quest.get("day", 0))))
    struct.pack_into("<I", buf, OFF_DAY, day)

    player_name = str(quest.get("player_name", "")).encode("cp932", errors="replace")[
        : LEN_PLAYER_NAME - 1
    ]
    buf[OFF_PLAYER_NAME : OFF_PLAYER_NAME + len(player_name)] = player_name

    records = quest.get("records", {})
    if isinstance(records, dict):
        for i, key in enumerate(RECORD_NAMES):
            val = max(0, min(0xFFFFFFFF, int(records.get(key, 0))))
            struct.pack_into("<I", buf, OFF_RECORDS + i * 4, val)

    struct.pack_into("<I", buf, OFF_TOTAL_DAYS, max(0, min(0xFFFFFFFF, int(quest.get("total_days", 0)))))
    struct.pack_into("<I", buf, OFF_LAPS, max(0, min(0xFFFFFFFF, int(quest.get("laps", 0)))))
    struct.pack_into("<I", buf, OFF_SPIN_PAID, max(0, min(0xFFFFFFFF, int(quest.get("spin_paid", 0)))))

    crc = zlib.crc32(buf[:OFF_CRC32]) & 0xFFFFFFFF
    struct.pack_into("<I", buf, OFF_CRC32, crc)
    return bytes(buf)


def _decode_cp932_cstr(raw: bytes, field: str) -> str:
    try:
        return raw.split(b"\0", 1)[0].decode("cp932")
    except UnicodeDecodeError as exc:
        raise XboxSaveError(f"Invalid CP932 bytes in {field}") from exc


def xbox_slot_to_quest(data: bytes, verify_checksum: bool = True) -> dict[str, Any]:
    """Decode an Xbox save slot payload into port ``{"quest": ...}`` JSON."""
    if len(data) < SLOT_SIZE:
        raise XboxSaveError(f"Save slot too small ({len(data)} bytes, expected {SLOT_SIZE})")
    if data[OFF_MAGIC : OFF_MAGIC + len(SLOT_MAGIC)] != SLOT_MAGIC:
        raise XboxSaveError("Invalid Xbox save slot magic")
    if verify_checksum:
        expected_crc = zlib.crc32(data[:OFF_CRC32]) & 0xFFFFFFFF
        actual_crc = struct.unpack_from("<I", data, OFF_CRC32)[0]
        if actual_crc != expected_crc:
            raise XboxSaveError(
                f"Save slot CRC32 mismatch (got 0x{actual_crc:08x}, expected 0x{expected_crc:08x})"
            )

    filetime = struct.unpack_from("<Q", data, OFF_FILETIME)[0]
    packed_money = struct.unpack_from("<I", data, OFF_MONEY_PACKED)[0]
    cleared = bool(packed_money & CLEARED_MONEY_BIT)
    money = packed_money & ~CLEARED_MONEY_BIT
    riding = _decode_cp932_cstr(data[OFF_RIDING : OFF_RIDING + LEN_RIDING], "riding")

    seen_events: list[int] = []
    for byte_idx in range(LEN_SEEN_BITS):
        mask = data[OFF_SEEN_BITS + byte_idx]
        if not mask:
            continue
        for bit in range(8):
            if mask & (1 << bit):
                seen_events.append(byte_idx * 8 + bit)

    won_contests = [i for i in range(LEN_WON_CONTESTS) if data[OFF_WON_CONTESTS + i] != 0]
    day = struct.unpack_from("<I", data, OFF_DAY)[0]
    player_name = _decode_cp932_cstr(data[OFF_PLAYER_NAME : OFF_PLAYER_NAME + LEN_PLAYER_NAME], "player_name")

    records: dict[str, int] = {}
    for i, key in enumerate(RECORD_NAMES):
        val = struct.unpack_from("<I", data, OFF_RECORDS + i * 4)[0]
        if val:
            records[key] = val

    quest: dict[str, Any] = {
        "player_name": player_name,
        "seen_events": seen_events,
        "day": day,
        "money": money,
        "cleared": cleared,
        "riding": riding,
        "saved": _format_saved_timestamp(filetime),
        "spin_paid": struct.unpack_from("<I", data, OFF_SPIN_PAID)[0],
        "total_days": struct.unpack_from("<I", data, OFF_TOTAL_DAYS)[0],
        "laps": struct.unpack_from("<I", data, OFF_LAPS)[0],
        "won_contests": won_contests,
        "records": records,
    }
    return {"quest": quest}
