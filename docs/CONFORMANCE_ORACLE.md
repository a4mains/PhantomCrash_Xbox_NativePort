# XBE Conformance Oracle

`tools/conformance_oracle.py` loads retail Xbox `default.xbe` sections into a
Unicorn 32-bit x86 VM and executes isolated cdecl routines for oracle testing.

## Usage

```bash
python3 tools/conformance_oracle.py --synthetic
python3 tools/conformance_oracle.py --xbe /path/to/default.xbe
PHANTOM_USA_XBE=/path/to/default.xbe pytest tests/test_conformance_oracle.py
```

When neither `PHANTOM_USA_XBE` nor `PHANTOM_EU_XBE` is set, retail XBE checks
skip cleanly while synthetic x86 oracle tests always run.
