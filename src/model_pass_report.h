#pragma once

// What a parse says about the model passes its deployment runs without.
//
// Layout, table structure and figure classification are each switched off
// by a setting (GRPARSE_LAYOUT, GRPARSE_TABLE_STRUCTURE and
// GRPARSE_FIGURE_CLASSES). A document parsed without one is still a
// successful parse, so the loss has to travel with it: a typed warning on
// the document, once, and a typed rejection when the request asked for the
// pass by name. Before #410 neither existed, and a deployment whose model
// volume was missing a file answered OK with plain text where its tables
// and pictures should have been.

#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/ocr_types.h"
#include "grparse/page_scheduler.h"
#include "grparse/warning.h"

namespace grparse {

// What one document's pages held for the two passes that work on layout's
// regions. A document with no table has lost nothing to a missing
// table-structure model, and likewise for pictures and the classifier, so
// the warnings below are raised only for what the pages showed.
struct PassDemand {
  bool tables = false;
  bool pictures = false;

  // Adds what `page` holds, as the scheduler delivered it.
  void note(const OcrPage& page);
};

// The warnings one document read on the CV path carries for the passes in
// `passes` that are off, at most one per pass however many pages it has:
//
//   layout off                      WARNING_CODE_LAYOUT_PASS_DISABLED, and
//                                   nothing else, since without regions
//                                   there is no table or picture for the
//                                   other two passes to have missed;
//   table structure off             WARNING_CODE_TABLE_STRUCTURE_PASS_DISABLED
//                                   when a page holds a table and the
//                                   request did not set do_table_structure
//                                   to false itself;
//   figure classification off       WARNING_CODE_FIGURE_CLASSIFICATION_PASS_DISABLED
//                                   when a page holds a picture and the
//                                   request did not set
//                                   do_picture_classification to false.
//
// The caller stamps them under the CV collector's name ("grparse-cv").
std::vector<Warning> disabled_pass_warnings(const PageScheduler::ModelPasses& passes,
                                            const PageScheduler::OcrTuning& tuning,
                                            const PassDemand& demand);

// The typed rejection of a request that sets do_table_structure or
// do_picture_classification to true while that pass is off, OK otherwise.
// Unset and false pass: unset takes what the deployment runs, and says so
// in a warning. Call it only for a request the CV path may read; the
// options mean nothing to a document a collector converts.
grpc::Status refuse_disabled_passes(const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
                                    const PageScheduler::ModelPasses& passes,
                                    const std::string& surface);

}  // namespace grparse
