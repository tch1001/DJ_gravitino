// Musical grouping rules for Recommended mode in the track library.
#include "TrackRecommendation.h"

#include <cmath>

namespace gvt {
namespace {

bool sameSong(const TrackData& left, const TrackData& right)
{
    if (!left.songId.isEmpty() && !right.songId.isEmpty())
        return left.songId == right.songId;
    if (!left.structureFingerprint.isEmpty() &&
        !right.structureFingerprint.isEmpty())
        return left.structureFingerprint == right.structureFingerprint;
    if (!left.fingerprint.isEmpty() && !right.fingerprint.isEmpty())
        return left.fingerprint == right.fingerprint;
    return !left.filePath.isEmpty() && left.filePath == right.filePath;
}

bool bpmMatches(const TrackData& candidate, const TrackData& reference)
{
    return candidate.bpm > 0.0 && reference.bpm > 0.0 &&
           std::fabs(candidate.bpm - reference.bpm) <= 10.0 + 1e-9;
}

bool keyMatches(const TrackData& candidate, const TrackData& reference)
{
    const QString candidateKey = candidate.camelotKey.trimmed();
    const QString referenceKey = reference.camelotKey.trimmed();
    return !candidateKey.isEmpty() && !referenceKey.isEmpty() &&
           candidateKey.compare(referenceKey, Qt::CaseInsensitive) == 0;
}

} // namespace

TrackRecommendationTier trackRecommendationTier(
    const TrackData& candidate,
    const std::vector<const TrackData*>& playingReferences)
{
    for (const TrackData* reference : playingReferences)
        if (reference && sameSong(candidate, *reference))
            return TrackRecommendationTier::Normal;

    bool matchesBpm = false;
    for (const TrackData* reference : playingReferences) {
        if (!reference || !bpmMatches(candidate, *reference)) continue;
        matchesBpm = true;
        if (keyMatches(candidate, *reference))
            return TrackRecommendationTier::BpmAndKey;
    }
    return matchesBpm ? TrackRecommendationTier::BpmOnly
                      : TrackRecommendationTier::Normal;
}

} // namespace gvt
