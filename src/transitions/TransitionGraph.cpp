// Builds canonical song nodes from transition endpoints and estimates the
// longest timing-feasible, non-repeating route through recorded transitions.
#include "TransitionGraph.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace gvt {
namespace {

QString normalized(const QString& text)
{
    return text.simplified().toCaseFolded();
}

QString endpointArtistText(const GvtTrackRef& endpoint)
{
    if (!endpoint.artists.isEmpty())
        return endpoint.artists.join(QStringLiteral(";"));
    return endpoint.artist;
}

std::vector<QString> endpointIdentityKeys(const GvtTrackRef& endpoint)
{
    std::vector<QString> keys;
    const auto append = [&keys](const QString& key) {
        if (!key.isEmpty() &&
            std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    };
    for (const TransitionFingerprint& fingerprint : endpoint.fingerprints) {
        if (fingerprint.algorithm.startsWith(
                QStringLiteral("gravitino-structure"),
                Qt::CaseInsensitive) && !fingerprint.value.isEmpty())
            append(QStringLiteral("structure:") +
                   normalized(fingerprint.value));
    }
    if (!endpoint.musicBrainzRecording.isEmpty())
        append(QStringLiteral("musicbrainz:") +
               normalized(endpoint.musicBrainzRecording));
    if (!endpoint.isrc.isEmpty())
        append(QStringLiteral("isrc:") + normalized(endpoint.isrc));
    if (!endpoint.fingerprint.isEmpty())
        append(QStringLiteral("fingerprint:") +
               normalized(endpoint.fingerprint));
    for (const TransitionFingerprint& fingerprint : endpoint.fingerprints)
        if (!fingerprint.value.isEmpty())
            append(QStringLiteral("fingerprint:") +
                   normalized(fingerprint.algorithm) + QLatin1Char(':') +
                   normalized(fingerprint.value));
    // Metadata is only a fallback identity. Never use a matching title to
    // merge endpoints that carry different structural/recording evidence
    // (for example a radio edit and an extended mix).
    if (keys.empty() &&
        (!endpoint.title.isEmpty() || !endpointArtistText(endpoint).isEmpty()))
        append(QStringLiteral("metadata:") +
               normalized(endpointArtistText(endpoint)) + QLatin1Char('|') +
               normalized(endpoint.title) + QLatin1Char('|') +
               normalized(endpoint.versionName));
    if (keys.empty()) keys.push_back(QStringLiteral("metadata:||"));
    return keys;
}

double endpointDurationBeats(const GvtTrackRef& endpoint)
{
    if (endpoint.durationBeats > 0.0) return endpoint.durationBeats;
    if (endpoint.durationSec > 0.0 && endpoint.bpm > 0.0)
        return endpoint.durationSec * endpoint.bpm / 60.0;
    return 0.0;
}

double endpointSecondsPerBeat(const TransitionGraphNode& node)
{
    if (node.nativeBpm > 0.0) return 60.0 / node.nativeBpm;
    if (node.durationSeconds > 0.0 && node.durationBeats > 0.0)
        return node.durationSeconds / node.durationBeats;
    return 0.0;
}

double secondsBetween(const TransitionGraphNode& node, double fromBeat,
                      double toBeat)
{
    return std::max(0.0, toBeat - fromBeat) * endpointSecondsPerBeat(node);
}

double remainingSeconds(const TransitionGraphNode& node, double atBeat)
{
    if (node.durationSeconds <= 0.0) return 0.0;
    if (node.endBeat > node.startBeat && endpointSecondsPerBeat(node) > 0.0)
        return secondsBetween(node, std::max(atBeat, node.startBeat),
                              node.endBeat);
    return node.durationSeconds;
}

bool incomingStartControl(ControlId control) noexcept
{
    return control == ControlId::Play ||
           (control >= ControlId::HotCue1 &&
            control <= ControlId::HotCue8) ||
           (control >= ControlId::TransitionCue1 &&
            control <= ControlId::TransitionCue8) ||
           (control >= ControlId::SavedLoop1 &&
            control <= ControlId::SavedLoop8);
}

double incomingLaunchBeat(const GvtFile& file)
{
    if (file.initialComplete && file.initialTo.captured &&
        file.initialTo.playing)
        return 0.0;

    // PLAY is where the recorder captures anchorTo, including a cue+PLAY
    // latch. Fall back to a cue/loop launch only for unusual older files that
    // have no explicit PLAY event.
    for (const GvtEvent& event : file.events)
        if (event.role == Role::ToDeck &&
            event.control == ControlId::Play && event.value >= 0.5)
            return event.beat;
    for (const GvtEvent& event : file.events)
        if (event.role == Role::ToDeck && event.value >= 0.5 &&
            incomingStartControl(event.control))
            return event.beat;
    return transitionGraphEffectiveEndBeat(file);
}

double incomingBeatRate(const GvtFile& file, double launchBeat)
{
    const double nativeBpm = file.to.bpm;
    const double masterBpm = file.masterBpm > 0.0
                                 ? file.masterBpm : nativeBpm;
    if (nativeBpm <= 0.0 || masterBpm <= 0.0) return 1.0;

    double ratio = file.initialTo.captured
                       ? file.initialTo.tempoRatio
                       : masterBpm / nativeBpm;
    for (const GvtEvent& event : file.events) {
        if (event.beat > launchBeat + 0.0005) break;
        if (event.role == Role::ToDeck &&
            event.control == ControlId::Tempo && event.value > 0.0)
            ratio = event.value;
    }
    return nativeBpm * ratio / masterBpm;
}

void mergeEndpoint(TransitionGraphNode& node, const GvtTrackRef& endpoint)
{
    if (node.title.isEmpty() && !endpoint.title.isEmpty())
        node.title = endpoint.title;
    if (node.nativeBpm <= 0.0 && endpoint.bpm > 0.0)
        node.nativeBpm = endpoint.bpm;
    if (node.durationSeconds <= 0.0 && endpoint.durationSec > 0.0)
        node.durationSeconds = endpoint.durationSec;
    const double beats = endpointDurationBeats(endpoint);
    if (node.durationBeats <= 0.0 && beats > 0.0)
        node.durationBeats = beats;
    if (node.referenceDownbeatSeconds == 0.0 &&
        endpoint.referenceDownbeatSec != 0.0)
        node.referenceDownbeatSeconds = endpoint.referenceDownbeatSec;

    const double secondsPerBeat = endpointSecondsPerBeat(node);
    node.startBeat = secondsPerBeat > 0.0
                         ? -node.referenceDownbeatSeconds / secondsPerBeat
                         : 0.0;
    node.endBeat = node.startBeat + node.durationBeats;
}

} // namespace

QString transitionGraphEndpointKey(const GvtTrackRef& endpoint)
{
    return endpointIdentityKeys(endpoint).front();
}

double transitionGraphEffectiveEndBeat(const GvtFile& file) noexcept
{
    if (file.endBeat.has_value()) return std::max(0.0, *file.endBeat);
    double latest = 0.0;
    for (const GvtEvent& event : file.events)
        if (transitionEventIsExecutable(event))
            latest = std::max(latest, event.beat);
    return latest + 1.0;
}

TransitionGraph buildTransitionGraph(const std::vector<GvtFile>& files)
{
    TransitionGraph graph;
    std::map<QString, int> nodeByKey;
    const auto nodeFor = [&graph, &nodeByKey](const GvtTrackRef& endpoint) {
        const std::vector<QString> keys = endpointIdentityKeys(endpoint);
        const auto found = std::find_if(
            keys.begin(), keys.end(), [&nodeByKey](const QString& key) {
                return nodeByKey.contains(key);
            });
        if (found != keys.end()) {
            const int index = nodeByKey.at(*found);
            mergeEndpoint(graph.nodes[static_cast<std::size_t>(index)],
                          endpoint);
            for (const QString& key : keys) nodeByKey.emplace(key, index);
            return index;
        }
        const int index = static_cast<int>(graph.nodes.size());
        TransitionGraphNode node;
        node.key = keys.front();
        node.title = endpoint.title.isEmpty()
                         ? QStringLiteral("Unknown song") : endpoint.title;
        mergeEndpoint(node, endpoint);
        graph.nodes.push_back(std::move(node));
        for (const QString& key : keys) nodeByKey.emplace(key, index);
        return index;
    };

    // A converted portable copy and its permanent legacy source are one
    // musical transition, even though the Library table deliberately exposes
    // both files for migration testing. Prefer the richer portable document.
    std::vector<const GvtFile*> logicalFiles;
    std::map<QString, std::size_t> logicalById;
    for (const GvtFile& file : files) {
        const QString logicalId = file.legacySourceId.isEmpty()
                                      ? file.id : file.legacySourceId;
        if (logicalId.isEmpty()) {
            logicalFiles.push_back(&file);
            continue;
        }
        const auto found = logicalById.find(logicalId);
        if (found == logicalById.end()) {
            logicalById.emplace(logicalId, logicalFiles.size());
            logicalFiles.push_back(&file);
        } else if (file.sourceFormat == TransitionSourceFormat::PortableYaml &&
                   logicalFiles[found->second]->sourceFormat !=
                       TransitionSourceFormat::PortableYaml) {
            logicalFiles[found->second] = &file;
        }
    }

    graph.edges.reserve(logicalFiles.size());
    for (const GvtFile* filePointer : logicalFiles) {
        const GvtFile& file = *filePointer;
        const int from = nodeFor(file.from);
        const int to = nodeFor(file.to);
        const double endBeat = transitionGraphEffectiveEndBeat(file);
        const double masterBpm = file.masterBpm > 0.0
                                     ? file.masterBpm
                                     : (file.from.bpm > 0.0
                                            ? file.from.bpm : file.to.bpm);
        const double launchBeat = incomingLaunchBeat(file);
        TransitionGraphEdge edge;
        edge.from = from;
        edge.to = to;
        edge.name = file.name.trimmed();
        edge.transitionId = file.id;
        edge.filePath = file.filePath;
        edge.outgoingBeat = file.anchorFromBeat;
        edge.incomingBeatAtEnd = file.anchorToBeat +
            std::max(0.0, endBeat - launchBeat) *
                incomingBeatRate(file, launchBeat);
        edge.transitionSeconds = masterBpm > 0.0
                                     ? endBeat * 60.0 / masterBpm : 0.0;
        graph.edges.push_back(std::move(edge));
    }

    graph.outgoingEdges.resize(graph.nodes.size());
    for (int index = 0; index < static_cast<int>(graph.edges.size()); ++index) {
        const TransitionGraphEdge& edge =
            graph.edges[static_cast<std::size_t>(index)];
        if (edge.from >= 0 &&
            edge.from < static_cast<int>(graph.outgoingEdges.size()))
            graph.outgoingEdges[static_cast<std::size_t>(edge.from)]
                .push_back(index);
    }
    return graph;
}

TransitionGraphRoute longestTransitionGraphRoute(
    const TransitionGraph& graph, int startNode, std::size_t maximumStates)
{
    TransitionGraphRoute empty;
    if (startNode < 0 || startNode >= static_cast<int>(graph.nodes.size()))
        return empty;

    std::vector<bool> visited(graph.nodes.size(), false);
    std::size_t states = 0;
    bool complete = true;
    std::function<TransitionGraphRoute(int, double)> search =
        [&](int nodeIndex, double arrivalBeat) -> TransitionGraphRoute {
        TransitionGraphRoute best;
        best.nodes = {nodeIndex};
        best.durationSeconds = remainingSeconds(
            graph.nodes[static_cast<std::size_t>(nodeIndex)], arrivalBeat);
        if (++states > maximumStates) {
            complete = false;
            return best;
        }

        visited[static_cast<std::size_t>(nodeIndex)] = true;
        for (int edgeIndex :
             graph.outgoingEdges[static_cast<std::size_t>(nodeIndex)]) {
            const TransitionGraphEdge& edge =
                graph.edges[static_cast<std::size_t>(edgeIndex)];
            if (edge.to < 0 || edge.to >= static_cast<int>(graph.nodes.size()) ||
                visited[static_cast<std::size_t>(edge.to)] ||
                edge.outgoingBeat + 0.0005 < arrivalBeat)
                continue;

            TransitionGraphRoute child = search(edge.to,
                                                 edge.incomingBeatAtEnd);
            const double duration = secondsBetween(
                                        graph.nodes[static_cast<std::size_t>(
                                            nodeIndex)],
                                        arrivalBeat, edge.outgoingBeat) +
                                    edge.transitionSeconds +
                                    child.durationSeconds;
            if (duration > best.durationSeconds + 0.0005 ||
                (std::fabs(duration - best.durationSeconds) <= 0.0005 &&
                 child.nodes.size() + 1 > best.nodes.size())) {
                best.durationSeconds = duration;
                best.edges = {edgeIndex};
                best.edges.insert(best.edges.end(), child.edges.begin(),
                                  child.edges.end());
                best.nodes = {nodeIndex};
                best.nodes.insert(best.nodes.end(), child.nodes.begin(),
                                  child.nodes.end());
            }
        }
        visited[static_cast<std::size_t>(nodeIndex)] = false;
        return best;
    };

    const TransitionGraphNode& start =
        graph.nodes[static_cast<std::size_t>(startNode)];
    TransitionGraphRoute result = search(startNode, start.startBeat);
    result.completeSearch = complete;
    return result;
}

} // namespace gvt
