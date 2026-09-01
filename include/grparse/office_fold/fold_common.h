// Shared vocabulary of the office fold: the two wire namespaces it lives
// between, and the repeated fields its collaborators hand each other.
#pragma once

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"

namespace grparse::office_fold {

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

using TextRuns = google::protobuf::RepeatedPtrField<officev1::TextRun>;
using LineBoxes = google::protobuf::RepeatedPtrField<officev1::LineBox>;
using InlineSpans = google::protobuf::RepeatedPtrField<docv1::InlineSpan>;
using ProvenanceItems =
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>;
using SourceTypes = google::protobuf::RepeatedPtrField<docv1::SourceType>;
using ChildRefs = google::protobuf::RepeatedPtrField<docv1::RefItem>;

}  // namespace grparse::office_fold
