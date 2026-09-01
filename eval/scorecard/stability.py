"""Compare repeated summaries of the same document and name the first difference."""

from __future__ import annotations

from typing import Any

# Leaves a repeat run may legitimately word differently without the document
# being unstable. The only one today is the title a VLM derendered onto a
# raster chart (``pictures[i].derender.title``): a generative model may
# rephrase a title between runs while reading the same bars. The derendered
# cell counts and cells stay in the fingerprint, so a run that reads
# different numbers is still unstable. Mirrors DESCRIPTIVE_LEAVES in
# ``agreement.py``.
DESCRIPTIVE_LEAF_SUFFIXES = ("].derender.title",)


def descriptive(path: str) -> bool:
    return path.endswith(DESCRIPTIVE_LEAF_SUFFIXES)


def first_difference(a: Any, b: Any, path: str = "") -> str | None:
    """Dotted path of the first leaf where two JSON-like values differ, or None;
    descriptive leaves (see DESCRIPTIVE_LEAF_SUFFIXES) never count."""
    if descriptive(path):
        return None
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            here = f"{path}.{key}" if path else str(key)
            if key not in a or key not in b:
                return here
            found = first_difference(a[key], b[key], here)
            if found is not None:
                return found
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f"{path}[len {len(a)} != {len(b)}]"
        for index, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f"{path}[{index}]")
            if found is not None:
                return found
        return None
    return None if a == b else (path or "<root>")


def stability(summaries: list[dict[str, Any]]) -> tuple[bool | None, str | None]:
    """(stable, first differing path); stable is None with fewer than two summaries."""
    if len(summaries) < 2:
        return None, None
    for other in summaries[1:]:
        diff = first_difference(summaries[0], other)
        if diff is not None:
            return False, diff
    return True, None
