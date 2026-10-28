#pragma once

#include <fuse/cinematics/timeline.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

/// Why an editor `.seq` import failed. The text must be the line-based dialect
/// already accepted by `load_timeline_from_asset` (`duration_ms`, `sprite`,
/// `camera`, `actor`, `motion`, and `#` comments).
enum class SeqImportErrorKind {
    EmptyFile,
    UnknownTrack,
    BadKeyframe,
    NotSeqDialect,
    InvalidSyntax,
};

struct SeqImportError {
    /// 1-based line in the source text. An empty file is reported on line 1.
    int line = 1;
    SeqImportErrorKind kind = SeqImportErrorKind::InvalidSyntax;
    std::string message;
};

/// Editor import of `.seq` text.
///
/// On success `ok` is true, `timeline` is populated, and `errors` is empty.
/// On failure `ok` is false, `timeline` is empty, and `errors` holds the first
/// line-numbered diagnostic. Malformed input does not throw.
struct SeqImportResult {
    bool ok = false;
    Timeline timeline;
    std::vector<SeqImportError> errors;
};

[[nodiscard]] SeqImportResult import_seq(const std::string& text);

} // namespace fuse::cinematics
