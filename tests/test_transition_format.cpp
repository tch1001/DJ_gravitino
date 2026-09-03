// Portable transition format tests — protect YAML safety, extensibility,
// fractional timing, semantic cues, and the non-destructive legacy adapter.

#include "transitions/Transition.h"

#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)
}

int main()
{
    using namespace gvt;
    GvtFile file;
    file.id = QStringLiteral("d7f55bd5-618d-4f74-ad35-6d55a1a5f963");
    file.name = QStringLiteral("Fractional mix");
    file.author = QStringLiteral("DJ Test");
    file.license = QStringLiteral("CC-BY-4.0");
    file.tags = {QStringLiteral("house"), QStringLiteral("teaching")};
    file.requirements = {QStringLiteral("timeline.v1"),
                         QStringLiteral("temporary-cues.v1")};
    file.from.title = QStringLiteral("Outgoing");
    file.from.artists = {QStringLiteral("Artist A")};
    file.from.artist = QStringLiteral("Artist A");
    file.from.bpm = 128.0;
    file.from.durationSec = 180.0;
    file.from.durationBeats = 384.0;
    file.from.referenceDownbeatSec = 0.125;
    file.from.fingerprints.push_back(
        {QStringLiteral("gravitino-structure-1"),
         QStringLiteral("gvsf1:0123456789abcdef0123456789abcdef"), {}});
    file.to = file.from;
    file.to.title = QStringLiteral("Incoming");
    file.anchorFromBeat = 296.452345678;
    file.anchorToBeat = 32.125;
    file.masterBpm = 127.5;
    file.initialComplete = true;
    file.initialFrom.captured = true;
    file.initialTo.captured = true;
    file.initialMixerCaptured = true;
    file.transitionCues.push_back(
        {QStringLiteral("incoming-launch"), Role::ToDeck, 32.125,
         QStringLiteral("Launch"), QStringLiteral("start-track"),
         QStringLiteral("#55b9df"), QStringLiteral("launch"),
         QStringLiteral("custom"), 2, QStringLiteral("3"), {}, {}});
    GvtEvent event;
    event.beat = 12.375123456;
    event.role = Role::ToDeck;
    event.control = ControlId::HotCue3;
    event.value = 1.0;
    event.cueId = QStringLiteral("incoming-launch");
    event.extraYaml.insert(QStringLiteral("future_event_field"), 7);
    event.inputExtraYaml.insert(QStringLiteral("future_gesture_field"),
                                QStringLiteral("kept"));
    file.events.push_back(event);
    file.cues.push_back({12.375123456, QStringLiteral("Between beats"), {}});
    file.metadataExtraYaml.insert(QStringLiteral("future_credit"),
                                  QStringLiteral("guest"));
    file.from.assumptionsExtraYaml.insert(QStringLiteral("future_grid"), true);
    file.outgoingAnchorExtraYaml.insert(QStringLiteral("future_anchor"), 4);
    file.extensions.insert(QStringLiteral("example.vendor"),
                           QJsonObject{{QStringLiteral("enabled"), true}});

    const QString yaml = transitionSerialize(file);
    CHECK(yaml.contains(QStringLiteral("format: \"gravitino.transition\"")));
    CHECK(yaml.contains(QStringLiteral("at_beat: 12.375123456")));
    CHECK(yaml.contains(QStringLiteral("control: \"deck.transition_cue\"")));

    GvtFile parsed;
    QString error;
    QStringList warnings;
    CHECK(transitionParse(yaml, parsed, &error, &warnings));
    CHECK(error.isEmpty());
    CHECK(parsed.id == file.id);
    CHECK(parsed.name == file.name);
    CHECK(parsed.license == file.license);
    CHECK(parsed.from.artists == file.from.artists);
    CHECK(std::fabs(parsed.anchorFromBeat - file.anchorFromBeat) < 1e-10);
    CHECK(parsed.events.size() == 1);
    CHECK(std::fabs(parsed.events[0].beat - event.beat) < 1e-10);
    CHECK(parsed.events[0].cueId == event.cueId);
    CHECK(parsed.transitionCues.size() == 1);
    CHECK(parsed.transitionCues[0].preferredPad == 2);
    CHECK(parsed.metadataExtraYaml.value(QStringLiteral("future_credit")) ==
          QStringLiteral("guest"));
    CHECK(parsed.from.assumptionsExtraYaml.value(
              QStringLiteral("future_grid")).toBool());
    CHECK(parsed.events[0].extraYaml.value(
              QStringLiteral("future_event_field")).toInt() == 7);
    CHECK(parsed.events[0].inputExtraYaml.value(
              QStringLiteral("future_gesture_field")) == QStringLiteral("kept"));
    CHECK(parsed.outgoingAnchorExtraYaml.value(
              QStringLiteral("future_anchor")).toInt() == 4);
    CHECK(parsed.extensions.contains(QStringLiteral("example.vendor")));
    CHECK(transitionSerialize(parsed) == yaml);

    const QString unsupported = QString(yaml).replace(
        QStringLiteral("temporary-cues.v1"), QStringLiteral("warp-grid.v9"));
    CHECK(transitionParse(unsupported, parsed, &error, &warnings));
    CHECK(parsed.unsupportedRequirements.contains(QStringLiteral("warp-grid.v9")));

    CHECK(!transitionParse(QStringLiteral(
        "format: gravitino.transition\nversion: 1\nid: &id unsafe\ncopy: *id\n"),
        parsed, &error, nullptr));
    CHECK(error.contains(QStringLiteral("anchor"), Qt::CaseInsensitive) ||
          error.contains(QStringLiteral("alias"), Qt::CaseInsensitive));
    CHECK(!transitionParse(QStringLiteral(
        "format: gravitino.transition\nformat: duplicate\nversion: 1\n"),
        parsed, &error, nullptr));
    CHECK(error.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));

    QString invalid = yaml;
    invalid.replace(QStringLiteral("curve: \"step\""),
                    QStringLiteral("curve: \"bezier\""));
    CHECK(!transitionParse(invalid, parsed, &error, nullptr));
    CHECK(error.contains(QStringLiteral("curve"), Qt::CaseInsensitive));
    invalid = yaml;
    invalid.replace(QStringLiteral("at_beat: 12.375123456"),
                    QStringLiteral("at_beat: .nan"));
    CHECK(!transitionParse(invalid, parsed, &error, nullptr));
    invalid = yaml;
    invalid.replace(QStringLiteral("pad: 3"), QStringLiteral("pad: 9"));
    CHECK(!transitionParse(invalid, parsed, &error, nullptr));
    CHECK(!transitionParse(yaml + QStringLiteral("---\n{}\n"),
                           parsed, &error, nullptr));

    GvtFile tooManyCues = file;
    for (int index = 0; index < 8; ++index) {
        TransitionHotCue cue = tooManyCues.transitionCues.front();
        cue.id = QStringLiteral("extra-%1").arg(index);
        cue.preferredPad = -1;
        tooManyCues.transitionCues.push_back(cue);
    }
    CHECK(!transitionParse(transitionSerialize(tooManyCues), parsed,
                           &error, nullptr));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString legacyPath = dir.filePath(QStringLiteral("old.gvt"));
    QFile legacy(legacyPath);
    CHECK(legacy.open(QIODevice::WriteOnly | QIODevice::Text));
    legacy.write("gravitino-transition 1\n\n[meta]\nname = old\n\n"
                 "[from]\ntitle = A\nartist = X\nbpm = 120\nduration = 60\n"
                 "fingerprint = gvfp1:a\n\n[to]\ntitle = B\nartist = Y\n"
                 "bpm = 120\nduration = 60\nfingerprint = gvfp1:b\n\n"
                 "[sync]\nanchor_from = -0.25\nanchor_to = 4.5\nmaster_bpm = 120\n\n"
                 "[hotcues]\nb1 = 4.5\n\n[events]\n0.125 b hotcue_1 1\n");
    legacy.close();
    GvtFile imported;
    CHECK(loadTransitionFile(legacyPath, imported, &error, &warnings));
    CHECK(imported.sourceFormat == TransitionSourceFormat::LegacyGvt);
    CHECK(imported.id.startsWith(QStringLiteral("legacy-")));
    CHECK(imported.transitionCues.size() == 1);
    CHECK(imported.events[0].cueId == QStringLiteral("incoming-hotcue-1"));
    CHECK(std::fabs(imported.anchorFromBeat + 0.25) < 1e-12);

    if (failures) return 1;
    std::printf("test_transition_format: portable YAML and legacy adapter passed\n");
    return 0;
}
