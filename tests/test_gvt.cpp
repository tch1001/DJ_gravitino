// .gvt parser/serializer test: parses the spec example from
// docs/TRANSITION_FORMAT.md, checks fields, unknown-control skipping, and
// lossless round-trip. Returns 0 on pass. Owner: claude-transitions.
#include <QString>
#include <QStringList>
#include <cmath>
#include <cstdio>
#include "transitions/Transition.h"

namespace {

int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

const char* kExample = R"gvt(gravitino-transition 1

[meta]
name        = Slam on the drop
author      = fish
created     = 2026-08-15
description = Kill the lows, slam the crossfader on A's last drop.

[from]
title       = Demo Track 1
artist      = PioneerDJ
bpm         = 130.00
duration    = 121.36
fingerprint = gvfp1:9f83a2c11d40be77

[to]
title       = Demo Track 2
artist      = PioneerDJ
bpm         = 128.00
duration    = 118.02
fingerprint = gvfp1:31b0cc04a9e15df2

[sync]
anchor_from = 224.0     ; beat in FROM track where the transition begins
anchor_to   = 32.0      ; beat in TO track aligned to transition beat 0
master_bpm  = 130.00    ; tempo the mix runs at during the transition

[events]
; beat | target | control | value | curve
0.000    b        load
0.000    b        tempo_sync
0.000    b        eq_low      0.00
0.000    b        fader       1.00
0.000    b        play
0.031    x        xfader      0.00
8.000    x        xfader      0.50   scurve
16.000   b        eq_low      1.00   linear
16.000   a        eq_low      0.00   linear
24.000   x        xfader      1.00   scurve
32.000   a        stop
)gvt";

bool eventsEqual(const gvt::GvtEvent& a, const gvt::GvtEvent& b) {
    return near(a.beat, b.beat) && a.role == b.role && a.control == b.control &&
           near(a.value, b.value) && a.curve == b.curve;
}

bool refsEqual(const gvt::GvtTrackRef& a, const gvt::GvtTrackRef& b) {
    return a.title == b.title && a.artist == b.artist &&
           a.fingerprint == b.fingerprint && near(a.bpm, b.bpm) &&
           near(a.durationSec, b.durationSec);
}

// Deep compare of every known field (filePath excluded — not serialized).
bool filesEqual(const gvt::GvtFile& a, const gvt::GvtFile& b) {
    if (a.version != b.version) return false;
    if (a.name != b.name || a.author != b.author || a.created != b.created ||
        a.description != b.description) return false;
    if (a.extraMeta != b.extraMeta) return false;
    if (!refsEqual(a.from, b.from) || !refsEqual(a.to, b.to)) return false;
    if (!near(a.anchorFromBeat, b.anchorFromBeat) ||
        !near(a.anchorToBeat, b.anchorToBeat) ||
        !near(a.masterBpm, b.masterBpm)) return false;
    if (a.events.size() != b.events.size()) return false;
    for (size_t i = 0; i < a.events.size(); ++i)
        if (!eventsEqual(a.events[i], b.events[i])) return false;
    return true;
}

} // namespace

