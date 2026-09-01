"""Known findings and who owns them. A failure the battery keeps red on
purpose (a collector's bug, a stack caveat, a schema follow-on) is named
here so the run report says whose it is instead of leaving a reader to
re-triage it. Match is by check name and a substring of the grouped cause."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class KnownFinding:
    check: str
    cause_contains: str
    owner: str
    note: str


KNOWN_FINDINGS: tuple[KnownFinding, ...] = (
    KnownFinding(
        "warnings_typed", "collector warnings keyed as custom_fields strings", "gRParse (schema follow-on)",
        "collector warnings land on body.meta.custom_fields[collector_warnings:<name>] because the Document "
        "has no typed slot for them; the fix is a typed Document.warnings extension, a fleet-wide schema sweep, "
        "so the check stays red until it lands"),
    KnownFinding(
        "parse_succeeds", "fastwarc collector is not configured", "fastwarc-grpc (wire dialect)",
        "the stack leaves GRPARSE_FASTWARC_TARGET unset because the vendored fastwarc.v1 dialect is not "
        "wire-compatible with the published image (AGENTS.md, the fastwarc caveat); every WARC object fails "
        "until the two contracts are reconciled"),
    KnownFinding(
        "sniff_route", "fastwarc collector is not configured", "fastwarc-grpc (wire dialect)",
        "the extension-less WARC routes by its bytes to the same unconfigured collector"),
    KnownFinding(
        "parse_succeeds", "render exceeded the per-document timeout", "grpc-libreoffice",
        "a sparse sheet with cells at the far corners of the grid (calamine's corners.xlsx) makes the office "
        "core render the whole used range and run past its per-document timeout"),
    KnownFinding(
        "table_grids", "grid rows have", "grpc-markup",
        "an HTML table whose header cell spans two columns is emitted with a ragged grid (2 cells in a "
        "3-column row); the dialect's grid repeats a spanning cell at every position it covers"),
)


def owner_of(check: str, cause: str) -> KnownFinding | None:
    for entry in KNOWN_FINDINGS:
        if entry.check == check and entry.cause_contains in cause:
            return entry
    return None
