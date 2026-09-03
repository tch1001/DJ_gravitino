// Portable transition YAML — safe parsing, deterministic serialization, and
// the compatibility adapter that keeps the replay engine format-neutral.

#include "Transition.h"
#include "../analysis/TrackData.h"
#include "../performance/PerformancePads.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <yaml.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gvt {
namespace {

constexpr qsizetype kMaximumDocumentBytes = 2 * 1024 * 1024;
constexpr int kMaximumYamlDepth = 32;
constexpr int kMaximumYamlNodes = 100000;
constexpr int kMaximumTimelineEvents = 20000;
constexpr int kMaximumLabels = 1000;

QString yamlProblem(const yaml_parser_t& parser)
{
    const QString problem = parser.problem
                                ? QString::fromUtf8(parser.problem)
                                : QStringLiteral("invalid YAML");
    return QStringLiteral("YAML line %1, column %2: %3")
        .arg(parser.problem_mark.line + 1)
        .arg(parser.problem_mark.column + 1)
        .arg(problem);
}

bool inspectSafeYaml(const QByteArray& bytes, QString* error)
{
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        if (error) *error = QStringLiteral("could not initialize YAML parser");
        return false;
    }
    yaml_parser_set_input_string(
        &parser, reinterpret_cast<const unsigned char*>(bytes.constData()),
        static_cast<size_t>(bytes.size()));

    int count = 0;
    int documents = 0;
    bool ok = true;
    for (;;) {
        yaml_event_t event;
        if (!yaml_parser_parse(&parser, &event)) {
            if (error) *error = yamlProblem(parser);
            ok = false;
            break;
        }
        ++count;
        if (event.type == YAML_DOCUMENT_START_EVENT) ++documents;

        const yaml_char_t* anchor = nullptr;
        const yaml_char_t* tag = nullptr;
        switch (event.type) {
        case YAML_ALIAS_EVENT:
            if (error) *error = QStringLiteral("YAML aliases are not allowed");
            ok = false;
            break;
        case YAML_SCALAR_EVENT:
            anchor = event.data.scalar.anchor;
            tag = event.data.scalar.tag;
            break;
        case YAML_SEQUENCE_START_EVENT:
            anchor = event.data.sequence_start.anchor;
            tag = event.data.sequence_start.tag;
            break;
        case YAML_MAPPING_START_EVENT:
            anchor = event.data.mapping_start.anchor;
            tag = event.data.mapping_start.tag;
            break;
        default:
            break;
        }
        if (ok && anchor) {
            if (error) *error = QStringLiteral("YAML anchors are not allowed");
            ok = false;
        }
        if (ok && tag) {
            if (error) *error = QStringLiteral("explicit YAML tags are not allowed");
            ok = false;
        }
        const bool streamEnd = event.type == YAML_STREAM_END_EVENT;
        yaml_event_delete(&event);
        if (!ok || streamEnd) break;
        if (count > kMaximumYamlNodes) {
            if (error) *error = QStringLiteral("YAML document is too complex");
            ok = false;
            break;
        }
    }
    yaml_parser_delete(&parser);
    if (ok && documents != 1) {
        if (error) *error = QStringLiteral("exactly one YAML document is required");
        return false;
    }
    return ok;
}

QJsonValue scalarValue(const yaml_node_t& node)
{
    const QString text = QString::fromUtf8(
        reinterpret_cast<const char*>(node.data.scalar.value),
        static_cast<qsizetype>(node.data.scalar.length));
    if (node.data.scalar.style != YAML_PLAIN_SCALAR_STYLE)
        return text;

    const QString lower = text.toLower();
    if (lower == QLatin1String("null") || text == QLatin1String("~"))
        return QJsonValue(QJsonValue::Null);
    if (lower == QLatin1String("true")) return true;
    if (lower == QLatin1String("false")) return false;

    static const QRegularExpression number(
        QStringLiteral("^-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$"));
    if (number.match(text).hasMatch()) {
        bool parsed = false;
        const double value = text.toDouble(&parsed);
        if (parsed && std::isfinite(value)) return value;
    }
    return text;
}

bool nodeToJson(yaml_document_t& document, int index, int depth,
                int& nodesVisited, QJsonValue& out, QString* error)
{
    if (depth > kMaximumYamlDepth || ++nodesVisited > kMaximumYamlNodes) {
        if (error) *error = QStringLiteral("YAML document is too deeply nested or complex");
        return false;
    }
    yaml_node_t* node = yaml_document_get_node(&document, index);
    if (!node) {
        if (error) *error = QStringLiteral("YAML contains an invalid node reference");
        return false;
    }
    if (node->type == YAML_SCALAR_NODE) {
        out = scalarValue(*node);
        return true;
    }
    if (node->type == YAML_SEQUENCE_NODE) {
        QJsonArray array;
        for (yaml_node_item_t* item = node->data.sequence.items.start;
             item < node->data.sequence.items.top; ++item) {
            QJsonValue value;
            if (!nodeToJson(document, *item, depth + 1, nodesVisited,
                            value, error))
                return false;
            array.append(value);
        }
        out = array;
        return true;
    }
    if (node->type == YAML_MAPPING_NODE) {
        QJsonObject object;
        for (yaml_node_pair_t* pair = node->data.mapping.pairs.start;
             pair < node->data.mapping.pairs.top; ++pair) {
            yaml_node_t* keyNode = yaml_document_get_node(&document, pair->key);
            if (!keyNode || keyNode->type != YAML_SCALAR_NODE) {
                if (error) *error = QStringLiteral("YAML mapping keys must be strings");
                return false;
            }
            const QString key = QString::fromUtf8(
                reinterpret_cast<const char*>(keyNode->data.scalar.value),
                static_cast<qsizetype>(keyNode->data.scalar.length));
            if (key == QLatin1String("<<")) {
                if (error) *error = QStringLiteral("YAML merge keys are not allowed");
                return false;
            }
            if (object.contains(key)) {
                if (error) *error = QStringLiteral("duplicate YAML key: %1").arg(key);
                return false;
            }
            QJsonValue value;
            if (!nodeToJson(document, pair->value, depth + 1, nodesVisited,
                            value, error))
                return false;
            object.insert(key, value);
        }
        out = object;
        return true;
    }
    if (error) *error = QStringLiteral("unsupported YAML node type");
    return false;
}

bool parseYamlObject(const QString& text, QJsonObject& root, QString* error)
{
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() > kMaximumDocumentBytes) {
        if (error) *error = QStringLiteral("transition file exceeds the 2 MiB limit");
        return false;
    }
    if (bytes.contains('\0')) {
        if (error) *error = QStringLiteral("transition file contains a NUL byte");
        return false;
    }
    if (!inspectSafeYaml(bytes, error)) return false;

    yaml_parser_t parser;
    yaml_document_t document;
    if (!yaml_parser_initialize(&parser)) {
        if (error) *error = QStringLiteral("could not initialize YAML parser");
        return false;
    }
    yaml_parser_set_input_string(
        &parser, reinterpret_cast<const unsigned char*>(bytes.constData()),
        static_cast<size_t>(bytes.size()));
    if (!yaml_parser_load(&parser, &document)) {
        if (error) *error = yamlProblem(parser);
        yaml_parser_delete(&parser);
        return false;
    }
    QJsonValue value;
    int visited = 0;
    yaml_node_t* rootNode = yaml_document_get_root_node(&document);
    const int rootIndex = rootNode
                              ? static_cast<int>(rootNode - document.nodes.start) + 1
                              : 0;
    const bool converted = rootIndex > 0 &&
                           nodeToJson(document, rootIndex, 0, visited, value, error);
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    if (!converted) {
        if (rootIndex == 0 && error) *error = QStringLiteral("empty YAML document");
        return false;
    }
    if (!value.isObject()) {
        if (error) *error = QStringLiteral("transition YAML root must be a mapping");
        return false;
    }
    root = value.toObject();
    return true;
}