int main() {
    using namespace gvt;

    // ---- parse the spec example -------------------------------------------
    GvtFile f;
    QString error;
    QStringList warnings;
    CHECK(gvtParse(QString::fromUtf8(kExample), f, &error, &warnings));
    if (!error.isEmpty())
        std::fprintf(stderr, "parse error: %s\n", qPrintable(error));
    CHECK(warnings.isEmpty());

    CHECK(f.version == 1);
    CHECK(f.name == QStringLiteral("Slam on the drop"));
    CHECK(f.author == QStringLiteral("fish"));
    CHECK(f.created == QStringLiteral("2026-08-15"));
    CHECK(f.description ==
          QStringLiteral("Kill the lows, slam the crossfader on A's last drop."));
    CHECK(f.extraMeta.empty());

    CHECK(f.from.title == QStringLiteral("Demo Track 1"));
    CHECK(f.from.artist == QStringLiteral("PioneerDJ"));
    CHECK(near(f.from.bpm, 130.0));
    CHECK(near(f.from.durationSec, 121.36));
    CHECK(f.from.fingerprint == QStringLiteral("gvfp1:9f83a2c11d40be77"));
    CHECK(f.to.title == QStringLiteral("Demo Track 2"));
    CHECK(near(f.to.bpm, 128.0));
    CHECK(near(f.to.durationSec, 118.02));
    CHECK(f.to.fingerprint == QStringLiteral("gvfp1:31b0cc04a9e15df2"));

    CHECK(near(f.anchorFromBeat, 224.0));
    CHECK(near(f.anchorToBeat, 32.0));
    CHECK(near(f.masterBpm, 130.0));

    CHECK(f.events.size() == 11);

    // The "8.000 x xfader 0.50 scurve" line. With five beat-0 events plus the
    // 0.031 xfader, sorted order puts it at index 6.
    if (f.events.size() == 11) {
        const GvtEvent& e = f.events[6];
        CHECK(near(e.beat, 8.0));
        CHECK(e.role == Role::Mixer);
        CHECK(e.control == ControlId::Crossfader);
        CHECK(near(e.value, 0.5));
        CHECK(e.curve == Curve::SCurve);
        // Neighbors, to pin the sorted ordering.
        CHECK(near(f.events[5].beat, 0.031));
        CHECK(f.events[0].control == ControlId::Load);
        CHECK(f.events[10].control == ControlId::Stop);
        CHECK(f.events[10].role == Role::FromDeck);
        // Triggers default value, step curve.
        CHECK(f.events[4].control == ControlId::Play);
        CHECK(f.events[4].curve == Curve::Step);
        // eq_low linear pair at beat 16.
        CHECK(f.events[7].control == ControlId::EqLow);
        CHECK(f.events[7].curve == Curve::Linear);
    }

    // ---- unknown control: skipped with exactly one warning ----------------
    {
        QString withUnknown = QString::fromUtf8(kExample);
        withUnknown.replace(QStringLiteral("8.000    x        xfader"),
                            QStringLiteral("5.0 b flux_capacitor 1.0\n"
                                           "8.000    x        xfader"));
        GvtFile g;
        QStringList w2;
        QString e2;
        CHECK(gvtParse(withUnknown, g, &e2, &w2));
        CHECK(w2.size() == 1);
        CHECK(!w2.isEmpty() && w2.first().contains(QStringLiteral("flux_capacitor")));
        CHECK(g.events.size() == 11);  // unknown line skipped, rest kept
        CHECK(filesEqual(f, g));
    }

    // ---- unknown meta keys preserved and re-emitted -----------------------
    {
        QString withExtra = QString::fromUtf8(kExample);
        withExtra.replace(QStringLiteral("[meta]\n"),
                          QStringLiteral("[meta]\nenergy      = 9\n"));
        withExtra.replace(QStringLiteral("[sync]\n"),
                          QStringLiteral("[sync]\nquantize    = 0.25\n"));
        GvtFile g;
        QStringList w2;
        CHECK(gvtParse(withExtra, g, nullptr, &w2));
        CHECK(w2.isEmpty());
        CHECK(g.extraMeta.size() == 2);
        CHECK(g.extraMeta.count(QStringLiteral("energy")) == 1 &&
              g.extraMeta.at(QStringLiteral("energy")) == QStringLiteral("9"));
        CHECK(g.extraMeta.count(QStringLiteral("sync.quantize")) == 1 &&
              g.extraMeta.at(QStringLiteral("sync.quantize")) == QStringLiteral("0.25"));
        GvtFile g2;
        CHECK(gvtParse(gvtSerialize(g), g2, nullptr, nullptr));
        CHECK(filesEqual(g, g2));
    }

    // ---- round-trip: parse -> serialize -> parse is lossless --------------
    {
        const QString text = gvtSerialize(f);
        CHECK(text.startsWith(QStringLiteral("gravitino-transition 1\n")));
        GvtFile g;
        QStringList w2;
        QString e2;
        CHECK(gvtParse(text, g, &e2, &w2));
        CHECK(w2.isEmpty());
        CHECK(filesEqual(f, g));
    }

    // ---- structural failure: bad magic ------------------------------------
    {
        GvtFile g;
        QString e2;
        CHECK(!gvtParse(QStringLiteral("not-a-gvt-file\n"), g, &e2, nullptr));
        CHECK(!e2.isEmpty());
    }

    if (failures) {
        std::fprintf(stderr, "test_gvt: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_gvt: all checks passed\n");
    return 0;
}
