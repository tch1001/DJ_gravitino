// Recommended-library tiers prioritize combined BPM/key matches, then BPM,
// while excluding a playing song from recommending itself.

#include "library/TrackRecommendation.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

gvt::TrackData track(const char* id, double bpm, const char* key)
{
    gvt::TrackData result;
    result.songId = QString::fromLatin1(id);
    result.bpm = bpm;
    result.camelotKey = QString::fromLatin1(key);
    return result;
}
} // namespace

int main()
{
    using gvt::TrackRecommendationTier;
    const gvt::TrackData playingA = track("playing-a", 120.0, "8A");
    const gvt::TrackData playingB = track("playing-b", 98.0, "4B");
    const std::vector<const gvt::TrackData*> references {
        &playingA, &playingB};

    CHECK(gvt::trackRecommendationTier(track("both", 130.0, "8a"),
                                       references) ==
          TrackRecommendationTier::BpmAndKey);
    CHECK(gvt::trackRecommendationTier(track("bpm", 111.0, "2A"),
                                       references) ==
          TrackRecommendationTier::BpmOnly);
    CHECK(gvt::trackRecommendationTier(track("other-deck", 91.0, "4B"),
                                       references) ==
          TrackRecommendationTier::BpmAndKey);
    CHECK(gvt::trackRecommendationTier(track("outside", 130.01, "8A"),
                                       {&playingA}) ==
          TrackRecommendationTier::Normal);
    CHECK(gvt::trackRecommendationTier(track("no-key", 120.0, ""),
                                       references) ==
          TrackRecommendationTier::BpmOnly);
    CHECK(gvt::trackRecommendationTier(playingA, references) ==
          TrackRecommendationTier::Normal);
    CHECK(gvt::trackRecommendationTier(track("none", 120.0, "8A"), {}) ==
          TrackRecommendationTier::Normal);

    if (failures) return 1;
    std::printf("test_track_recommendation: three recommendation tiers passed\n");
    return 0;
}
