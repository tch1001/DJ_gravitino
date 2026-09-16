// Transition-graph tests protect canonical node merging and timing-aware,
// cycle-safe set-route estimates.
#include "transitions/TransitionGraph.h"

#include <cmath>
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

gvt::GvtTrackRef song(const QString& title, const QString& fingerprint,
                      double durationSeconds = 120.0)
{
    gvt::GvtTrackRef ref;
    ref.title = title;
    ref.artist = QStringLiteral("Fixture artist");
    ref.bpm = 120.0;
    ref.durationSec = durationSeconds;
    ref.durationBeats = durationSeconds * 2.0;
    ref.fingerprints.push_back(
        {QStringLiteral("gravitino-structure-2"), fingerprint, {}});
    return ref;
}

gvt::GvtFile edge(const gvt::GvtTrackRef& from,
                  const gvt::GvtTrackRef& to, double outgoingBeat,
                  double incomingBeat, double endBeat,
                  double launchBeat = 0.0)
{
    gvt::GvtFile file;
    file.name = from.title + QStringLiteral(" into ") + to.title;
    file.from = from;
    file.to = to;
    file.anchorFromBeat = outgoingBeat;
    file.anchorToBeat = incomingBeat;
    file.masterBpm = 120.0;
    file.endBeat = endBeat;
    file.initialTo.captured = true;
    file.initialTo.tempoRatio = 1.0;
    file.events.push_back(
        {launchBeat, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0,
         gvt::Curve::Step});
    return file;
}
}

int main()
{
    using namespace gvt;
    const GvtTrackRef a = song(QStringLiteral("A"), QStringLiteral("same-a"));
    const GvtTrackRef b = song(QStringLiteral("B"), QStringLiteral("same-b"));
    const GvtTrackRef c = song(QStringLiteral("C"), QStringLiteral("same-c"));
    const GvtTrackRef d = song(QStringLiteral("D"), QStringLiteral("same-d"),
                               20.0);

    std::vector<GvtFile> files;
    // A: 32 s to its transition + 8 s transition. Incoming B launches at
    // transition beat 4 and therefore arrives at track beat 28.
    files.push_back(edge(a, b, 64.0, 16.0, 16.0, 4.0));
    // B waits from beat 28 to 100 (36 s), transitions for 4 s, then C has
    // 116 s left from beat 8. Total A route: 32+8+36+4+116 = 196 s.
    files.push_back(edge(b, c, 100.0, 0.0, 8.0));
    // This edge is impossible after arriving at B beat 28: its exit is gone.
    files.push_back(edge(b, d, 20.0, 0.0, 4.0));
    // A cycle exists but a planned route never repeats a song.
    files.push_back(edge(c, a, 200.0, 0.0, 4.0));

    // A duplicate endpoint profile with a different display spelling still
    // resolves to the same canonical structural-fingerprint node.
    GvtFile duplicate = edge(a, b, 32.0, 0.0, 4.0);
    duplicate.from.title = QStringLiteral("A (local spelling)");
    files.push_back(duplicate);

    const TransitionGraph graph = buildTransitionGraph(files);
    CHECK(graph.nodes.size() == 4);
    CHECK(graph.edges.size() == files.size());
    CHECK(graph.edges.front().name == QStringLiteral("A into B"));
    int aIndex = -1;
    for (int index = 0; index < static_cast<int>(graph.nodes.size()); ++index)
        if (graph.nodes[static_cast<std::size_t>(index)].title ==
            QStringLiteral("A"))
            aIndex = index;
    CHECK(aIndex >= 0);

    const TransitionGraphRoute route =
        longestTransitionGraphRoute(graph, aIndex);
    CHECK(route.completeSearch);
    CHECK(route.nodes.size() == 3);
    CHECK(route.edges.size() == 2);
    CHECK(graph.nodes[static_cast<std::size_t>(route.nodes[0])].title ==
          QStringLiteral("A"));
    CHECK(graph.nodes[static_cast<std::size_t>(route.nodes[1])].title ==
          QStringLiteral("B"));
    CHECK(graph.nodes[static_cast<std::size_t>(route.nodes[2])].title ==
          QStringLiteral("C"));
    CHECK(std::fabs(route.durationSeconds - 196.0) < 0.001);

    const TransitionGraphRoute limited =
        longestTransitionGraphRoute(graph, aIndex, 1);
    CHECK(!limited.completeSearch);
    CHECK(!limited.nodes.empty());

    GvtFile legacy = edge(a, b, 0.0, 0.0, 0.0);
    legacy.endBeat.reset();
    legacy.events = {
        {3.5, Role::Mixer, ControlId::Crossfader, 1.0, Curve::Step},
    };
    CHECK(std::fabs(transitionGraphEffectiveEndBeat(legacy) - 1.0) < 1e-9);
    legacy.events.push_back(
        {3.5, Role::ToDeck, ControlId::Play, 1.0, Curve::Step});
    CHECK(std::fabs(transitionGraphEffectiveEndBeat(legacy) - 4.5) < 1e-9);

    GvtFile legacySource = edge(a, b, 32.0, 0.0, 4.0);
    legacySource.id = QStringLiteral("stable-legacy-id");
    legacySource.sourceFormat = TransitionSourceFormat::LegacyGvt;
    GvtFile portableCopy = legacySource;
    portableCopy.id = QStringLiteral("portable-uuid");
    portableCopy.legacySourceId = legacySource.id;
    portableCopy.sourceFormat = TransitionSourceFormat::PortableYaml;
    portableCopy.name = QStringLiteral("Preferred portable name");
    portableCopy.endBeat = 8.0;
    const TransitionGraph migratedGraph =
        buildTransitionGraph({legacySource, portableCopy});
    CHECK(migratedGraph.nodes.size() == 2);
    CHECK(migratedGraph.edges.size() == 1);
    CHECK(migratedGraph.edges.front().name ==
          QStringLiteral("Preferred portable name"));
    CHECK(std::fabs(migratedGraph.edges.front().transitionSeconds - 4.0) <
          0.001);

    if (failures) return 1;
    std::puts("test_transition_graph: timing-aware routes passed");
    return 0;
}
