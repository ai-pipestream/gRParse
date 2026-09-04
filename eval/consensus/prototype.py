"""Cross-engine consensus prototype: reading order, outline, sections,
tables.

The idea under test: with several typed parsers behind one contract, a
second (and third) reading of the same document plus light NLP can correct
what any single engine gets wrong. Candidates come from the three PDF
backends (poppler emission order, pdfium content order, qparse sanitized
order) plus pdftotext -layout as the simple text leg. The consensus picks
a reading order by bigram agreement (each candidate scored by how many of
its adjacent word pairs the other candidates also emit adjacently) with a
sentence-continuity tiebreak, assembles an outline from the embedded
outlines plus font-size evidence in the winning cell stream, derives
sections and chunks from that outline, and (for the form fixture) rebuilds
a table grid from qparse's ruled-line shapes filled with word cells.

Everything is judged by the scorecard's own truth metrics
(eval/scorecard/truth_metrics.py), so a win here is a win on the
production gate. Output items carry the annotation model: the winning
value under "protomolt", the per-parser values under their own names.

Run (backends listening)::

    uv run --with grpcio --with grpcio-tools python eval/consensus/prototype.py \
        --targets localhost:51241 localhost:51242 localhost:51243

Writes ``out/report.md`` and ``out/<doc>.json``.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
OUT = HERE / "out"
sys.path.insert(0, str(REPO / "eval" / "pdf_diff"))
sys.path.insert(0, str(REPO / "eval" / "scorecard"))

from runner import load_stubs, stage_stubs  # noqa: E402  (eval/pdf_diff)
import truth_metrics  # noqa: E402  (eval/scorecard)

TRUTH_DIR = REPO / "eval" / "scorecard" / "truth"
CORPUS_DIR = REPO / "tests" / "golden" / "corpus"

# The digital-text truth documents; the rotated scans have no cell stream.
DOCS = {
    "pdf-two-column": CORPUS_DIR / "two-column.pdf",
    "pdf-form": CORPUS_DIR / "form.pdf",
}


# ---------------------------------------------------------------------------
# Candidate legs
# ---------------------------------------------------------------------------

def fetch_backend(stub, svc, types, data: bytes) -> dict:
    """One backend's cells (emission order), fonts, outline, and shapes."""
    request = svc.ParseRequest(document=types.PdfDocument(data=data))
    doc: dict = {"pages": {}, "outline": [], "shapes": {}, "backend": ""}
    for msg in stub.Parse(request, timeout=600):
        kind = msg.WhichOneof("payload")
        if kind == "header":
            doc["backend"] = msg.header.capabilities.backend_name
            for info in msg.header.pages:
                doc["pages"][info.page_index] = {
                    "height": info.height_pts,
                    "cells": [],
                }
        elif kind == "page":
            page = doc["pages"].setdefault(
                msg.page.page_index, {"height": 0.0, "cells": []}
            )
            for cell in msg.page.text_cells:
                page["cells"].append(
                    {
                        "text": cell.text,
                        "x0": cell.bbox.x0,
                        "y0": cell.bbox.y0,
                        "x1": cell.bbox.x1,
                        "y1": cell.bbox.y1,
                        "size": cell.font_size,
                    }
                )
            for shape in msg.page.shapes:
                segs = doc["shapes"].setdefault(msg.page.page_index, [])
                points = []
                for seg in shape.segments:
                    which = seg.WhichOneof("op")
                    if which in ("move_to", "line_to"):
                        point = getattr(seg, which)
                        points.append((point.x, point.y))
                segs.append(points)
        elif kind == "outline":
            def walk(node, level):
                doc["outline"].append({"text": node.title, "level": level})
                for child in node.children:
                    walk(child, level + 1)
            for root in msg.outline.roots:
                walk(root, 0)
    return doc


def candidate_words(doc: dict) -> list[str]:
    words: list[str] = []
    for index in sorted(doc["pages"]):
        for cell in doc["pages"][index]["cells"]:
            words.extend(cell["text"].split())
    return words