QJsonObject without(QJsonObject value, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) value.remove(QLatin1String(key));
    return value;
}

QString stringAt(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).toString();
}

double numberAt(const QJsonObject& object, const char* key, double fallback = 0.0)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() && std::isfinite(value.toDouble())
               ? value.toDouble() : fallback;
}

bool boolAt(const QJsonObject& object, const char* key, bool fallback = false)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : fallback;
}

QStringList stringArray(const QJsonValue& value)
{
    QStringList result;
    if (!value.isArray()) return result;
    for (const QJsonValue& item : value.toArray())
        if (item.isString()) result.append(item.toString());
    return result;
}

QString roleName(Role role)
{
    switch (role) {
    case Role::FromDeck: return QStringLiteral("outgoing");
    case Role::ToDeck: return QStringLiteral("incoming");
    case Role::Mixer: return QStringLiteral("mixer");
    }
    return QStringLiteral("mixer");
}

bool parseRole(const QString& text, Role& role)
{
    if (text == QLatin1String("outgoing")) { role = Role::FromDeck; return true; }
    if (text == QLatin1String("incoming")) { role = Role::ToDeck; return true; }
    if (text == QLatin1String("mixer")) { role = Role::Mixer; return true; }
    return false;
}

QString portableControlName(ControlId control, Role role)
{
    const QString prefix = role == Role::Mixer ? QStringLiteral("mixer.")
                                                : QStringLiteral("deck.");
    return prefix + QString::fromLatin1(controlName(control));
}

bool parsePortableControl(QString name, ControlId& control)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) name = name.mid(dot + 1);
    return controlFromName(name.toUtf8().constData(), control);
}

QString curveNamePortable(Curve curve)
{
    switch (curve) {
    case Curve::Step: return QStringLiteral("step");
    case Curve::Linear: return QStringLiteral("linear");
    case Curve::SCurve: return QStringLiteral("scurve");
    }
    return QStringLiteral("step");
}

bool parseCurvePortable(const QString& name, Curve& curve)
{
    if (name == QLatin1String("step")) { curve = Curve::Step; return true; }
    if (name == QLatin1String("linear")) { curve = Curve::Linear; return true; }
    if (name == QLatin1String("scurve")) { curve = Curve::SCurve; return true; }
    return false;
}

bool finiteField(const QJsonObject& object, const char* key, double minimum,
                 double maximum, bool required, const QString& path,
                 QString* error)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined() && !required) return true;
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        value.toDouble() < minimum || value.toDouble() > maximum) {
        if (error) {
            *error = QStringLiteral("%1.%2 must be a finite number from %3 to %4")
                         .arg(path, QLatin1String(key))
                         .arg(minimum, 0, 'g', 12)
                         .arg(maximum, 0, 'g', 12);
        }
        return false;
    }
    return true;
}

bool validateEndpointFields(const QJsonObject& endpoint, const QString& path,
                            QString* error)
{
    if (!endpoint.value(QStringLiteral("identity")).isObject() ||
        !endpoint.value(QStringLiteral("assumptions")).isObject()) {
        if (error) *error = path + QStringLiteral(" needs identity and assumptions mappings");
        return false;
    }
    const QJsonObject identity = endpoint.value(QStringLiteral("identity")).toObject();
    if (!identity.value(QStringLiteral("title")).isString() ||
        !identity.value(QStringLiteral("artists")).isArray() ||
        !identity.value(QStringLiteral("identifiers")).isObject()) {
        if (error) *error = path + QStringLiteral(".identity has invalid field types");
        return false;
    }
    for (const QJsonValue& artist :
         identity.value(QStringLiteral("artists")).toArray()) {
        if (!artist.isString()) {
            if (error) *error = path + QStringLiteral(".identity.artists must contain strings");
            return false;
        }
    }
    const QJsonObject assumptions = endpoint.value(
        QStringLiteral("assumptions")).toObject();
    static const QRegularExpression meter(
        QStringLiteral("^[1-9][0-9]*/[1-9][0-9]*$"));
    if (!assumptions.value(QStringLiteral("meter")).isString() ||
        !meter.match(assumptions.value(QStringLiteral("meter")).toString())
             .hasMatch() ||
        !assumptions.value(QStringLiteral("fingerprints")).isArray()) {
        if (error) *error = path + QStringLiteral(".assumptions has invalid meter/fingerprints");
        return false;
    }
    return finiteField(assumptions, "native_bpm", 20.0, 400.0, true,
                       path + QStringLiteral(".assumptions"), error) &&
           finiteField(assumptions, "duration_seconds", 0.01, 86400.0, true,
                       path + QStringLiteral(".assumptions"), error) &&
           finiteField(assumptions, "duration_beats", 0.01, 1000000.0, true,
                       path + QStringLiteral(".assumptions"), error) &&
           finiteField(assumptions, "reference_downbeat_seconds", -3600.0,
                       3600.0, true,
                       path + QStringLiteral(".assumptions"), error);
}

bool portableTimelineControlAllowed(ControlId control)
{
    switch (control) {
    case ControlId::Load:
    case ControlId::BrowseSelect:
    case ControlId::BrowseNavigate:
    case ControlId::HeadphoneCue:
    case ControlId::MasterCue:
    case ControlId::HeadphoneMix:
    case ControlId::Jog:
    case ControlId::PlatterScratch:
    case ControlId::PlatterTouch:
    case ControlId::PerformancePadMode:
    case ControlId::PerformancePad1:
    case ControlId::PerformancePad2:
    case ControlId::PerformancePad3:
    case ControlId::PerformancePad4:
    case ControlId::PerformancePad5:
    case ControlId::PerformancePad6:
    case ControlId::PerformancePad7:
    case ControlId::PerformancePad8:
    case ControlId::TempoRange:
    case ControlId::Trim:
    case ControlId::Count:
        return false;
    default:
        return true;
    }
}

bool validateTimelineValue(ControlId control, double value, QString* error,
                           int eventNumber)
{
    double minimum = 0.0, maximum = 1.0;
    switch (control) {
    case ControlId::Tempo: minimum = 0.01; maximum = 4.0; break;
    case ControlId::LoopAuto: minimum = 0.03125; maximum = 64.0; break;
    case ControlId::BeatJump: minimum = -1024.0; maximum = 1024.0; break;
    case ControlId::FxType: minimum = 0.0; maximum = 2.0; break;
    case ControlId::FxBeats: minimum = 0.25; maximum = 4.0; break;
    default: break;
    }
    const bool integerRequired = control == ControlId::FxType;
    if (!std::isfinite(value) || value < minimum || value > maximum ||
        (integerRequired && std::floor(value) != value)) {
        if (error) {
            *error = QStringLiteral("timeline event %1 has an out-of-range value for %2")
                         .arg(eventNumber)
                         .arg(QString::fromLatin1(controlName(control)));
        }
        return false;
    }
    return true;
}

QString padModeName(int mode)
{
    if (mode < 0 || mode >= static_cast<int>(PerformancePadMode::Count))
        return {};
    return QString::fromLatin1(
        performancePadModeKey(static_cast<PerformancePadMode>(mode)));
}

