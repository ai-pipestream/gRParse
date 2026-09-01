"""Determinism of the fixture generators under eval/scorecard/fixtures.

One generator is reproducible with nothing but the standard library: the
two-column layout is rendered to a flat ODT that is committed beside the
script, so this regenerates it into a temporary directory and compares byte
for byte. The rest end in a container format whose bytes are not stable even
for identical content (a zip stores per-entry timestamps, and the PDF leg runs
the source through an external office suite that stamps its own dates), and
they need third-party packages this suite does not carry, so they cannot be
re-run here. What is checkable about them offline is that they take no input
from the clock or a random source: every one pins a fixed timestamp. Their
output bytes are held instead by the pinned digests in fixtures/manifest.json
(see test_corpus_integrity.py).
"""

from __future__ import annotations

import ast
import importlib.util
import sys
import tempfile
from pathlib import Path

EVAL_DIR = Path(__file__).resolve().parents[2]
if str(EVAL_DIR) not in sys.path:
    sys.path.insert(0, str(EVAL_DIR))

FIXTURES = EVAL_DIR / "scorecard" / "fixtures"
FLAT_ODT = FIXTURES / "two-column.fodt"

# Names that would make a generator's output depend on when or where it ran.
NONDETERMINISTIC = ("datetime.now", "datetime.today", "time.time", "random.", "uuid.", "os.urandom")
# Generators that write a fixture and therefore have to be reproducible.
GENERATORS = ("two_column_pdf.py", "pptx_notes.py", "docx_figures.py",
              "xlsx_sixty_sheets.py", "docx_form.py")


def _load(name: str):
    """Import a generator by path; they are scripts, not a package."""
    path = FIXTURES / name
    spec = importlib.util.spec_from_file_location(f"scorecard_fixture_{path.stem}", path)
    assert spec is not None and spec.loader is not None, f"cannot load {path}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_every_generator_is_present() -> None:
    missing = [name for name in GENERATORS if not (FIXTURES / name).is_file()]
    assert not missing, f"generators named by build_all.sh but absent: {missing}"


def test_build_all_runs_every_generator() -> None:
    script = (FIXTURES / "build_all.sh").read_text()
    unrun = [name for name in GENERATORS if name not in script]
    assert not unrun, f"generators build_all.sh does not run: {unrun}"
    assert "rotated_scan.sh" in script, "build_all.sh no longer runs the rotated scan leg"


def test_two_column_layout_regenerates_byte_for_byte() -> None:
    module = _load("two_column_pdf.py")
    paragraphs = module.load_paragraphs()
    first = module.render(paragraphs)
    second = module.render(module.load_paragraphs())
    assert first == second, "the flat ODT render is not stable within one process"
    with tempfile.TemporaryDirectory() as work:
        target = Path(work) / "two-column.fodt"
        target.write_text(first, encoding="utf-8")
        regenerated = target.read_bytes()
    committed = FLAT_ODT.read_bytes()
    assert regenerated == committed, (
        "eval/scorecard/fixtures/two-column.fodt no longer matches what two_column_pdf.py "
        "renders, so two-column.pdf and the rotated scan derived from it describe a layout "
        "the generator would not produce again; regenerate with "
        "eval/scorecard/fixtures/build_all.sh and update fixtures/manifest.json deliberately"
    )


def test_two_column_layout_is_a_pure_function_of_its_prose() -> None:
    module = _load("two_column_pdf.py")
    paragraphs = module.load_paragraphs()
    assert paragraphs, "the Gatsby excerpt loaded no paragraphs"
    assert module.render(list(paragraphs)) == module.render(list(paragraphs)), (
        "render() is not a pure function of its paragraph list"
    )


def test_generators_take_nothing_from_the_clock_or_a_random_source() -> None:
    problems: list[str] = []
    for name in GENERATORS:
        source = (FIXTURES / name).read_text()
        tree = ast.parse(source, filename=name)
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            rendered = ast.unparse(node.func)
            problems.extend(f"{name}: calls {rendered}"
                            for banned in NONDETERMINISTIC if banned in rendered)
    assert not problems, (
        "a fixture generator whose output depends on when it ran cannot be regenerated to "
        "the pinned digests:\n  " + "\n  ".join(problems)
    )


def test_document_generators_pin_a_fixed_timestamp() -> None:
    """The four container-format generators stamp a constant, never the clock."""
    for name in ("pptx_notes.py", "docx_figures.py", "xlsx_sixty_sheets.py", "docx_form.py"):
        source = (FIXTURES / name).read_text()
        assert "FIXED_TIME" in source, f"{name} does not pin a fixed document timestamp"
        assert "datetime.now" not in source, f"{name} reads the clock"
