from __future__ import annotations

import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from check_repo_hygiene import (  # noqa: E402
    check_cpp_line_budgets,
    check_forbidden_tracked_assets,
    check_registries_and_json,
    run_all_checks,
)


def test_repo_hygiene_passes() -> None:
    assert run_all_checks(REPO_ROOT) == []


def test_cpp_line_budget_enforced(tmp_path: pathlib.Path) -> None:
    src = tmp_path / "port" / "src"
    src.mkdir(parents=True)
    big = src / "too_big.cpp"
    big.write_text("\n".join("int x = 0;" for _ in range(405)), encoding="utf-8")
    errors = check_cpp_line_budgets(tmp_path)
    assert len(errors) == 1
    assert "too_big.cpp" in errors[0]


def test_forbidden_assets_detected(tmp_path: pathlib.Path) -> None:
    errors = check_forbidden_tracked_assets(tmp_path, ["artifacts/default.xbe", "save/save.dat"])
    assert len(errors) == 2


def test_registry_validation_catches_bad_json_and_va(tmp_path: pathlib.Path) -> None:
    cfg = tmp_path / "config"
    cfg.mkdir()
    (cfg / "approximations.json").write_text("{bad json", encoding="utf-8")
    (cfg / "symbols.tsv").write_text(
        "eu_va\tkind\tsubsystem\tsymbol\tnotes\nBADVA\tcode\ttest\tsym_a\tnote\n",
        encoding="utf-8",
    )
    errors = check_registries_and_json(tmp_path)
    assert any("invalid JSON" in e for e in errors)
    assert any("invalid hex eu_va" in e for e in errors)