int parsePadMode(const QJsonValue& value)
{
    if (value.isDouble()) return value.toInt(-1);
    const QString name = value.toString();
    for (int i = 0; i < static_cast<int>(PerformancePadMode::Count); ++i)
        if (name == QLatin1String(performancePadModeKey(
                        static_cast<PerformancePadMode>(i))))
            return i;
    return -1;
}

void parseEndpoint(const QJsonObject& object, GvtTrackRef& ref,
                   QStringList* warnings)
{
    const QJsonObject identity = object.value(QStringLiteral("identity")).toObject();
    const QJsonObject identifiers = identity.value(QStringLiteral("identifiers")).toObject();
    const QJsonObject providers = identifiers.value(QStringLiteral("providers")).toObject();
    const QJsonObject assumptions = object.value(QStringLiteral("assumptions")).toObject();

    ref.title = stringAt(identity, "title").trimmed();
    ref.artists = stringArray(identity.value(QStringLiteral("artists")));
    ref.artist = ref.artists.join(QStringLiteral(", "));
    ref.versionName = stringAt(identity, "version").trimmed();
    ref.isrc = stringAt(identifiers, "isrc").trimmed();
    ref.musicBrainzRecording =
        stringAt(identifiers, "musicbrainz_recording").trimmed();
    for (auto it = providers.begin(); it != providers.end(); ++it)
        if (it.value().isString()) ref.providerIds[it.key()] = it.value().toString();
    ref.providersExtraYaml = providers;
    for (const auto& [key, value] : ref.providerIds)
        ref.providersExtraYaml.remove(key);

    ref.bpm = numberAt(assumptions, "native_bpm");
    ref.durationSec = numberAt(assumptions, "duration_seconds");
    ref.durationBeats = numberAt(assumptions, "duration_beats");
    ref.meter = stringAt(assumptions, "meter");
    if (ref.meter.isEmpty()) ref.meter = QStringLiteral("4/4");
    ref.referenceDownbeatSec = numberAt(assumptions, "reference_downbeat_seconds");
    const QJsonArray fingerprints = assumptions.value(
        QStringLiteral("fingerprints")).toArray();
    for (const QJsonValue& value : fingerprints) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        TransitionFingerprint fingerprint;
        fingerprint.algorithm = stringAt(item, "algorithm").trimmed();
        fingerprint.value = stringAt(item, "value").trimmed();
        fingerprint.extraYaml = without(item, {"algorithm", "value"});
        if (fingerprint.algorithm.isEmpty() || fingerprint.value.isEmpty()) {
            if (warnings) warnings->append(
                QStringLiteral("ignored a fingerprint without algorithm/value"));
            continue;
        }
        ref.fingerprints.push_back(fingerprint);
        if (fingerprint.algorithm == QLatin1String("gvfp1"))
            ref.fingerprint = fingerprint.value.startsWith(QLatin1String("gvfp1:"))
                                  ? fingerprint.value
                                  : QStringLiteral("gvfp1:") + fingerprint.value;
    }
    ref.notes = stringAt(object, "notes");
    ref.extraYaml = without(object, {"identity", "assumptions", "notes"});
    ref.identityExtraYaml = without(identity,
        {"title", "artists", "version", "identifiers"});
    ref.identifiersExtraYaml = without(identifiers,
        {"isrc", "musicbrainz_recording", "providers"});
    ref.assumptionsExtraYaml = without(assumptions,
        {"native_bpm", "duration_seconds", "duration_beats", "meter",
         "reference_downbeat_seconds", "fingerprints"});
}

void parseInitialDeck(const QJsonObject& object, GvtInitialState& state)
{
    state.captured = boolAt(object, "captured", !object.isEmpty());
    state.playing = boolAt(object, "playing");
    state.positionBeat = numberAt(object, "position_beat");
    state.cueBeat = numberAt(object, "cue_beat");
    state.tempoRatio = numberAt(object, "tempo_ratio", 1.0);
    state.fader = numberAt(object, "fader", 1.0);
    state.trimCaptured = object.contains(QStringLiteral("trim"));
    state.trim = numberAt(object, "trim", 0.5);
    state.eqLow = numberAt(object, "eq_low", 0.5);
    state.eqMid = numberAt(object, "eq_mid", 0.5);
    state.eqHigh = numberAt(object, "eq_high", 0.5);
    state.filter = numberAt(object, "filter", 0.5);
    state.quantizeCaptured = object.contains(QStringLiteral("quantize"));
    state.quantize = boolAt(object, "quantize", true);
    state.loopActive = boolAt(object, "loop_active");
    state.loopStartBeat = numberAt(object, "loop_start_beat");
    state.loopEndBeat = numberAt(object, "loop_end_beat");
    state.fxType = static_cast<int>(numberAt(object, "fx_type"));
    state.fxOn = boolAt(object, "fx_on");
    state.fxWet = numberAt(object, "fx_wet", 0.5);
    state.fxBeats = numberAt(object, "fx_beats", 0.5);
    state.stemVocals = numberAt(object, "stem_vocals", 1.0);
    state.stemMelody = numberAt(object, "stem_melody", 1.0);
    state.stemBass = numberAt(object, "stem_bass", 1.0);
    state.stemDrums = numberAt(object, "stem_drums", 1.0);
    state.extraYaml = without(object,
        {"captured", "playing", "position_beat", "cue_beat", "tempo_ratio",
         "fader", "trim", "eq_low", "eq_mid", "eq_high", "filter",
         "quantize", "loop_active", "loop_start_beat", "loop_end_beat",
         "fx_type", "fx_on", "fx_wet", "fx_beats", "stem_vocals",
         "stem_melody", "stem_bass", "stem_drums"});
}

void setIfText(QJsonObject& object, const QString& key, const QString& value)
{
    if (value.isEmpty()) object.remove(key);
    else object.insert(key, value);
}

QJsonArray stringsJson(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values) array.append(value);
    return array;
}

QJsonObject endpointJson(const GvtTrackRef& ref)
{
    QJsonObject endpoint = ref.extraYaml;
    QJsonObject identity = ref.identityExtraYaml;
    identity.insert(QStringLiteral("title"), ref.title);
    QStringList artists = ref.artists;
    if (artists.isEmpty() && !ref.artist.isEmpty()) artists.append(ref.artist);
    identity.insert(QStringLiteral("artists"), stringsJson(artists));
    setIfText(identity, QStringLiteral("version"), ref.versionName);

    QJsonObject identifiers = ref.identifiersExtraYaml;
    setIfText(identifiers, QStringLiteral("isrc"), ref.isrc);
    setIfText(identifiers, QStringLiteral("musicbrainz_recording"),
              ref.musicBrainzRecording);
    QJsonObject providers = ref.providersExtraYaml;
    for (const auto& [key, value] : ref.providerIds) providers.insert(key, value);
    identifiers.insert(QStringLiteral("providers"), providers);
    identity.insert(QStringLiteral("identifiers"), identifiers);
    endpoint.insert(QStringLiteral("identity"), identity);

    QJsonObject assumptions = ref.assumptionsExtraYaml;
    assumptions.insert(QStringLiteral("native_bpm"), ref.bpm);
    assumptions.insert(QStringLiteral("duration_seconds"), ref.durationSec);
    assumptions.insert(QStringLiteral("duration_beats"),
                       ref.durationBeats > 0.0
                           ? ref.durationBeats
                           : ref.durationSec * ref.bpm / 60.0);
    assumptions.insert(QStringLiteral("meter"),
                       ref.meter.isEmpty() ? QStringLiteral("4/4") : ref.meter);
    assumptions.insert(QStringLiteral("reference_downbeat_seconds"),
                       ref.referenceDownbeatSec);
    QJsonArray fingerprints;
    if (!ref.fingerprints.empty()) {
        for (const TransitionFingerprint& fingerprint : ref.fingerprints) {
            QJsonObject item = fingerprint.extraYaml;
            item.insert(QStringLiteral("algorithm"), fingerprint.algorithm);
            item.insert(QStringLiteral("value"), fingerprint.value);
            fingerprints.append(item);
        }
    } else if (!ref.fingerprint.isEmpty()) {
        QJsonObject item;
        item.insert(QStringLiteral("algorithm"), QStringLiteral("gvfp1"));
        item.insert(QStringLiteral("value"), ref.fingerprint);
        fingerprints.append(item);
    }
    assumptions.insert(QStringLiteral("fingerprints"), fingerprints);
    endpoint.insert(QStringLiteral("assumptions"), assumptions);
    setIfText(endpoint, QStringLiteral("notes"), ref.notes);
    return endpoint;
}

