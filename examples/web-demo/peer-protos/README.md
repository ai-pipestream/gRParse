# Vendored peer proto trees

Each directory here is a byte-identical copy of one sibling repository's
proto include tree, laid out as `<repo>/<include-root>/...` (the same
shape the shell's resolver probes for a sibling checkout, so the image
copies them to `/` and nothing in `server.js` changes). The list of trees
comes from `../peers.js`; `../sync-peer-protos.sh` writes them and
`../sync-peer-protos.sh --check` names anything that drifted.

Never edit a file here: the sibling owns its contract. After a peer's
contract changes, run the sync script, commit the result, and rebuild the
shell image. The `document.proto` copies inside these trees are the
siblings' own copies of the fleet schema; the fleet rule in `AGENTS.md`
(section 3) says they must match gRParse's `document.proto`, and the sync
script prints a note for every one that does not.