def pdftotext_words(path: Path) -> list[str]:
    proc = subprocess.run(
        ["pdftotext", "-layout", str(path), "-"], capture_output=True, timeout=300
    )
    if proc.returncode != 0:
        return []
    return proc.stdout.decode("utf-8", "replace").split()


# ---------------------------------------------------------------------------
# Reading-order consensus
# ---------------------------------------------------------------------------

def bigrams(words: list[str]) -> Counter:
    normalized = [truth_metrics.normalize(w) for w in words]
    return Counter(zip(normalized, normalized[1:]))


def agreement(candidate: Counter, others: list[Counter]) -> float:
    """Share of the candidate's adjacent pairs that other legs also emit
    adjacently, averaged over the other legs."""
    total = sum(candidate.values())
    if total == 0 or not others:
        return 0.0
    scores = []
    for other in others:
        shared = sum(min(count, other[pair]) for pair, count in candidate.items())
        scores.append(shared / total)
    return sum(scores) / len(scores)


SENTENCE_END = ".!?"


def continuity(words: list[str]) -> float:
    """Light NLP tiebreak: adjacent pairs that read like running text
    (mid-sentence lowercase continuation, or a sentence end followed by a
    capital)."""
    if len(words) < 2:
        return 0.0
    smooth = 0
    for prev, cur in zip(words, words[1:]):
        if not prev or not cur:
            continue
        ends_sentence = prev[-1] in SENTENCE_END
        starts_upper = cur[0].isupper()
        if (not ends_sentence and not starts_upper) or (ends_sentence and starts_upper):
            smooth += 1
    return smooth / (len(words) - 1)


def pick_order(word_lists: dict[str, list[str]]) -> tuple[str, dict[str, dict]]:
    grams = {name: bigrams(words) for name, words in word_lists.items()}
    scores: dict[str, dict] = {}
    for name, words in word_lists.items():
        others = [grams[o] for o in grams if o != name]
        scores[name] = {
            "agreement": round(agreement(grams[name], others), 4),
            "continuity": round(continuity(words), 4),
        }
        scores[name]["combined"] = round(
            0.8 * scores[name]["agreement"] + 0.2 * scores[name]["continuity"], 4
        )
    winner = max(scores, key=lambda n: scores[n]["combined"])
    return winner, scores


def truth_order(anchors: list[str], words: list[str]):
    text = " ".join(words)
    entries = [{"text": text}]
    return truth_metrics.reading_order_scores(anchors, entries, text)


# ---------------------------------------------------------------------------
# Outline consensus
# ---------------------------------------------------------------------------

def line_groups(cells: list[dict]) -> list[list[dict]]:
    """Cells grouped into visual lines by vertical overlap, left to right."""
    lines: list[list[dict]] = []
    for cell in sorted(cells, key=lambda c: (-c["y1"], c["x0"])):
        placed = False
        for line in lines:
            top = max(c["y1"] for c in line)
            bottom = min(c["y0"] for c in line)
            if cell["y0"] < top and cell["y1"] > bottom:
                line.append(cell)
                placed = True
                break
        if not placed:
            lines.append([cell])
    for line in lines:
        line.sort(key=lambda c: c["x0"])
    return lines


def font_headings(doc: dict) -> list[dict]:
    """Heading candidates from the cell stream: lines whose font size rises
    clearly above the body size, leveled by size tier."""
    sizes = [
        c["size"]
        for page in doc["pages"].values()
        for c in page["cells"]
        if c.get("size")
    ]
    if not sizes:
        return []
    body = statistics.median(sizes)
    found: list[dict] = []
    for index in sorted(doc["pages"]):
        for line in line_groups(doc["pages"][index]["cells"]):
            line_size = max((c.get("size") or 0.0) for c in line)
            text = " ".join(c["text"] for c in line)
            if line_size >= body * 1.2 and 0 < len(text.split()) <= 16:
                found.append({"text": text, "size": line_size, "page": index})
    tiers = sorted({round(h["size"], 1) for h in found}, reverse=True)
    for h in found:
        h["level"] = tiers.index(round(h["size"], 1))
    # Successive lines in the same tier are one wrapped heading.
    merged: list[dict] = []
    for h in found:
        if merged and merged[-1]["level"] == h["level"] and merged[-1]["page"] == h["page"]:
            merged[-1]["text"] += " " + h["text"]
        else:
            merged.append(dict(h))
    return [{"text": h["text"], "level": h["level"]} for h in merged]