QJsonObject initialDeckJson(const GvtInitialState& state)
{
    QJsonObject object = state.extraYaml;
    object.insert(QStringLiteral("captured"), state.captured);
    object.insert(QStringLiteral("playing"), state.playing);
    object.insert(QStringLiteral("position_beat"), state.positionBeat);
    object.insert(QStringLiteral("cue_beat"), state.cueBeat);
    object.insert(QStringLiteral("tempo_ratio"), state.tempoRatio);
    object.insert(QStringLiteral("fader"), state.fader);
    if (state.trimCaptured) object.insert(QStringLiteral("trim"), state.trim);
    else object.remove(QStringLiteral("trim"));
    object.insert(QStringLiteral("eq_low"), state.eqLow);
    object.insert(QStringLiteral("eq_mid"), state.eqMid);
    object.insert(QStringLiteral("eq_high"), state.eqHigh);
    object.insert(QStringLiteral("filter"), state.filter);
    if (state.quantizeCaptured)
        object.insert(QStringLiteral("quantize"), state.quantize);
    else object.remove(QStringLiteral("quantize"));
    object.insert(QStringLiteral("loop_active"), state.loopActive);
    object.insert(QStringLiteral("loop_start_beat"), state.loopStartBeat);
    object.insert(QStringLiteral("loop_end_beat"), state.loopEndBeat);
    object.insert(QStringLiteral("fx_type"), state.fxType);
    object.insert(QStringLiteral("fx_on"), state.fxOn);
    object.insert(QStringLiteral("fx_wet"), state.fxWet);
    object.insert(QStringLiteral("fx_beats"), state.fxBeats);
    object.insert(QStringLiteral("stem_vocals"), state.stemVocals);
    object.insert(QStringLiteral("stem_melody"), state.stemMelody);
    object.insert(QStringLiteral("stem_bass"), state.stemBass);
    object.insert(QStringLiteral("stem_drums"), state.stemDrums);
    return object;
}

QString quoteYaml(const QString& text)
{
    QString result = QStringLiteral("\"");
    for (QChar ch : text) {
        switch (ch.unicode()) {
        case '\\': result += QStringLiteral("\\\\"); break;
        case '"': result += QStringLiteral("\\\""); break;
        case '\n': result += QStringLiteral("\\n"); break;
        case '\r': result += QStringLiteral("\\r"); break;
        case '\t': result += QStringLiteral("\\t"); break;
        default:
            if (ch.unicode() < 0x20)
                result += QStringLiteral("\\u%1").arg(static_cast<unsigned int>(ch.unicode()), 4, 16,
                                                       QLatin1Char('0'));
            else result += ch;
            break;
        }
    }
    return result + QLatin1Char('"');
}

QString scalarYaml(const QJsonValue& value)
{
    if (value.isNull() || value.isUndefined()) return QStringLiteral("null");
    if (value.isBool()) return value.toBool() ? QStringLiteral("true")
                                               : QStringLiteral("false");
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 15);
    return quoteYaml(value.toString());
}

QString yamlKey(const QString& key)
{
    static const QRegularExpression simple(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*$"));
    return simple.match(key).hasMatch() ? key : quoteYaml(key);
}

void emitYamlValue(QString& output, const QJsonValue& value, int indent);

void emitYamlObject(QString& output, const QJsonObject& object, int indent)
{
    if (object.isEmpty()) {
        output += QStringLiteral("{}\n");
        return;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        output += QString(indent, QLatin1Char(' ')) + yamlKey(it.key()) +
                  QLatin1Char(':');
        if (it.value().isObject() || it.value().isArray()) {
            const bool empty = (it.value().isObject() && it.value().toObject().isEmpty()) ||
                               (it.value().isArray() && it.value().toArray().isEmpty());
            if (empty) {
                output += it.value().isObject() ? QStringLiteral(" {}\n")
                                                : QStringLiteral(" []\n");
            } else {
                output += QLatin1Char('\n');
                emitYamlValue(output, it.value(), indent + 2);
            }
        } else {
            output += QLatin1Char(' ') + scalarYaml(it.value()) + QLatin1Char('\n');
        }
    }
}

void emitYamlArray(QString& output, const QJsonArray& array, int indent)
{
    if (array.isEmpty()) {
        output += QString(indent, QLatin1Char(' ')) + QStringLiteral("[]\n");
        return;
    }
    for (const QJsonValue& value : array) {
        output += QString(indent, QLatin1Char(' ')) + QLatin1Char('-');
        if (value.isObject() || value.isArray()) {
            const bool empty = (value.isObject() && value.toObject().isEmpty()) ||
                               (value.isArray() && value.toArray().isEmpty());
            if (empty) {
                output += value.isObject() ? QStringLiteral(" {}\n")
                                           : QStringLiteral(" []\n");
            } else {
                output += QLatin1Char('\n');
                emitYamlValue(output, value, indent + 2);
            }
        } else {
            output += QLatin1Char(' ') + scalarYaml(value) + QLatin1Char('\n');
        }
    }
}

void emitYamlValue(QString& output, const QJsonValue& value, int indent)
{
    if (value.isObject()) emitYamlObject(output, value.toObject(), indent);
    else if (value.isArray()) emitYamlArray(output, value.toArray(), indent);
    else output += QString(indent, QLatin1Char(' ')) + scalarYaml(value) +
                   QLatin1Char('\n');
}

void ensurePortableDefaults(GvtFile& file)
{
    if (file.id.isEmpty()) file.id = stableLegacyTransitionId(file);
    if (file.requirements.isEmpty()) {
        file.requirements = {QStringLiteral("timeline.v1"),
                             QStringLiteral("temporary-cues.v1")};
    }
    const auto normalizeRef = [](GvtTrackRef& ref) {
        if (ref.artists.isEmpty() && !ref.artist.isEmpty())
            ref.artists.append(ref.artist);
        if (ref.artist.isEmpty() && !ref.artists.isEmpty())
            ref.artist = ref.artists.join(QStringLiteral(", "));
        if (ref.durationBeats <= 0.0 && ref.bpm > 0.0)
            ref.durationBeats = ref.durationSec * ref.bpm / 60.0;
        if (ref.fingerprints.empty() && !ref.fingerprint.isEmpty()) {
            TransitionFingerprint fingerprint;
            const int colon = ref.fingerprint.indexOf(QLatin1Char(':'));
            fingerprint.algorithm = colon > 0 ? ref.fingerprint.left(colon)
                                                : QStringLiteral("gvfp1");
            fingerprint.value = ref.fingerprint;
            ref.fingerprints.push_back(fingerprint);
        }
    };
    normalizeRef(file.from);
    normalizeRef(file.to);
}

void addLegacyTransitionCues(GvtFile& file)
{
    const auto addRole = [&file](Role role, const std::array<double, 8>& beats,
                                 const QString& prefix) {
        for (int pad = 0; pad < 8; ++pad) {
            if (!hotCueBeatIsMapped(beats[static_cast<std::size_t>(pad)])) continue;
            TransitionHotCue cue;
            cue.id = QStringLiteral("%1-hotcue-%2").arg(prefix).arg(pad + 1);
            cue.role = role;
            cue.trackBeat = beats[static_cast<std::size_t>(pad)];
            cue.label = QStringLiteral("HOT CUE %1").arg(pad + 1);
            cue.purpose = QStringLiteral("legacy-hot-cue");
            cue.preferredPad = pad;
            file.transitionCues.push_back(cue);
        }
    };
    if (file.transitionCues.empty()) {
        addRole(Role::FromDeck, file.fromHotCueBeats, QStringLiteral("outgoing"));
        addRole(Role::ToDeck, file.toHotCueBeats, QStringLiteral("incoming"));
    }
    for (GvtEvent& event : file.events) {
        if (event.control < ControlId::HotCue1 ||
            event.control > ControlId::HotCue8 || event.role == Role::Mixer)
            continue;
        const int pad = static_cast<int>(event.control) -
                        static_cast<int>(ControlId::HotCue1);
        event.cueId = QStringLiteral("%1-hotcue-%2")
                          .arg(event.role == Role::FromDeck
                                   ? QStringLiteral("outgoing")
                                   : QStringLiteral("incoming"))
                          .arg(pad + 1);
        event.gestureControl = static_cast<ControlId>(
            static_cast<int>(ControlId::PerformancePad1) + pad);
        event.gesturePadMode = static_cast<int>(PerformancePadMode::Sampler);
    }
}

} // namespace

