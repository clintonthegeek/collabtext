#pragma once

#include <collabtext/DocumentSession.h>

#include "crdt/SegmentWriter.h"

namespace CollabText {

/// Source-private seam for exercising session recovery under transport faults.
class DocumentSessionTestAccess {
public:
  [[nodiscard]] static Result<std::unique_ptr<DocumentSession>>
  open(DocumentSessionConfig config, const Crdt::WriterConfig &writer_config) {
    return DocumentSession::open_with_writer_config(std::move(config),
                                                     writer_config);
  }
};

} // namespace CollabText