NUMBERING = __import__("re").compile(r"^(\d+(?:\.\d+)*)\.?\s")


def numbering_level(text: str) -> int | None:
    """Section numbering is the strongest depth signal: "1." is level 1,
    "1.1" level 2, and so on."""
    hit = NUMBERING.match(text.strip())
    if hit is None:
        return None
    return hit.group(1).count(".") + 1


def consensus_outline(backends: dict[str, dict], cells_source: str) -> list[dict]:
    """Merge embedded outlines and font-derived headings; each node carries
    the winning text under "protomolt" and every source's version. Depth
    resolves by signal strength: section numbering in the title, then an
    embedded outline's nesting, then the font-size tier."""
    candidates: dict[str, list[dict]] = {}
    for name, doc in backends.items():
        if doc["outline"]:
            candidates[name] = doc["outline"]
    candidates["cells:" + cells_source] = font_headings(backends[cells_source])

    nodes: list[dict] = []
    for source, headings in candidates.items():
        embedded = not source.startswith("cells:")
        for heading in headings:
            hit = next(
                (
                    n
                    for n in nodes
                    if truth_metrics.prefix_match(n["protomolt"], heading["text"])
                ),
                None,
            )
            if hit is None:
                hit = {"protomolt": heading["text"], "sources": {}, "levels": {}}
                nodes.append(hit)
            hit["sources"][source] = heading["text"]
            hit["levels"]["embedded" if embedded else "font"] = heading["level"]
            # The longer rendition usually carries the full wrapped title.
            if len(heading["text"]) > len(hit["protomolt"]):
                hit["protomolt"] = heading["text"]
    # Embedded outline depths are relative to the outline tree, which often
    # starts below the document title; font tiers are absolute within the
    # document. Anchor embedded depths by the smallest font tier seen on a
    # node that carries both signals.
    anchors = [
        n["levels"]["font"] - n["levels"]["embedded"]
        for n in nodes
        if "embedded" in n["levels"] and "font" in n["levels"]
    ]
    embedded_offset = min(anchors) if anchors else 0
    for node in nodes:
        levels = node.pop("levels")
        numbered = numbering_level(node["protomolt"])
        if numbered is not None:
            node["level"] = numbered
        elif "embedded" in levels:
            node["level"] = levels["embedded"] + embedded_offset
        else:
            node["level"] = levels.get("font", 0)
    return nodes


# ---------------------------------------------------------------------------
# Sections and chunks
# ---------------------------------------------------------------------------

def sections_and_chunks(words: list[str], outline: list[dict], chunk_chars: int = 1800):
    text = " ".join(words)
    normalized = truth_metrics.normalize(text)
    marks = []
    for node in outline:
        needle = truth_metrics.normalize(node["protomolt"])
        pos = normalized.find(needle)
        if pos >= 0:
            marks.append((pos, node["protomolt"], node["level"]))
    marks.sort()
    sections = []
    for i, (pos, title, level) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(normalized)
        body = normalized[pos:end]
        chunks = []
        cursor = 0
        while cursor < len(body):
            cut = body.rfind(" ", cursor, cursor + chunk_chars)
            if cut <= cursor or len(body) - cursor <= chunk_chars:
                cut = min(cursor + chunk_chars, len(body))
            chunks.append(body[cursor:cut].strip())
            cursor = cut + 1
        sections.append(
            {"title": title, "level": level, "chars": len(body), "chunks": len(chunks)}
        )
    return sections


# ---------------------------------------------------------------------------
# Bidirectional reconciliation
# ---------------------------------------------------------------------------