QString stableLegacyTransitionId(const GvtFile& file)
{
    GvtFile canonical = file;
    canonical.filePath.clear();
    canonical.id.clear();
    const QByteArray digest = QCryptographicHash::hash(
        gvtSerialize(canonical).toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("legacy-%1").arg(QString::fromLatin1(digest.left(32)));
}

bool transitionParse(const QString& text, GvtFile& out, QString* error,
                     QStringList* warnings)
{
    QJsonObject root;
    if (!parseYamlObject(text, root, error)) return false;
    if (stringAt(root, "format") != QLatin1String("gravitino.transition")) {
        if (error) *error = QStringLiteral("format must be 'gravitino.transition'");
        return false;
    }
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    const int version = versionValue.isDouble()
                            ? static_cast<int>(versionValue.toDouble()) : -1;
    if (version != 1 || !versionValue.isDouble() ||
        versionValue.toDouble() != version) {
        if (error) *error = QStringLiteral("unsupported .transition version %1").arg(version);
        return false;
    }

    GvtFile file;
    file.version = version;
    file.sourceFormat = TransitionSourceFormat::PortableYaml;
    file.id = stringAt(root, "id").trimmed();
    if (file.id.isEmpty()) {
        if (error) *error = QStringLiteral("id must be a non-empty string");
        return false;
    }
    if (!root.value(QStringLiteral("metadata")).isObject() ||
        !root.value(QStringLiteral("requires")).isArray() ||
        !root.value(QStringLiteral("endpoints")).isObject() ||
        !root.value(QStringLiteral("performance")).isObject()) {
        if (error) *error = QStringLiteral(
            "metadata, requires, endpoints, and performance have invalid types");
        return false;
    }
    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    file.name = stringAt(metadata, "name");
    file.author = stringAt(metadata, "author");
    file.created = stringAt(metadata, "created_at");
    file.description = stringAt(metadata, "description");
    file.license = stringAt(metadata, "license");
    file.tags = stringArray(metadata.value(QStringLiteral("tags")));
    file.metadataExtraYaml = without(metadata,
        {"name", "author", "created_at", "description", "license", "tags"});

    const QJsonArray requirementValues = root.value(
        QStringLiteral("requires")).toArray();
    for (const QJsonValue& value : requirementValues) {
        if (!value.isString() || value.toString().trimmed().isEmpty()) {
            if (error) *error = QStringLiteral(
                "requires must contain only non-empty strings");
            return false;
        }
        file.requirements.append(value.toString().trimmed());
    }
    const QSet<QString> supported {QStringLiteral("timeline.v1"),
                                    QStringLiteral("temporary-cues.v1")};
    for (const QString& requirement : file.requirements)
        if (!supported.contains(requirement))
            file.unsupportedRequirements.append(requirement);
    if (!file.unsupportedRequirements.isEmpty() && warnings)
        warnings->append(QStringLiteral("unsupported required capabilities: %1")
                             .arg(file.unsupportedRequirements.join(", ")));

    const QJsonObject endpoints = root.value(QStringLiteral("endpoints")).toObject();
    if (!validateEndpointFields(
            endpoints.value(QStringLiteral("outgoing")).toObject(),
            QStringLiteral("endpoints.outgoing"), error) ||
        !validateEndpointFields(
            endpoints.value(QStringLiteral("incoming")).toObject(),
            QStringLiteral("endpoints.incoming"), error))
        return false;
    parseEndpoint(endpoints.value(QStringLiteral("outgoing")).toObject(),
                  file.from, warnings);
    parseEndpoint(endpoints.value(QStringLiteral("incoming")).toObject(),
                  file.to, warnings);
    file.endpointsExtraYaml = without(endpoints, {"outgoing", "incoming"});

    const QJsonObject performance = root.value(QStringLiteral("performance")).toObject();
    if (!finiteField(performance, "master_bpm", 20.0, 400.0, true,
                     QStringLiteral("performance"), error))
        return false;
    file.masterBpm = numberAt(performance, "master_bpm");
    const QJsonObject anchors = performance.value(QStringLiteral("anchors")).toObject();
    if (!anchors.value(QStringLiteral("outgoing")).isObject() ||
        !anchors.value(QStringLiteral("incoming")).isObject() ||
        !finiteField(anchors.value(QStringLiteral("outgoing")).toObject(),
                     "track_beat", -1000000.0, 1000000.0, true,
                     QStringLiteral("performance.anchors.outgoing"), error) ||
        !finiteField(anchors.value(QStringLiteral("incoming")).toObject(),
                     "track_beat", -1000000.0, 1000000.0, true,
                     QStringLiteral("performance.anchors.incoming"), error))
        return false;
    file.anchorFromBeat = numberAt(
        anchors.value(QStringLiteral("outgoing")).toObject(), "track_beat");
    file.anchorToBeat = numberAt(
        anchors.value(QStringLiteral("incoming")).toObject(), "track_beat");
    file.outgoingAnchorExtraYaml = without(
        anchors.value(QStringLiteral("outgoing")).toObject(), {"track_beat"});
    file.incomingAnchorExtraYaml = without(
        anchors.value(QStringLiteral("incoming")).toObject(), {"track_beat"});
    file.anchorsExtraYaml = without(anchors, {"outgoing", "incoming"});

    const QJsonObject initial = performance.value(
        QStringLiteral("initial_state")).toObject();
    if (!performance.value(QStringLiteral("initial_state")).isObject() ||
        !performance.value(QStringLiteral("cues")).isArray() ||
        !performance.value(QStringLiteral("labels")).isArray() ||
        !performance.value(QStringLiteral("timeline")).isArray()) {
        if (error) *error = QStringLiteral(
            "performance initial_state/cues/labels/timeline have invalid types");
        return false;
    }
    const auto validateInitial = [error](const QJsonObject& state,
                                         const QString& path) {
        if (state.isEmpty()) return true;
        const struct Field { const char* name; double minimum; double maximum; } fields[] = {
            {"position_beat", -1000000.0, 1000000.0},
            {"cue_beat", -1000000.0, 1000000.0},
            {"tempo_ratio", 0.01, 4.0}, {"fader", 0.0, 1.0},
            {"trim", 0.0, 1.0}, {"eq_low", 0.0, 1.0},
            {"eq_mid", 0.0, 1.0}, {"eq_high", 0.0, 1.0},
            {"filter", 0.0, 1.0},
            {"loop_start_beat", -1000000.0, 1000000.0},
            {"loop_end_beat", -1000000.0, 1000000.0},
            {"fx_type", 0.0, 2.0}, {"fx_wet", 0.0, 1.0},
            {"fx_beats", 0.25, 4.0}, {"stem_vocals", 0.0, 1.0},
            {"stem_melody", 0.0, 1.0}, {"stem_bass", 0.0, 1.0},
            {"stem_drums", 0.0, 1.0},
        };
        for (const Field& field : fields)
            if (!finiteField(state, field.name, field.minimum, field.maximum,
                             false, path, error))
                return false;
        const QJsonValue fxType = state.value(QStringLiteral("fx_type"));
        if (fxType.isDouble() && std::floor(fxType.toDouble()) !=
                                     fxType.toDouble()) {
            if (error) *error = path + QStringLiteral(".fx_type must be an integer");
            return false;
        }
        if (boolAt(state, "loop_active") &&
            numberAt(state, "loop_end_beat") <=
                numberAt(state, "loop_start_beat")) {
            if (error) *error = path +
                QStringLiteral(" has an invalid active loop range");
            return false;
        }
        return true;
    };
    if (!validateInitial(initial.value(QStringLiteral("outgoing")).toObject(),
                         QStringLiteral("performance.initial_state.outgoing")) ||
        !validateInitial(initial.value(QStringLiteral("incoming")).toObject(),
                         QStringLiteral("performance.initial_state.incoming")))
        return false;
    const QJsonObject mixerState = initial.value(
        QStringLiteral("mixer")).toObject();
    if (!finiteField(mixerState, "crossfader", 0.0, 1.0, false,
                     QStringLiteral("performance.initial_state.mixer"), error))
        return false;
    file.initialComplete = boolAt(initial, "complete");
    const QJsonObject mixer = initial.value(QStringLiteral("mixer")).toObject();
    file.initialMixerCaptured = boolAt(mixer, "captured", !mixer.isEmpty());
    file.initialCrossfader = numberAt(mixer, "crossfader");
    file.mixerInitialExtraYaml = without(mixer, {"captured", "crossfader"});
    parseInitialDeck(initial.value(QStringLiteral("outgoing")).toObject(),
                     file.initialFrom);
    parseInitialDeck(initial.value(QStringLiteral("incoming")).toObject(),
                     file.initialTo);
    file.initialStateExtraYaml = without(initial,
        {"complete", "mixer", "outgoing", "incoming"});

    QSet<QString> cueIds;
    std::map<QString, Role> cueRoles;
    int outgoingCues = 0;
    int incomingCues = 0;
    const QJsonArray transitionCues = performance.value(
        QStringLiteral("cues")).toArray();
    for (const QJsonValue& value : transitionCues) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("performance.cues entries must be mappings");
            return false;
        }
        const QJsonObject item = value.toObject();
        TransitionHotCue cue;
        cue.id = stringAt(item, "id").trimmed();
        if (cue.id.isEmpty() || cueIds.contains(cue.id)) {
            if (error) *error = QStringLiteral("transition cue IDs must be non-empty and unique");
            return false;
        }
        cueIds.insert(cue.id);
        if (!parseRole(stringAt(item, "endpoint"), cue.role) ||
            cue.role == Role::Mixer) {
            if (error) *error = QStringLiteral("transition cue '%1' has an invalid endpoint")
                                    .arg(cue.id);
            return false;
        }
        if (!finiteField(item, "track_beat", -1000000.0, 1000000.0, true,
                         QStringLiteral("performance.cues[%1]")
                             .arg(file.transitionCues.size()), error)) {
            return false;
        }
        cue.trackBeat = numberAt(item, "track_beat");
        cue.label = stringAt(item, "label");
        cue.purpose = stringAt(item, "purpose");
        cue.color = stringAt(item, "color");
        cue.pairingGroup = stringAt(item, "pairing_group");
        const QJsonObject input = item.value(QStringLiteral("preferred_input")).toObject();
        cue.preferredBank = stringAt(input, "bank");
        if (cue.preferredBank.isEmpty()) cue.preferredBank = QStringLiteral("custom");
        if (cue.preferredBank != QLatin1String("custom")) {
            if (error) *error = QStringLiteral(
                "transition cue '%1' requests an unsupported bank").arg(cue.id);
            return false;
        }
        const QJsonValue padValue = input.value(QStringLiteral("pad"));
        if (!padValue.isUndefined() &&
            (!padValue.isDouble() || std::floor(padValue.toDouble()) !=
                                         padValue.toDouble() ||
             padValue.toDouble() < 1.0 || padValue.toDouble() > 8.0)) {
            if (error) *error = QStringLiteral(
                "transition cue '%1' preferred pad must be 1 through 8")
                                    .arg(cue.id);
            return false;
        }
        cue.preferredPad = padValue.isUndefined()
                               ? -1 : static_cast<int>(padValue.toDouble()) - 1;
        cue.preferredKey = stringAt(input, "key");
        cue.inputExtraYaml = without(input, {"bank", "pad", "key"});
        cue.extraYaml = without(item,
            {"id", "endpoint", "track_beat", "label", "purpose", "color",
             "pairing_group", "preferred_input"});
        if (cue.role == Role::FromDeck) ++outgoingCues;
        else ++incomingCues;
        cueRoles.emplace(cue.id, cue.role);
        file.transitionCues.push_back(cue);
    }
    if (outgoingCues > 8 || incomingCues > 8) {
        if (error) *error = QStringLiteral("v1 supports at most eight transition cues per endpoint");
        return false;
    }

    const QJsonArray labels = performance.value(QStringLiteral("labels")).toArray();
    if (labels.size() > kMaximumLabels) {
        if (error) *error = QStringLiteral("too many transition labels");
        return false;
    }
    for (const QJsonValue& value : labels) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("performance.labels entries must be mappings");
            return false;
        }
        const QJsonObject item = value.toObject();
        const double beat = numberAt(item, "at_beat",
                                     std::numeric_limits<double>::quiet_NaN());
        const QString label = stringAt(item, "label").trimmed();
        if (!std::isfinite(beat) || label.isEmpty()) {
            if (error) *error = QStringLiteral(
                "performance.labels entries need a finite at_beat and label");
            return false;
        }
        file.cues.push_back({beat, label,
                            without(item, {"at_beat", "label"})});
    }

    const QJsonArray timeline = performance.value(QStringLiteral("timeline")).toArray();
    if (timeline.size() > kMaximumTimelineEvents) {
        if (error) *error = QStringLiteral("timeline exceeds the 20,000-event limit");
        return false;
    }
    for (int i = 0; i < timeline.size(); ++i) {
        if (!timeline.at(i).isObject()) {
            if (error) *error = QStringLiteral(
                "timeline event %1 must be a mapping").arg(i + 1);
            return false;
        }
        const QJsonObject item = timeline.at(i).toObject();
        GvtEvent event;
        event.beat = numberAt(item, "at_beat",
                              std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(event.beat)) {
            if (error) *error = QStringLiteral("timeline event %1 has an invalid beat").arg(i + 1);
            return false;
        }
        if (!parseRole(stringAt(item, "target"), event.role)) {
            if (error) *error = QStringLiteral("timeline event %1 has an invalid target").arg(i + 1);
            return false;
        }
        const QString controlNameText = stringAt(item, "control");
        const bool semanticCueControl =
            controlNameText == QLatin1String("deck.transition_cue");
        if (semanticCueControl) {
            event.control = ControlId::TransitionCue1;
        } else if (!parsePortableControl(controlNameText, event.control)) {
            if (error) *error = QStringLiteral(
                "timeline event %1 uses an unknown control").arg(i + 1);
            return false;
        }
        const QString expectedPrefix = event.role == Role::Mixer
                                           ? QStringLiteral("mixer.")
                                           : QStringLiteral("deck.");
        if (!controlNameText.startsWith(expectedPrefix) ||
            !portableTimelineControlAllowed(event.control) ||
            (!semanticCueControl &&
             event.control >= ControlId::TransitionCue1 &&
             event.control <= ControlId::TransitionCue8) ||
            (event.role == Role::Mixer &&
             event.control != ControlId::Crossfader) ||
            (event.role != Role::Mixer &&
             event.control == ControlId::Crossfader)) {
            if (error) *error = QStringLiteral(
                "timeline event %1 has an invalid control/target combination")
                                    .arg(i + 1);
            return false;
        }
        const QJsonValue eventValue = item.value(QStringLiteral("value"));
        if (!eventValue.isDouble() ||
            !validateTimelineValue(event.control, eventValue.toDouble(),
                                   error, i + 1))
            return false;
        event.value = eventValue.toDouble();
        if (!parseCurvePortable(stringAt(item, "curve"), event.curve)) {
            if (error) *error = QStringLiteral(
                "timeline event %1 has an invalid curve").arg(i + 1);
            return false;
        }
        event.cueId = stringAt(item, "cue_id").trimmed();
        if (!event.cueId.isEmpty() && !cueIds.contains(event.cueId)) {
            if (error) *error = QStringLiteral("timeline event %1 refers to unknown cue '%2'")
                                    .arg(i + 1).arg(event.cueId);
            return false;
        }
        if (semanticCueControl && event.cueId.isEmpty()) {
            if (error) *error = QStringLiteral(
                "timeline event %1 needs cue_id for deck.transition_cue")
                                    .arg(i + 1);
            return false;
        }
        if (!event.cueId.isEmpty() && cueRoles[event.cueId] != event.role) {
            if (error) *error = QStringLiteral(
                "timeline event %1 cue belongs to the other endpoint")
                                    .arg(i + 1);
            return false;
        }
        if (!event.cueId.isEmpty() && !semanticCueControl &&
            !(event.control >= ControlId::HotCue1 &&
              event.control <= ControlId::HotCue8) &&
            !(event.control >= ControlId::TransitionCue1 &&
              event.control <= ControlId::TransitionCue8)) {
            if (error) *error = QStringLiteral(
                "timeline event %1 attaches cue_id to a non-cue control")
                                    .arg(i + 1);
            return false;
        }
        if ((event.control >= ControlId::HotCue1 &&
             event.control <= ControlId::HotCue8) && event.cueId.isEmpty()) {
            if (error) *error = QStringLiteral(
                "timeline event %1 must reference a semantic cue ID")
                                    .arg(i + 1);
            return false;
        }
        if (controlIsTrigger(event.control) && event.curve != Curve::Step) {
            if (error) *error = QStringLiteral(
                "timeline event %1 cannot glide a state/button control")
                                    .arg(i + 1);
            return false;
        }
        const QJsonObject input = item.value(QStringLiteral("input_hint")).toObject();
        event.inputExtraYaml = input;
        if (!input.isEmpty()) {
            if (parsePortableControl(stringAt(input, "control"),
                                     event.gestureControl))
                event.inputExtraYaml.remove(QStringLiteral("control"));
            const int padMode = parsePadMode(
                input.value(QStringLiteral("pad_mode")));
            if (padMode >= 0) {
                event.gesturePadMode = padMode;
                event.inputExtraYaml.remove(QStringLiteral("pad_mode"));
            }
        }
        event.extraYaml = without(item,
            {"at_beat", "target", "control", "value", "curve", "cue_id",
             "input_hint"});
        file.events.push_back(event);
    }
    std::stable_sort(file.events.begin(), file.events.end(),
                     [](const GvtEvent& a, const GvtEvent& b) {
                         return a.beat < b.beat;
                     });
    std::stable_sort(file.cues.begin(), file.cues.end(),
                     [](const GvtCue& a, const GvtCue& b) {
                         return a.beat < b.beat;
                     });

    file.performanceExtraYaml = without(performance,
        {"master_bpm", "anchors", "initial_state", "cues", "labels", "timeline"});
    file.extensions = root.value(QStringLiteral("extensions")).toObject();
    file.legacySourceId = file.extensions
        .value(QStringLiteral("gravitino.legacy"))
        .toObject().value(QStringLiteral("source_id")).toString();
    file.extraYaml = without(root,
        {"format", "version", "id", "metadata", "requires", "endpoints",
         "performance", "extensions"});
    ensurePortableDefaults(file);
    out = std::move(file);
    return true;
}

