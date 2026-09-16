// Pure transition-graph and set-duration planning model. The UI can lay out
// and render this without owning file-format or replay timing semantics.
#pragma once

#include "Transition.h"

#include <QString>
#include <cstddef>
#include <vector>

namespace gvt {

struct TransitionGraphNode {
    QString key;
    QString title;
    double nativeBpm = 0.0;
    double durationSeconds = 0.0;
    double durationBeats = 0.0;
    double referenceDownbeatSeconds = 0.0;
    double startBeat = 0.0;
    double endBeat = 0.0;
};

struct TransitionGraphEdge {
    int from = -1;
    int to = -1;
    QString name;
    QString transitionId;
    QString filePath;
    double outgoingBeat = 0.0;
    double incomingBeatAtEnd = 0.0;
    double transitionSeconds = 0.0;
};

struct TransitionGraph {
    std::vector<TransitionGraphNode> nodes;
    std::vector<TransitionGraphEdge> edges;
    std::vector<std::vector<int>> outgoingEdges;
};

struct TransitionGraphRoute {
    std::vector<int> nodes;
    std::vector<int> edges;
    double durationSeconds = 0.0;
    bool completeSearch = true;
};

QString transitionGraphEndpointKey(const GvtTrackRef& endpoint);
double transitionGraphEffectiveEndBeat(const GvtFile& file) noexcept;
TransitionGraph buildTransitionGraph(const std::vector<GvtFile>& files);

// Finds the longest-duration feasible simple route. Songs never repeat, which
// makes routes through cyclic transition libraries finite. Edges whose saved
// outgoing beat has already passed on arrival are not feasible.
TransitionGraphRoute longestTransitionGraphRoute(
    const TransitionGraph& graph, int startNode,
    std::size_t maximumStates = 250000);

} // namespace gvt