def word_cells(doc: dict) -> list[dict]:
    """The document's cells flattened in emission order, with page, running
    index, and character offset into that leg's own reading text."""
    out = []
    offset = 0
    for page in sorted(doc["pages"]):
        for cell in doc["pages"][page]["cells"]:
            entry = dict(cell)
            entry["page"] = page
            entry["index"] = len(out)
            entry["offset"] = offset
            offset += len(cell["text"]) + 1
            out.append(entry)
    return out


def reconcile(backends: dict[str, dict], base_name: str,
              scores: dict[str, dict]) -> dict:
    """Word-level alignment between the consensus base stream and every
    other word-granularity leg, both directions at once: each base word
    keeps its consensus index and offset next to the matched leg's index
    and offset, so either side can look up the other. Deviations are
    annotated: order breaks (the leg's adjacency differs from consensus),
    missing words, and same-place text disagreements, which are the
    correction sites. Each leg carries its vote score as the source
    weight; a future structure source (a markdown converter, a language
    model's outline) joins as one more weighted leg."""
    base_cells = word_cells(backends[base_name])
    parsers: dict = {}
    full: dict = {}
    for name, doc in backends.items():
        if name == base_name:
            continue
        cells = word_cells(doc)
        if not cells or len(cells) < len(base_cells) * 0.5:
            # Line-granularity sources are out of scope for this word-level
            # prototype; the production form (consensus_page_source.cpp)
            # splits their line texts into words and aligns those.
            continue
        by_text: dict = {}
        for cell in cells:
            key = (cell["page"], truth_metrics.normalize(cell["text"]))
            by_text.setdefault(key, []).append(cell)
        used: set[int] = set()
        alignment = []
        text_deviations = []
        missing = 0
        for cell in base_cells:
            key = (cell["page"], truth_metrics.normalize(cell["text"]))
            candidates = [c for c in by_text.get(key, []) if c["index"] not in used]
            match = None
            deviation = None
            if candidates:
                match = min(
                    candidates,
                    key=lambda c: (c["x0"] - cell["x0"]) ** 2 + (c["y0"] - cell["y0"]) ** 2,
                )
            else:
                nearby = [
                    c
                    for c in cells
                    if c["page"] == cell["page"] and c["index"] not in used
                    and abs(c["x0"] - cell["x0"]) < 3.0 and abs(c["y0"] - cell["y0"]) < 3.0
                ]
                if nearby:
                    match = min(
                        nearby,
                        key=lambda c: (c["x0"] - cell["x0"]) ** 2 + (c["y0"] - cell["y0"]) ** 2,
                    )
                    deviation = "text"
                    text_deviations.append((cell, nearby[0]))
                else:
                    missing += 1
                    deviation = "missing"
            if match is not None:
                used.add(match["index"])
            alignment.append((cell, match, deviation))
        # A break is a regression in the source's order; a forward jump is
        # an insertion on the source side, not a reordering.
        order_breaks = 0
        previous = None
        for _, match, _ in alignment:
            if match is None:
                continue
            if previous is not None and match["index"] <= previous:
                order_breaks += 1
            previous = match["index"]
        parsers[name] = {
            "weight": scores.get(name, {}).get("combined"),
            "matched": len(base_cells) - missing,
            "missing": missing,
            "order_breaks": order_breaks,
            "text_deviation_count": len(text_deviations),
            "text_deviations": [
                {
                    "consensus": {"index": b["index"], "offset": b["offset"], "text": b["text"]},
                    name: {"index": p["index"], "offset": p["offset"], "text": p["text"]},
                }
                for b, p in text_deviations[:20]
            ],
        }
        full[name] = [
            {
                "c_index": b["index"],
                "c_offset": b["offset"],
                "p_index": None if p is None else p["index"],
                "p_offset": None if p is None else p["offset"],
                "dev": dev,
            }
            for b, p, dev in alignment
        ]
    return {"base": base_name, "words": len(base_cells), "parsers": parsers, "full": full}


# ---------------------------------------------------------------------------
# Table from ruled lines (the form fixture)
# ---------------------------------------------------------------------------

