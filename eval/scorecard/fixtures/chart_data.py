"""The one data set behind every chart fixture and its rendered twin.

``xlsx_charts.py`` and ``pptx_charts.py`` build their charts from these
rows, ``render_charts`` paints the same rows with matplotlib for the
derender comparison in ``eval/chart_derender``, and the truth files under
``eval/scorecard/truth`` quote them. One module means the truth cannot
drift from the fixture.

Values are integers and decimals chosen so a display string is
unambiguous: ``120`` stays ``120`` (never ``120.0``) and ``135.5`` cannot be
confused with a rounded integer.
"""

from __future__ import annotations

import json
import zipfile
from dataclasses import dataclass
from pathlib import Path

FIXED_ZIP_TIME = (1980, 1, 1, 0, 0, 0)


@dataclass(frozen=True)
class ChartSpec:
    """One chart: its data grid plus the titles the source declares."""

    name: str
    kind: str  # bar, line, pie
    title: str | None
    x_title: str | None
    y_title: str | None
    header: tuple[str, ...]
    rows: tuple[tuple[str, float | int, ...], ...]

    @property
    def categories(self) -> tuple[str, ...]:
        return tuple(str(row[0]) for row in self.rows)

    def series(self, index: int) -> tuple[float | int, ...]:
        return tuple(row[index + 1] for row in self.rows)

    @property
    def series_count(self) -> int:
        return len(self.header) - 1

    def grid(self) -> list[list[str]]:
        """The chart as the table gRParse binds: header row, then one row per category."""
        return [list(self.header)] + [[str(cell) for cell in row] for row in self.rows]

    def as_truth(self) -> dict:
        return {
            "name": self.name, "kind": self.kind, "title": self.title or "",
            "x_title": self.x_title or "", "y_title": self.y_title or "",
            "header": list(self.header), "rows": [list(row) for row in self.rows],
        }


BAR = ChartSpec(
    name="Revenue", kind="bar", title="Revenue by region", x_title="Region", y_title="kUSD",
    header=("Region", "Q1", "Q2"),
    rows=(("North", 120, 135.5), ("South", 80, 97), ("East", 143, 70.25), ("West", 88, 101)),
)
LINE = ChartSpec(
    name="Temperature", kind="line", title="Monthly mean temperature", x_title="Month", y_title="Degrees C",
    header=("Month", "Celsius"),
    rows=(("Jan", 3.5), ("Feb", 4.1), ("Mar", 7.8), ("Apr", 11.2), ("May", 15.6), ("Jun", 18.9)),
)
# The pie deliberately carries no title: the composite must not invent a
# caption, and the derender comparison must cope with a missing title.
PIE = ChartSpec(
    name="Share", kind="pie", title=None, x_title=None, y_title=None,
    header=("Segment", "Share"),
    rows=(("Alpha", 45), ("Beta", 30), ("Gamma", 15), ("Delta", 10)),
)
CHARTS = (BAR, LINE, PIE)


def display(value: float | int | str) -> str:
    """The text a spreadsheet shows for a value: integers without a decimal point."""
    if isinstance(value, bool):
        return str(value)
    if isinstance(value, (int, float)) and float(value).is_integer():
        return str(int(value))
    return str(value)


def normalize_zip(path: Path) -> None:
    """Rewrite an OOXML package with fixed entry timestamps.

    openpyxl and python-pptx stamp every zip entry with the wall clock, which
    is the one nondeterministic thing left once the document properties are
    pinned. Entry order and bytes are kept; only the date_time changes.
    Nested packages (a chart's embedded workbook inside a pptx) are
    normalized the same way, and their core properties get a fixed
    timestamp because xlsxwriter offers no hook for it.
    """
    with zipfile.ZipFile(path) as archive:
        entries = [(info, archive.read(info.filename)) for info in archive.infolist()]
    rewritten: list[tuple[zipfile.ZipInfo, bytes]] = []
    for info, data in entries:
        data = _pin_core_properties(info.filename, data)
        if info.filename.endswith(".xlsx"):
            data = _normalize_embedded_xlsx(data)
        clean = zipfile.ZipInfo(info.filename, date_time=FIXED_ZIP_TIME)
        clean.compress_type = zipfile.ZIP_DEFLATED
        clean.external_attr = info.external_attr
        rewritten.append((clean, data))
    with zipfile.ZipFile(path, "w") as archive:
        for info, data in rewritten:
            archive.writestr(info, data)


def _pin_core_properties(name: str, payload: bytes) -> bytes:
    """docProps/core.xml with created and modified pinned to 2000-01-01.

    openpyxl rewrites ``modified`` on every save whatever the workbook
    properties say, and xlsxwriter stamps ``created`` from the clock.
    """
    if name != "docProps/core.xml":
        return payload
    import re

    for tag in (b"created", b"modified"):
        payload = re.sub(rb">[^<]*</dcterms:" + tag + b">", b">2000-01-01T00:00:00Z</dcterms:" + tag + b">", payload)
    return payload


def _normalize_embedded_xlsx(data: bytes) -> bytes:
    import io

    source = zipfile.ZipFile(io.BytesIO(data))
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as archive:
        for info in source.infolist():
            payload = _pin_core_properties(info.filename, source.read(info.filename))
            clean = zipfile.ZipInfo(info.filename, date_time=FIXED_ZIP_TIME)
            clean.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(clean, payload)
    return out.getvalue()


def render_charts(out_dir: Path) -> list[Path]:
    """Paint every chart with matplotlib (Agg, bundled DejaVu fonts, no
    version metadata) and write ``truth.json`` beside the PNGs."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for spec in CHARTS:
        fig, ax = plt.subplots(figsize=(6.4, 4.0), dpi=100)
        categories = spec.categories
        if spec.kind == "bar":
            width = 0.8 / spec.series_count
            for index in range(spec.series_count):
                offsets = [i + (index - (spec.series_count - 1) / 2) * width for i in range(len(categories))]
                ax.bar(offsets, spec.series(index), width=width, label=spec.header[index + 1])
            ax.set_xticks(range(len(categories)), categories)
            ax.legend()
        elif spec.kind == "line":
            for index in range(spec.series_count):
                ax.plot(categories, spec.series(index), marker="o", label=spec.header[index + 1])
            ax.legend()
        else:
            ax.pie(spec.series(0), labels=categories, autopct="%1.0f%%", startangle=90, counterclock=False)
            ax.axis("equal")
        if spec.title:
            ax.set_title(spec.title)
        if spec.x_title:
            ax.set_xlabel(spec.x_title)
        if spec.y_title:
            ax.set_ylabel(spec.y_title)
        target = out_dir / f"{spec.kind}.png"
        fig.savefig(target, format="png", metadata={"Software": None})
        plt.close(fig)
        written.append(target)
    truth = {spec.kind: spec.as_truth() | {"image": f"{spec.kind}.png"} for spec in CHARTS}
    truth_path = out_dir / "truth.json"
    truth_path.write_text(json.dumps(truth, indent=1, sort_keys=True) + "\n")
    written.append(truth_path)
    return written