QString transitionSerialize(const GvtFile& source)
{
    GvtFile file = source;
    ensurePortableDefaults(file);
    QJsonObject root = file.extraYaml;
    root.insert(QStringLiteral("format"), QStringLiteral("gravitino.transition"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("id"), file.id);

    QJsonObject metadata = file.metadataExtraYaml;
    metadata.insert(QStringLiteral("name"), file.name);
    setIfText(metadata, QStringLiteral("author"), file.author);
    setIfText(metadata, QStringLiteral("created_at"), file.created);
    setIfText(metadata, QStringLiteral("description"), file.description);
    setIfText(metadata, QStringLiteral("license"), file.license);
    metadata.insert(QStringLiteral("tags"), stringsJson(file.tags));
    root.insert(QStringLiteral("metadata"), metadata);
    root.insert(QStringLiteral("requires"), stringsJson(file.requirements));

    QJsonObject endpoints = file.endpointsExtraYaml;
    endpoints.insert(QStringLiteral("outgoing"), endpointJson(file.from));
    endpoints.insert(QStringLiteral("incoming"), endpointJson(file.to));
    root.insert(QStringLiteral("endpoints"), endpoints);

    QJsonObject performance = file.performanceExtraYaml;
    performance.insert(QStringLiteral("master_bpm"), file.masterBpm);
    QJsonObject anchors = file.anchorsExtraYaml;
    QJsonObject outgoingAnchor = file.outgoingAnchorExtraYaml;
    outgoingAnchor.insert(QStringLiteral("track_beat"), file.anchorFromBeat);
    QJsonObject incomingAnchor = file.incomingAnchorExtraYaml;
    incomingAnchor.insert(QStringLiteral("track_beat"), file.anchorToBeat);
    anchors.insert(QStringLiteral("outgoing"), outgoingAnchor);
    anchors.insert(QStringLiteral("incoming"), incomingAnchor);
    performance.insert(QStringLiteral("anchors"), anchors);

    QJsonObject initial = file.initialStateExtraYaml;
    initial.insert(QStringLiteral("complete"), file.initialComplete);
    QJsonObject mixer = file.mixerInitialExtraYaml;
    mixer.insert(QStringLiteral("captured"), file.initialMixerCaptured);
    mixer.insert(QStringLiteral("crossfader"), file.initialCrossfader);
    initial.insert(QStringLiteral("mixer"), mixer);
    initial.insert(QStringLiteral("outgoing"), initialDeckJson(file.initialFrom));
    initial.insert(QStringLiteral("incoming"), initialDeckJson(file.initialTo));
    performance.insert(QStringLiteral("initial_state"), initial);

    QJsonArray cueArray;
    for (const TransitionHotCue& cue : file.transitionCues) {
        QJsonObject item = cue.extraYaml;
        item.insert(QStringLiteral("id"), cue.id);
        item.insert(QStringLiteral("endpoint"), roleName(cue.role));
        item.insert(QStringLiteral("track_beat"), cue.trackBeat);
        setIfText(item, QStringLiteral("label"), cue.label);
        setIfText(item, QStringLiteral("purpose"), cue.purpose);
        setIfText(item, QStringLiteral("color"), cue.color);
        setIfText(item, QStringLiteral("pairing_group"), cue.pairingGroup);
        QJsonObject input = cue.inputExtraYaml;
        input.insert(QStringLiteral("bank"),
                     cue.preferredBank.isEmpty() ? QStringLiteral("custom")
                                                 : cue.preferredBank);
        if (cue.preferredPad >= 0)
            input.insert(QStringLiteral("pad"), cue.preferredPad + 1);
        else input.remove(QStringLiteral("pad"));
        setIfText(input, QStringLiteral("key"), cue.preferredKey);
        item.insert(QStringLiteral("preferred_input"), input);
        cueArray.append(item);
    }
    performance.insert(QStringLiteral("cues"), cueArray);

    QJsonArray labels;
    for (const GvtCue& cue : file.cues) {
        QJsonObject item = cue.extraYaml;
        item.insert(QStringLiteral("at_beat"), cue.beat);
        item.insert(QStringLiteral("label"), cue.label);
        labels.append(item);
    }
    performance.insert(QStringLiteral("labels"), labels);

    QJsonArray timeline;
    for (const GvtEvent& event : file.events) {
        QJsonObject item = event.extraYaml;
        item.insert(QStringLiteral("at_beat"), event.beat);
        item.insert(QStringLiteral("target"), roleName(event.role));
        item.insert(QStringLiteral("control"), event.cueId.isEmpty()
            ? portableControlName(event.control, event.role)
            : QStringLiteral("deck.transition_cue"));
        item.insert(QStringLiteral("value"), event.value);
        item.insert(QStringLiteral("curve"), curveNamePortable(event.curve));
        setIfText(item, QStringLiteral("cue_id"), event.cueId);
        if (event.gestureControl != ControlId::Count) {
            QJsonObject input = event.inputExtraYaml;
            input.insert(QStringLiteral("control"),
                         QStringLiteral("host.") +
                             QString::fromLatin1(controlName(event.gestureControl)));
            const QString mode = padModeName(event.gesturePadMode);
            if (!mode.isEmpty()) input.insert(QStringLiteral("pad_mode"), mode);
            item.insert(QStringLiteral("input_hint"), input);
        } else if (!event.inputExtraYaml.isEmpty()) {
            item.insert(QStringLiteral("input_hint"), event.inputExtraYaml);
        } else {
            item.remove(QStringLiteral("input_hint"));
        }
        timeline.append(item);
    }
    performance.insert(QStringLiteral("timeline"), timeline);
    root.insert(QStringLiteral("performance"), performance);
    root.insert(QStringLiteral("extensions"), file.extensions);

    QString output;
    output += QStringLiteral("# Gravitino portable transition — beat positions may be fractional.\n");
    emitYamlObject(output, root, 0);
    return output;
}

bool transitionLoadFile(const QString& path, GvtFile& out, QString* error,
                        QStringList* warnings)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot open %1: %2")
                                .arg(path, file.errorString());
        return false;
    }
    if (!transitionParse(QString::fromUtf8(file.readAll()), out, error, warnings))
        return false;
    out.filePath = QFileInfo(path).absoluteFilePath();
    out.sourceFormat = TransitionSourceFormat::PortableYaml;
    return true;
}

