#!/usr/bin/env python3
"""Stage the flat proto files under their import paths, as the build does.

    python3 scripts/stage-protos.py <source-root> <out-dir>

The repository keeps document.proto, parse*.proto, collectors/*.proto and
backends/*.proto flat; CMakeLists.txt copies each to its package import path
with configure_file(<src> "${STAGED_PROTO_DIR}/<dest>" COPYONLY). This reads
those same calls, so the staged tree buf checks (the proto-contract CI job,
#246) is the tree protoc compiles and cannot drift from it. A buf.yaml is
written at the root of the staged tree.
"""

import pathlib
import re
import shutil
import sys

PATTERN = re.compile(
    r'configure_file\(\s*(?P<src>[^\s)]+\.proto)\s+"\$\{STAGED_PROTO_DIR\}/(?P<dest>[^"]+\.proto)"\s+COPYONLY\s*\)'
)

# breaking: FILE, except that a field may be deleted once both its number
# and its name are reserved (the way a retired field leaves the contract,
# #253), and a message may be deleted (a map field's synthetic entry goes with
# it; a message still referenced fails as a field type change).
BUF_YAML = """version: v2
modules:
  - path: .
breaking:
  use:
    - FILE
    - FIELD_NO_DELETE_UNLESS_NUMBER_RESERVED
    - FIELD_NO_DELETE_UNLESS_NAME_RESERVED
  except:
    - FIELD_NO_DELETE
    - MESSAGE_NO_DELETE
lint:
  use:
    - STANDARD
"""


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 64
    root = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    pairs = [(m["src"], m["dest"]) for m in PATTERN.finditer(cmake)]
    if not pairs:
        print("stage-protos: no configure_file proto stagings found", file=sys.stderr)
        return 1
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for src, dest in pairs:
        target = out / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / src, target)
    (out / "buf.yaml").write_text(BUF_YAML, encoding="utf-8")
    print(f"stage-protos: {len(pairs)} files staged under {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