def cluster(values: list[float], tolerance: float = 3.0) -> list[float]:
    grouped: list[list[float]] = []
    for v in sorted(values):
        if grouped and v - grouped[-1][-1] <= tolerance:
            grouped[-1].append(v)
        else:
            grouped.append([v])
    return [sum(g) / len(g) for g in grouped]


def table_from_shapes(shapes: list[list[tuple[float, float]]], cells: list[dict]):
    """Grid from horizontal and vertical ruled segments, filled with word
    cells by center point."""
    horizontals: list[tuple[float, float, float]] = []  # (y, x_min, x_max)
    verticals: list[tuple[float, float, float]] = []  # (x, y_min, y_max)
    for points in shapes:
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            if abs(y1 - y0) < 1.0 and abs(x1 - x0) > 15.0:
                horizontals.append(((y0 + y1) / 2, min(x0, x1), max(x0, x1)))
            elif abs(x1 - x0) < 1.0 and abs(y1 - y0) > 8.0:
                verticals.append(((x0 + x1) / 2, min(y0, y1), max(y0, y1)))
    if not horizontals or not verticals:
        return None
    # The grid is where the two directions cross: keep rules inside the
    # perpendicular set's extent, which drops separators outside the table.
    v_lo = min(v[1] for v in verticals) - 3.0
    v_hi = max(v[2] for v in verticals) + 3.0
    h_lo = min(h[1] for h in horizontals) - 3.0
    h_hi = max(h[2] for h in horizontals) + 3.0
    rows = sorted(cluster([h[0] for h in horizontals if v_lo <= h[0] <= v_hi]),
                  reverse=True)
    cols = cluster([v[0] for v in verticals if h_lo <= v[0] <= h_hi])
    if len(rows) < 2 or len(cols) < 2:
        return None
    grid: dict[tuple[int, int], list[dict]] = {}
    for cell in cells:
        cx = (cell["x0"] + cell["x1"]) / 2
        cy = (cell["y0"] + cell["y1"]) / 2
        row = next((i for i in range(len(rows) - 1) if rows[i] >= cy >= rows[i + 1]), None)
        col = next((i for i in range(len(cols) - 1) if cols[i] <= cx <= cols[i + 1]), None)
        if row is None or col is None:
            continue
        grid.setdefault((row, col), []).append(cell)
    # Every grid position is a cell; an empty one is real data (a blank
    # form value), so it must be emitted, not omitted. Words inside a cell
    # read by visual line then left to right; a plain y-sort splits words
    # whose glyphs (checkboxes, symbols) sit in fonts with other heights.
    out = []
    for row in range(len(rows) - 1):
        for col in range(len(cols) - 1):
            members = grid.get((row, col), [])
            text = " ".join(
                c["text"] for line in line_groups(members) for c in line
            )
            out.append([row, col, 1, 1, text])
    return {"cells": out, "rows": len(rows) - 1, "cols": len(cols) - 1}


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", nargs="+", required=True)
    args = parser.parse_args()

    import grpc

    gen_dir = stage_stubs()
    svc, svc_grpc, types = load_stubs(gen_dir)
    stubs = {}
    for target in args.targets:
        channel = grpc.insecure_channel(
            target,
            options=[("grpc.max_receive_message_length", 520 * 1024 * 1024)],
        )
        stub = svc_grpc.PdfBackendServiceStub(channel)
        probe = stub.Probe(
            svc.ProbeRequest(document=types.PdfDocument(data=b"%PDF-1.4\n")),
            timeout=30,
        )
        stubs[probe.capabilities.backend_name.removeprefix("grpc-")] = stub

    OUT.mkdir(exist_ok=True)
    lines = ["# Cross-engine consensus prototype", ""]
    for doc_id, path in DOCS.items():
        truth = json.loads((TRUTH_DIR / f"{doc_id}.json").read_text())
        data = path.read_bytes()
        backends = {
            name: fetch_backend(stub, svc, types, data)
            for name, stub in stubs.items()
        }
        word_lists = {name: candidate_words(doc) for name, doc in backends.items()}
        word_lists["pdftotext"] = pdftotext_words(path)
        word_lists = {n: w for n, w in word_lists.items() if w}

        winner, scores = pick_order(word_lists)
        anchors = truth.get("anchors", [])
        lines += [f"## {doc_id}", "", "| leg | agreement | continuity | truth found | truth order |", "|---|---|---|---|---|"]
        order_results = {}
        for name, words in word_lists.items():
            result = truth_order(anchors, words)
            order_results[name] = result
            mark = " **(consensus pick)**" if name == winner else ""
            lines.append(
                f"| {name}{mark} | {scores[name]['agreement']} | {scores[name]['continuity']} "
                f"| {result.found:.3f} | {result.order:.3f} |"
            )
        lines.append("")

        outline = consensus_outline(backends, cells_source="poppler" if "poppler" in backends else winner)
        live_headings = [{"text": n["protomolt"], "level": n["level"]} for n in outline]
        heading_result = truth_metrics.heading_scores(truth.get("headings", []), live_headings)
        if heading_result:
            lines.append(
                f"Outline: {len(outline)} consensus nodes; truth headings recall "
                f"{heading_result.recall:.3f}, precision {heading_result.precision:.3f}"
                + (f", levels {heading_result.level_exact:.3f}" if heading_result.level_exact is not None else "")
            )
            per_backend_counts = {n: len(d["outline"]) for n, d in backends.items()}
            lines.append(f"Embedded outline nodes per backend: {per_backend_counts}; "
                         "font-derived headings fill the rest.")
        sections = sections_and_chunks(word_lists[winner], outline)
        lines.append(f"Sections from the consensus outline: {len(sections)}; "
                     f"chunks: {sum(s['chunks'] for s in sections)}")
        lines.append("")

        # Reconciliation base: the winner when it has geometry, else the
        # highest-scoring backend leg.
        base = winner if winner in backends else max(
            (n for n in backends), key=lambda n: scores[n]["combined"]
        )
        reconciliation = reconcile(backends, base, scores)
        for name, summary in reconciliation["parsers"].items():
            lines.append(
                f"Reconciliation {base} <-> {name} (weight {summary['weight']}): "
                f"{summary['matched']}/{reconciliation['words']} matched, "
                f"{summary['missing']} missing, {summary['order_breaks']} order breaks, "
                f"{summary['text_deviation_count']} text deviations"
            )
        lines.append("")
        (OUT / f"{doc_id}.alignment.json").write_text(
            json.dumps({"base": reconciliation["base"], "legs": reconciliation["full"]})
        )

        table_report = None
        if truth.get("tables"):
            qparse_doc = backends.get("qparse")
            fill_doc = backends.get("pdfium") or qparse_doc
            if qparse_doc and qparse_doc["shapes"]:
                page0 = sorted(qparse_doc["shapes"])[0]
                live = table_from_shapes(
                    qparse_doc["shapes"][page0],
                    fill_doc["pages"].get(page0, {}).get("cells", []),
                )
                if live:
                    result = truth_metrics.table_cell_scores(truth["tables"], [live])
                    if result:
                        table_report = result
                        lines.append(
                            f"Table from ruled lines + word cells: {live['rows']}x{live['cols']} grid, "
                            f"cell F1 {result.f1:.3f} (precision {result.precision:.3f}, recall {result.recall:.3f}, "
                            f"text found {result.text_found:.3f})"
                        )
                        lines.append("")

        (OUT / f"{doc_id}.json").write_text(
            json.dumps(
                {
                    "doc": doc_id,
                    "order": {
                        "protomolt": winner,
                        "scores": scores,
                        "truth": {
                            name: {"found": r.found, "order": r.order}
                            for name, r in order_results.items()
                        },
                    },
                    "outline": outline,
                    "sections": sections,
                    "reconciliation": {
                        "base": reconciliation["base"],
                        "words": reconciliation["words"],
                        "parsers": reconciliation["parsers"],
                    },
                    "table": None
                    if table_report is None
                    else {
                        "f1": table_report.f1,
                        "precision": table_report.precision,
                        "recall": table_report.recall,
                        "text_found": table_report.text_found,
                    },
                },
                indent=1,
            )
        )
    (OUT / "report.md").write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
