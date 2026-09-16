// Pure recommendation tiers for grouping the library around currently
// playing songs without coupling musical matching rules to Qt table widgets.
#pragma once

#include "../analysis/TrackData.h"

#include <vector>

namespace gvt {

enum class TrackRecommendationTier : int {
    BpmAndKey = 0,
    BpmOnly = 1,
    Normal = 2,
};

// Uses native track BPM with an inclusive +/- 10 BPM window and exact,
// case-insensitive Camelot-code equality. A candidate already represented by
// a playing reference is kept in Normal rather than recommending itself.
TrackRecommendationTier trackRecommendationTier(
    const TrackData& candidate,
    const std::vector<const TrackData*>& playingReferences);

} // namespace gvt
