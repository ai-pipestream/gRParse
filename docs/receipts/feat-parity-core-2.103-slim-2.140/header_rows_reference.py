"""Prints docling-core's export_to_markdown for the header-row cases in
tests/document_render_test.cpp (verify_markdown_header_rows_follow_docling_core_2_102)."""
import docling_core
from importlib.metadata import version
from docling_core.types.doc import DoclingDocument, TableCell, TableData

CASES = {
    "row labels flagged in the leading column": [
        [("", True), ("2024", True), ("2025", True)],
        [("Revenue", True), ("10", False), ("20", False)],
        [("Cost", True), ("5", False), ("6", False)],
    ],
    "an unflagged row label beside a stacked header": [
        [("Region", True), ("Q1", True)], [("North", False), ("7", True)], [("South", False), ("9", False)],
    ],
    "a fully flagged second header row": [
        [("Region", True), ("Q1", True)], [("", True), ("units", True)], [("South", False), ("9", False)],
    ],
    "a partially flagged first row": [
        [("Name", True), ("Score", False)], [("Ann", False), ("3", False)],
    ],
    "body text beside a flagged cell in row 1": [
        [("Item", True), ("Kind", True), ("Qty", True)],
        [("bolt", False), ("sub", True), ("42", False)],
        [("nut", False), ("hex", False), ("7", False)],
    ],
}

print("docling-core", version("docling-core"))
for name, rows in CASES.items():
    cells = [
        TableCell(text=t, column_header=h, start_row_offset_idx=r, end_row_offset_idx=r + 1,
                  start_col_offset_idx=c, end_col_offset_idx=c + 1)
        for r, row in enumerate(rows) for c, (t, h) in enumerate(row)
    ]
    doc = DoclingDocument(name="header-rows")
    doc.add_table(data=TableData(num_rows=len(rows), num_cols=len(rows[0]), table_cells=cells))
    print(f"== {name}")
    print(doc.export_to_markdown())