bool transitionSaveFile(const GvtFile& file, const QString& path, QString* error)
{
    const QString serialized = transitionSerialize(file);
    GvtFile validated;
    QStringList warnings;
    if (!transitionParse(serialized, validated, error, &warnings))
        return false;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot write %1: %2")
                                .arg(path, output.errorString());
        return false;
    }
    const QByteArray bytes = serialized.toUtf8();
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        if (error) *error = QStringLiteral("cannot save %1: %2")
                                .arg(path, output.errorString());
        return false;
    }
    return true;
}

bool loadTransitionFile(const QString& path, GvtFile& out, QString* error,
                        QStringList* warnings)
{
    if (QFileInfo(path).suffix().compare(QStringLiteral("gvt"),
                                         Qt::CaseInsensitive) != 0)
        return transitionLoadFile(path, out, error, warnings);
    if (!gvtLoadFile(path, out, error, warnings)) return false;
    out.sourceFormat = TransitionSourceFormat::LegacyGvt;
    out.id = stableLegacyTransitionId(out);
    ensurePortableDefaults(out);
    addLegacyTransitionCues(out);
    return true;
}

bool saveTransitionFile(const GvtFile& file, const QString& path, QString* error)
{
    return transitionSaveFile(file, path, error);
}

std::array<const TransitionHotCue*, 8>
transitionCueSlots(const GvtFile& file, Role role)
{
    std::array<const TransitionHotCue*, 8> cueSlots {};
    for (const TransitionHotCue& cue : file.transitionCues) {
        if (cue.role != role || cue.preferredPad < 0 || cue.preferredPad >= 8 ||
            cueSlots[static_cast<std::size_t>(cue.preferredPad)])
            continue;
        cueSlots[static_cast<std::size_t>(cue.preferredPad)] = &cue;
    }
    for (const TransitionHotCue& cue : file.transitionCues) {
        if (cue.role != role ||
            std::find(cueSlots.begin(), cueSlots.end(), &cue) != cueSlots.end())
            continue;
        const auto free = std::find(cueSlots.begin(), cueSlots.end(), nullptr);
        if (free != cueSlots.end()) *free = &cue;
    }
    return cueSlots;
}

double transitionBeatAtSec(const GvtFile& file, const TrackData& track,
                           double seconds)
{
    return file.sourceFormat == TransitionSourceFormat::PortableYaml
               ? track.canonicalBeatAtSec(seconds)
               : track.beatAtSec(seconds);
}

double transitionSecAtBeat(const GvtFile& file, const TrackData& track,
                           double beat)
{
    return file.sourceFormat == TransitionSourceFormat::PortableYaml
               ? track.secAtCanonicalBeat(beat)
               : track.secAtBeat(beat);
}

} // namespace gvt
