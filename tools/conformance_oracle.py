#!/usr/bin/env python3
"""Unicorn x86 execution oracle for verifying Phantom Crash XBE semantics."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import pathlib
import struct
import sys
from typing import Iterable, Sequence

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from phantomcrash_research.xbe import Xbe, parse as parse_xbe  # noqa: E402

PAGE_SIZE = 0x1000
STACK_BASE = 0x00E00000
STACK_SIZE = 0x00120000
STACK_TOP = 0x00F0FF00
STOP_ADDR = 0x00F90000


def _align_down(value: int, align: int = PAGE_SIZE) -> int:
    return value & ~(align - 1)


def _align_up(value: int, align: int = PAGE_SIZE) -> int:
    return (value + align - 1) & ~(align - 1)


@dataclass(frozen=True)
class OracleCallResult:
    eax: int
    edx: int
    esp: int
    returned: bool


class X86Oracle:
    """Thin 32-bit x86 Unicorn runner for pure XBE / synthetic machine code."""

    def __init__(self) -> None:
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32

        self._uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self._mapped_pages: set[int] = set()
        self.map_range(STACK_BASE, STACK_SIZE)
        self.map_range(STOP_ADDR, PAGE_SIZE)

    def map_range(self, address: int, size: int) -> None:
        page = _align_down(address)
        target_end = _align_up(address + max(size, 1))
        while page < target_end:
            if page in self._mapped_pages:
                page += PAGE_SIZE
                continue
            run_start = page
            while page < target_end and page not in self._mapped_pages:
                self._mapped_pages.add(page)
                page += PAGE_SIZE
            self._uc.mem_map(run_start, page - run_start)

    def write_bytes(self, address: int, data: bytes) -> None:
        self.map_range(address, len(data))
        self._uc.mem_write(address, data)

    def read_bytes(self, address: int, size: int) -> bytes:
        return bytes(self._uc.mem_read(address, size))

    def load_xbe(self, path: pathlib.Path | str) -> Xbe:
        raw = pathlib.Path(path).read_bytes()
        xbe = parse_xbe(raw)
        header_len = min(len(raw), PAGE_SIZE)
        self.write_bytes(xbe.base_address, raw[:header_len])
        for sec in xbe.sections:
            vsize = max(sec.virtual_size, sec.raw_size)
            if vsize <= 0:
                continue
            self.map_range(sec.virtual_address, vsize)
            payload = raw[sec.raw_address : sec.raw_address + sec.raw_size]
            if payload:
                self._uc.mem_write(sec.virtual_address, payload)
        return xbe

    def call_cdecl(
        self,
        entry_va: int,
        args: Sequence[int] = (),
        max_steps: int = 100_000,
        require_return: bool = True,
    ) -> OracleCallResult:
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_EIP, UC_X86_REG_ESP

        sp = STACK_TOP - 4 * (1 + len(args))
        frame = struct.pack("<I", STOP_ADDR) + b"".join(
            struct.pack("<I", a & 0xFFFFFFFF) for a in args
        )
        self._uc.mem_write(sp, frame)
        self._uc.reg_write(UC_X86_REG_ESP, sp)
        self._uc.reg_write(UC_X86_REG_EIP, entry_va)
        self._uc.emu_start(entry_va, STOP_ADDR, count=max_steps)
        eip = self._uc.reg_read(UC_X86_REG_EIP)
        returned = eip == STOP_ADDR
        if require_return and not returned:
            raise RuntimeError(
                f"Call at 0x{entry_va:08x} did not return within {max_steps} instructions (eip=0x{eip:08x})"
            )
        return OracleCallResult(
            eax=self._uc.reg_read(UC_X86_REG_EAX),
            edx=self._uc.reg_read(UC_X86_REG_EDX),
            esp=self._uc.reg_read(UC_X86_REG_ESP),
            returned=returned,
        )


def verify_synthetic_suite() -> dict[str, object]:
    """Verify x86 cdecl math, clamping, and stack discipline in Unicorn."""
    oracle = X86Oracle()
    # sub(a, b, c) => (a + b) * c  [32-bit cdecl]:
    # mov eax, [esp+4]; add eax, [esp+8]; imul eax, [esp+12]; ret
    code_mul = bytes.fromhex("8B442404034424080FAF44240CC3")
    oracle.write_bytes(0x00401000, code_mul)
    r1 = oracle.call_cdecl(0x00401000, [12, 8, 5])
    assert r1.eax == 100 and r1.esp == STACK_TOP - 12

    # clamp_nonneg(x): cmp eax=[esp+4], 0; jge +2; xor eax,eax; ret
    code_clamp = bytes.fromhex("8B44240485C07D0231C0C3")
    oracle.write_bytes(0x00401100, code_clamp)
    assert oracle.call_cdecl(0x00401100, [(-42) & 0xFFFFFFFF]).eax == 0
    assert oracle.call_cdecl(0x00401100, [37]).eax == 37
    return {"synthetic_checks": 3, "status": "ok"}


def inspect_xbe_in_oracle(xbe_path: pathlib.Path) -> dict[str, object]:
    oracle = X86Oracle()
    xbe = oracle.load_xbe(xbe_path)
    magic = oracle.read_bytes(xbe.base_address, 4)
    text_sec = xbe.section(".text")
    entry_bytes = oracle.read_bytes(xbe.entry_point, 8)
    return {
        "title_id": f"0x{xbe.title_id:08x}",
        "title_code": xbe.title_code,
        "xdk_build": xbe.xdk_build,
        "region_flags": f"0x{xbe.region_flags:08x}",
        "entry_point": f"0x{xbe.entry_point:08x}",
        "text_va": f"0x{text_sec.virtual_address:08x}",
        "text_size": text_sec.virtual_size,
        "mapped_magic": magic.decode("ascii", errors="replace"),
        "entry_prefix_hex": entry_bytes.hex(),
        "section_count": len(xbe.sections),
    }


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Phantom Crash XBE Unicorn conformance oracle")
    parser.add_argument("--xbe", type=pathlib.Path, help="Optional retail XBE image to load")
    parser.add_argument("--synthetic", action="store_true", help="Run synthetic Unicorn oracle checks")
    args = parser.parse_args(list(argv) if argv is not None else None)

    report: dict[str, object] = {}
    if args.synthetic or args.xbe is None:
        report["synthetic"] = verify_synthetic_suite()
    if args.xbe is not None:
        report["xbe"] = inspect_xbe_in_oracle(args.xbe)
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
