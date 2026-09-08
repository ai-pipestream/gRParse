#pragma once

// The one place the mirrored schema version lives. Every producer stamp and
// every export identity reads this constant; when the upstream schema minor
// moves, this is the only line that changes, and the oracle harnesses
// (scripts/validate_canonical_json.py against the reference checkout) are
// the check that the mirror actually caught up.
//
// This is docling-core's CURRENT_VERSION (docling_core/types/doc/common/
// constants.py), not a serializer or library version: docling stamps the
// schema version its document conforms to, and so do we. The value was
// verified equal to docling-core's CURRENT_VERSION ("1.10.0") at the time
// of this comment; bump it only in lockstep with the upstream mirror, or
// docling-core's check_version_is_compatible rejects the documents we
// stamp.
namespace grparse {

inline constexpr const char* kUpstreamSchemaVersion = "1.10.0";

// The wire schema identity producers stamp on Document.schema_name. Export
// interop does not run through it: the canonical renderer emits the
// upstream dialect's own name from its own constant.
inline constexpr const char* kWireSchemaName = "docling_document_v2";

}  // namespace grparse
