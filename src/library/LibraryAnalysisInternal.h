// Deterministic worker injection for queue regression tests; not a public API.
#pragma once
#include "../analysis/AnalysisInternal.h"

namespace gvt {
class TrackLibrary;
namespace detail {
using LibraryAnalyzer = std::function<TrackDataPtr(
    const QString&, QString*, const AnalysisProgress&)>;
// Set before scanning. Production libraries use the cache-aware analyzer.
void setLibraryAnalyzerForTesting(TrackLibrary&, LibraryAnalyzer);
}
}
