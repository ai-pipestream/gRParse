import tempfile
from pathlib import Path

from s3.seed_from_workspace import collect, extension_of, summarize, wanted


def _touch(path: Path, size: int = 3) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x" * size)


def test_wanted_filters_directories_names_and_extensions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel in ("tests/a.pdf", "tests/README.md", "tests/notes.md", "node_modules/x.pdf", "build/y.docx",
                    "tests/.hidden.pdf", "tests/package.json", "tests/sub/z.warc.gz", "tests/statement.layout.json",
                    "tests/.venv/lib/site.txt", "tests/script.py", "Testing/log.txt"):
            _touch(root / rel)
        keep = sorted(str(p.relative_to(root)) for p in root.rglob("*") if p.is_file() and wanted(p, root))
        assert keep == ["tests/a.pdf", "tests/notes.md", "tests/statement.layout.json", "tests/sub/z.warc.gz"]


def test_collect_keys_are_repo_relative_and_counted() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        workspace = Path(tmp)
        _touch(workspace / "grpc-x/tests/data/a.pdf")
        _touch(workspace / "grpc-x/tests/data/b.PDF")
        _touch(workspace / "grpc-x/tests/empty.pdf", size=0)
        _touch(workspace / "grpc-y/demos/c.eml")
        entries = collect([workspace / "grpc-x/tests", workspace / "grpc-y/demos"], workspace)
        assert [key for key, _ in entries] == ["grpc-x/tests/data/a.pdf", "grpc-x/tests/data/b.PDF", "grpc-y/demos/c.eml"]
        by_ext, by_repo = summarize(entries)
        assert by_ext == {"pdf": 2, "eml": 1} and by_repo == {"grpc-x": 2, "grpc-y": 1}
        assert extension_of("x.warc.gz") == "warc.gz"


def test_collect_ignores_missing_and_outside_directories() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        workspace = Path(tmp)
        _touch(workspace / "grpc-x/tests/a.pdf")
        _touch(workspace / "grpc-x/other/b.pdf")
        entries = collect([workspace / "grpc-x/tests", workspace / "missing"], workspace)
        assert [key for key, _ in entries] == ["grpc-x/tests/a.pdf"]
