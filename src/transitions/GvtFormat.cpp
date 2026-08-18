// .gvt parse/serialize — implements docs/TRANSITION_FORMAT.md (v1).
// Owner: claude-transitions. NOTE: gvt::matchTrack() lives in
// src/library/TransitionStore.cpp (analysis agent) — not here.
#include "Transition.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace gvt {

namespace {

// ---------------------------------------------------------------- helpers ---

// Strip ';' / '#' comments (whole-line or trailing) and trim.
QString stripComment(const QString& line) {
    int cut = -1;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char(';') || c == QLatin1Char('#')) { cut = i; break; }
    }
    return (cut >= 0 ? line.left(cut) : line).trimmed();
}

bool parseDouble(const QString& s, double& out) {
    bool ok = false;
    out = s.toDouble(&ok);
    return ok;
}

bool parseBool(const QString& s) {
    const QString value = s.trimmed().toLower();
    return value == QLatin1String("true") || value == QLatin1String("yes") ||
           value == QLatin1String("on") || value.toDouble() != 0.0;
}

// Smallest fixed precision in [minDec..maxDec] that reproduces v exactly
// (within 1e-9); falls back to full precision so round-trips stay lossless.
QString fmtNum(double v, int minDec, int maxDec) {
    for (int d = minDec; d <= maxDec; ++d) {
        QString s = QString::number(v, 'f', d);
        if (std::fabs(s.toDouble() - v) < 1e-9)
            return s;
    }
    return QString::number(v, 'g', 15);
}

QString kvLine(const QString& key, const QString& value) {
    return QStringLiteral("%1 = %2\n").arg(key, -11).arg(value);
}

// key = value with a trailing aligned comment ([sync] style in the spec).
QString kvLineC(const QString& key, const QString& value, const char* comment) {
    return QStringLiteral("%1 = %2 ; %3\n")
        .arg(key, -11).arg(value, -9).arg(QLatin1String(comment));
}

char roleLetter(Role r) {
    switch (r) {
        case Role::FromDeck: return 'a';
        case Role::ToDeck:   return 'b';
        case Role::Mixer:    return 'x';
    }
    return 'x';
}

bool roleFromLetter(const QString& s, Role& out) {
    if (s.size() != 1) return false;
    switch (s.at(0).toLower().unicode()) {
        case 'a': out = Role::FromDeck; return true;
        case 'b': out = Role::ToDeck;   return true;
        case 'x': out = Role::Mixer;    return true;
        default:  return false;
    }
}

bool triggerNeedsValue(ControlId id) {
    return id == ControlId::Cue ||
           (id >= ControlId::HotCue1 && id <= ControlId::HotCue8);
}

const char* curveName(Curve c) {
    switch (c) {
        case Curve::Step:   return "step";
        case Curve::Linear: return "linear";
        case Curve::SCurve: return "scurve";
    }
    return "step";
}

bool curveFromName(const QString& s, Curve& out) {
    const QString l = s.toLower();
    if (l == QLatin1String("step"))   { out = Curve::Step;   return true; }
    if (l == QLatin1String("linear")) { out = Curve::Linear; return true; }
    if (l == QLatin1String("scurve")) { out = Curve::SCurve; return true; }
    return false;
}

void warn(QStringList* warnings, const QString& msg) {
    if (warnings) warnings->append(msg);
}

// Section names we understand; anything else is preserved via extraMeta with
// a "<section>." key prefix (meta keys are stored unprefixed).
enum class Section {
    None, Meta, From, To, Sync, Initial, HotCues, Cues, Events, Unknown
};

void storeKnownKv(GvtFile& out, Section sec, const QString& secName,
                  const QString& key, const QString& value) {
    auto asDouble = [&value] { double d = 0.0; parseDouble(value, d); return d; };
    if (sec == Section::Meta) {
        if      (key == QLatin1String("name"))        out.name = value;
        else if (key == QLatin1String("author"))      out.author = value;
        else if (key == QLatin1String("created"))     out.created = value;
        else if (key == QLatin1String("description")) out.description = value;
        else out.extraMeta[key] = value;
        return;
    }
    if (sec == Section::From || sec == Section::To) {
        GvtTrackRef& t = (sec == Section::From) ? out.from : out.to;
        if      (key == QLatin1String("title"))       t.title = value;
        else if (key == QLatin1String("artist"))      t.artist = value;
        else if (key == QLatin1String("bpm"))         t.bpm = asDouble();
        else if (key == QLatin1String("duration"))    t.durationSec = asDouble();
        else if (key == QLatin1String("fingerprint")) t.fingerprint = value;
        else out.extraMeta[secName + QLatin1Char('.') + key] = value;
        return;
    }
    if (sec == Section::Sync) {
        if      (key == QLatin1String("anchor_from")) out.anchorFromBeat = asDouble();
        else if (key == QLatin1String("anchor_to"))   out.anchorToBeat = asDouble();
        else if (key == QLatin1String("master_bpm"))  out.masterBpm = asDouble();
        else out.extraMeta[secName + QLatin1Char('.') + key] = value;
        return;
    }
    if (sec == Section::Initial) {
        if (key == QLatin1String("complete")) {
            out.initialComplete = parseBool(value);
            return;
        }
        if (key == QLatin1String("crossfader")) {
            out.initialMixerCaptured = true;
            out.initialCrossfader = asDouble();
            return;
        }

        const bool toDeck = key.startsWith(QLatin1String("to_"));
        const QString deckKey = toDeck ? key.mid(3) : key;
        GvtInitialState& state = toDeck ? out.initialTo : out.initialFrom;
        bool known = true;
        if      (deckKey == QLatin1String("playing"))         state.playing = parseBool(value);
        else if (deckKey == QLatin1String("position_beat"))   state.positionBeat = asDouble();
        else if (deckKey == QLatin1String("cue_beat"))        state.cueBeat = asDouble();
        else if (deckKey == QLatin1String("tempo_ratio"))     state.tempoRatio = asDouble();
        else if (deckKey == QLatin1String("fader"))           state.fader = asDouble();
        else if (deckKey == QLatin1String("trim"))            state.trim = asDouble();
        else if (deckKey == QLatin1String("eq_low"))          state.eqLow = asDouble();
        else if (deckKey == QLatin1String("eq_mid"))          state.eqMid = asDouble();
        else if (deckKey == QLatin1String("eq_high"))         state.eqHigh = asDouble();
        else if (deckKey == QLatin1String("filter"))          state.filter = asDouble();
        else if (deckKey == QLatin1String("quantize")) {
            state.quantize = parseBool(value);
            state.quantizeCaptured = true;
        }
        else if (deckKey == QLatin1String("loop_active"))     state.loopActive = parseBool(value);
        else if (deckKey == QLatin1String("loop_start_beat")) state.loopStartBeat = asDouble();
        else if (deckKey == QLatin1String("loop_end_beat"))   state.loopEndBeat = asDouble();
        else if (deckKey == QLatin1String("fx_type"))         state.fxType = (int)std::lround(asDouble());
        else if (deckKey == QLatin1String("fx_on"))           state.fxOn = parseBool(value);
        else if (deckKey == QLatin1String("fx_wet"))          state.fxWet = asDouble();
        else if (deckKey == QLatin1String("fx_beats"))        state.fxBeats = asDouble();
        else if (deckKey == QLatin1String("stem_vocals"))     state.stemVocals = asDouble();
        else if (deckKey == QLatin1String("stem_melody"))     state.stemMelody = asDouble();
        else if (deckKey == QLatin1String("stem_bass"))       state.stemBass = asDouble();
        else if (deckKey == QLatin1String("stem_drums"))      state.stemDrums = asDouble();
        else known = false;

        if (known) state.captured = true;
        else out.extraMeta[secName + QLatin1Char('.') + key] = value;
        return;
    }
    if (sec == Section::HotCues) {
        static const QRegularExpression keyPattern(
            QStringLiteral("^([ab])([1-8])$"));
        const auto match = keyPattern.match(key.toLower());
        double beat = -1.0;
        if (match.hasMatch() && parseDouble(value, beat) &&
            std::isfinite(beat)) {
            const int pad = match.captured(2).toInt() - 1;
            auto& mappings = match.captured(1) == QLatin1String("a")
                                 ? out.fromHotCueBeats
                                 : out.toHotCueBeats;
            mappings[static_cast<std::size_t>(pad)] = beat;
        } else {
            out.extraMeta[secName + QLatin1Char('.') + key] = value;
        }
        return;
    }
    // Unknown section: preserve everything.
    out.extraMeta[secName + QLatin1Char('.') + key] = value;
}

bool parseEventLine(const QString& line, int lineNo, GvtFile& out,
                    QStringList* warnings) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList tok = line.split(ws, Qt::SkipEmptyParts);
    if (tok.size() < 3) {
        warn(warnings, QStringLiteral("line %1: malformed event (need beat target control), skipped").arg(lineNo));
        return false;
    }
    GvtEvent e;
    if (!parseDouble(tok[0], e.beat)) {
        warn(warnings, QStringLiteral("line %1: bad beat '%2', event skipped").arg(lineNo).arg(tok[0]));
        return false;
    }
    if (!roleFromLetter(tok[1], e.role)) {
        warn(warnings, QStringLiteral("line %1: bad target '%2' (want a|b|x), event skipped").arg(lineNo).arg(tok[1]));
        return false;
    }
    const QByteArray ctlName = tok[2].toUtf8();
    if (!controlFromName(ctlName.constData(), e.control)) {
        warn(warnings, QStringLiteral("line %1: unknown control '%2', event skipped").arg(lineNo).arg(tok[2]));
        return false;
    }
    int idx = 3;
    double v = 0.0;
    if (idx < tok.size() && parseDouble(tok[idx], v)) {
        e.value = v;
        ++idx;
    }
    if (idx < tok.size()) {
        if (!curveFromName(tok[idx], e.curve))
            warn(warnings, QStringLiteral("line %1: unknown curve '%2', treated as step").arg(lineNo).arg(tok[idx]));
        ++idx;
    }
    if (idx < tok.size())
        warn(warnings, QStringLiteral("line %1: trailing tokens ignored").arg(lineNo));
    out.events.push_back(e);
    return true;
}

bool parseCueLine(const QString& line, int lineNo, GvtFile& out,
                  QStringList* warnings) {
    const int eq = line.indexOf(QLatin1Char('='));
    if (eq < 0) {
        warn(warnings, QStringLiteral("line %1: malformed cue (want beat = label), skipped").arg(lineNo));
        return false;
    }
    GvtCue cue;
    if (!parseDouble(line.left(eq).trimmed(), cue.beat) ||
        !std::isfinite(cue.beat)) {
        warn(warnings, QStringLiteral("line %1: bad cue beat, skipped").arg(lineNo));
        return false;
    }
    cue.label = line.mid(eq + 1).trimmed();
    if (cue.label.isEmpty()) {
        warn(warnings, QStringLiteral("line %1: empty cue label, skipped").arg(lineNo));
        return false;
    }
    out.cues.push_back(std::move(cue));
    return true;
}

} // namespace

// ------------------------------------------------------------------ parse ---

bool gvtParse(const QString& text, GvtFile& out, QString* error,
              QStringList* warnings) {
    out = GvtFile{};

    const QStringList lines = text.split(QLatin1Char('\n'));
    bool sawMagic = false;
    Section sec = Section::None;
    QString secName;

    for (int i = 0; i < lines.size(); ++i) {
        const int lineNo = i + 1;
        const QString line = stripComment(lines[i]);
        if (line.isEmpty()) continue;

        if (!sawMagic) {
            static const QRegularExpression magicRe(
                QStringLiteral("^gravitino-transition\\s+(-?\\d+)$"));
            const auto m = magicRe.match(line);
            if (!m.hasMatch()) {
                if (error) *error = QStringLiteral(
                    "line %1: missing magic 'gravitino-transition <version>'").arg(lineNo);
                return false;
            }
            out.version = m.captured(1).toInt();
            sawMagic = true;
            continue;
        }

        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            secName = line.mid(1, line.size() - 2).trimmed().toLower();
            if      (secName == QLatin1String("meta"))   sec = Section::Meta;
            else if (secName == QLatin1String("from"))   sec = Section::From;
            else if (secName == QLatin1String("to"))     sec = Section::To;
            else if (secName == QLatin1String("sync"))   sec = Section::Sync;
            else if (secName == QLatin1String("initial")) sec = Section::Initial;
            else if (secName == QLatin1String("hotcues")) sec = Section::HotCues;
            else if (secName == QLatin1String("cues"))   sec = Section::Cues;
            else if (secName == QLatin1String("events")) sec = Section::Events;
            else {
                sec = Section::Unknown;
                warn(warnings, QStringLiteral("line %1: unknown section [%2], keys preserved").arg(lineNo).arg(secName));
            }
            continue;
        }

        if (sec == Section::Events) {
            parseEventLine(line, lineNo, out, warnings);
            continue;
        }
        if (sec == Section::Cues) {
            parseCueLine(line, lineNo, out, warnings);
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            warn(warnings, QStringLiteral("line %1: expected 'key = value', skipped").arg(lineNo));
            continue;
        }
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key.isEmpty()) {
            warn(warnings, QStringLiteral("line %1: empty key, skipped").arg(lineNo));
            continue;
        }
        if (sec == Section::None) {
            warn(warnings, QStringLiteral("line %1: key outside any section, skipped").arg(lineNo));
            continue;
        }
        storeKnownKv(out, sec, secName, key, value);
    }

    if (!sawMagic) {
        if (error) *error = QStringLiteral("empty file: missing magic 'gravitino-transition <version>'");
        return false;
    }

    std::stable_sort(out.events.begin(), out.events.end(),
                     [](const GvtEvent& a, const GvtEvent& b) { return a.beat < b.beat; });
    std::stable_sort(out.cues.begin(), out.cues.end(),
                     [](const GvtCue& a, const GvtCue& b) { return a.beat < b.beat; });
    return true;
}

// -------------------------------------------------------------- serialize ---

QString gvtSerialize(const GvtFile& f) {
    QString s;
    s += QStringLiteral("gravitino-transition %1\n\n").arg(f.version);

    // Split extraMeta by "<section>." prefix; unprefixed keys belong to [meta].
    std::map<QString, QString> metaX, fromX, toX, syncX, initialX, hotCuesX;
    std::map<QString, std::map<QString, QString>> otherX; // unknown sections
    for (const auto& [k, v] : f.extraMeta) {
        const int dot = k.indexOf(QLatin1Char('.'));
        if (dot < 0) { metaX[k] = v; continue; }
        const QString pre = k.left(dot), rest = k.mid(dot + 1);
        if      (pre == QLatin1String("from")) fromX[rest] = v;
        else if (pre == QLatin1String("to"))   toX[rest] = v;
        else if (pre == QLatin1String("sync")) syncX[rest] = v;
        else if (pre == QLatin1String("initial")) initialX[rest] = v;
        else if (pre == QLatin1String("hotcues")) hotCuesX[rest] = v;
        else otherX[pre][rest] = v;
    }

    s += QStringLiteral("[meta]\n");
    if (!f.name.isEmpty())        s += kvLine(QStringLiteral("name"), f.name);
    if (!f.author.isEmpty())      s += kvLine(QStringLiteral("author"), f.author);
    if (!f.created.isEmpty())     s += kvLine(QStringLiteral("created"), f.created);
    if (!f.description.isEmpty()) s += kvLine(QStringLiteral("description"), f.description);
    for (const auto& [k, v] : metaX) s += kvLine(k, v);
    s += QLatin1Char('\n');

    auto trackSection = [&](const char* name, const GvtTrackRef& t,
                            const std::map<QString, QString>& extra) {
        s += QStringLiteral("[%1]\n").arg(QLatin1String(name));
        if (!t.title.isEmpty())  s += kvLine(QStringLiteral("title"), t.title);
        if (!t.artist.isEmpty()) s += kvLine(QStringLiteral("artist"), t.artist);
        s += kvLine(QStringLiteral("bpm"), fmtNum(t.bpm, 2, 6));
        s += kvLine(QStringLiteral("duration"), fmtNum(t.durationSec, 2, 6));
        if (!t.fingerprint.isEmpty())
            s += kvLine(QStringLiteral("fingerprint"), t.fingerprint);
        for (const auto& [k, v] : extra) s += kvLine(k, v);
        s += QLatin1Char('\n');
    };
    trackSection("from", f.from, fromX);
    trackSection("to", f.to, toX);

    s += QStringLiteral("[sync]\n");
    s += kvLineC(QStringLiteral("anchor_from"), fmtNum(f.anchorFromBeat, 1, 6),
                 "beat in FROM track where the transition begins");
    s += kvLineC(QStringLiteral("anchor_to"), fmtNum(f.anchorToBeat, 1, 6),
                 "beat in TO track aligned to transition beat 0");
    s += kvLineC(QStringLiteral("master_bpm"), fmtNum(f.masterBpm, 2, 6),
                 "tempo the mix runs at during the transition");
    for (const auto& [k, v] : syncX) s += kvLine(k, v);
    s += QLatin1Char('\n');

    if (f.initialFrom.captured || f.initialTo.captured ||
        f.initialMixerCaptured || !initialX.empty()) {
        s += QStringLiteral("[initial]\n");
        if (f.initialComplete)
            s += kvLine(QStringLiteral("complete"), QStringLiteral("1"));
        if (f.initialMixerCaptured)
            s += kvLine(QStringLiteral("crossfader"),
                        fmtNum(f.initialCrossfader, 3, 6));
        const auto writeDeck = [&s](const GvtInitialState& state,
                                    const QString& prefix, bool complete) {
            const auto key = [&prefix](const char* name) {
                return prefix + QLatin1String(name);
            };
            s += kvLine(key("tempo_ratio"), fmtNum(state.tempoRatio, 3, 6));
            s += kvLine(key("fader"), fmtNum(state.fader, 3, 6));
            s += kvLine(key("trim"), fmtNum(state.trim, 3, 6));
            s += kvLine(key("eq_low"), fmtNum(state.eqLow, 3, 6));
            s += kvLine(key("eq_mid"), fmtNum(state.eqMid, 3, 6));
            s += kvLine(key("eq_high"), fmtNum(state.eqHigh, 3, 6));
            s += kvLine(key("filter"), fmtNum(state.filter, 3, 6));
            if (!complete) return;
            if (state.quantizeCaptured)
                s += kvLine(key("quantize"), state.quantize
                                                  ? QStringLiteral("1")
                                                  : QStringLiteral("0"));
            s += kvLine(key("playing"), state.playing ? QStringLiteral("1")
                                                       : QStringLiteral("0"));
            s += kvLine(key("position_beat"), fmtNum(state.positionBeat, 3, 6));
            s += kvLine(key("cue_beat"), fmtNum(state.cueBeat, 3, 6));
            s += kvLine(key("loop_active"), state.loopActive ? QStringLiteral("1")
                                                              : QStringLiteral("0"));
            s += kvLine(key("loop_start_beat"), fmtNum(state.loopStartBeat, 3, 6));
            s += kvLine(key("loop_end_beat"), fmtNum(state.loopEndBeat, 3, 6));
            s += kvLine(key("fx_type"), QString::number(state.fxType));
            s += kvLine(key("fx_on"), state.fxOn ? QStringLiteral("1")
                                                  : QStringLiteral("0"));
            s += kvLine(key("fx_wet"), fmtNum(state.fxWet, 3, 6));
            s += kvLine(key("fx_beats"), fmtNum(state.fxBeats, 3, 6));
            s += kvLine(key("stem_vocals"), fmtNum(state.stemVocals, 3, 6));
            s += kvLine(key("stem_melody"), fmtNum(state.stemMelody, 3, 6));
            s += kvLine(key("stem_bass"), fmtNum(state.stemBass, 3, 6));
            s += kvLine(key("stem_drums"), fmtNum(state.stemDrums, 3, 6));
        };
        if (f.initialFrom.captured) {
            writeDeck(f.initialFrom, QString(), f.initialComplete);
        }
        if (f.initialTo.captured)
            writeDeck(f.initialTo, QStringLiteral("to_"), f.initialComplete);
        for (const auto& [k, v] : initialX) s += kvLine(k, v);
        s += QLatin1Char('\n');
    }

    const auto hasHotCueMappings = [](const std::array<double, 8>& mappings) {
        return std::any_of(mappings.begin(), mappings.end(),
                           [](double beat) { return beat >= 0.0; });
    };
    if (hasHotCueMappings(f.fromHotCueBeats) ||
        hasHotCueMappings(f.toHotCueBeats) || !hotCuesX.empty()) {
        s += QStringLiteral("[hotcues]\n");
        s += QStringLiteral("; role+pad = track-relative beat\n");
        const auto writeMappings = [&s](char role,
                                        const std::array<double, 8>& mappings) {
            for (int pad = 0; pad < static_cast<int>(mappings.size()); ++pad) {
                if (!std::isfinite(mappings[static_cast<std::size_t>(pad)]) ||
                    mappings[static_cast<std::size_t>(pad)] < 0.0)
                    continue;
                s += kvLine(
                    QStringLiteral("%1%2").arg(QLatin1Char(role)).arg(pad + 1),
                    fmtNum(mappings[static_cast<std::size_t>(pad)], 3, 9));
            }
        };
        writeMappings('a', f.fromHotCueBeats);
        writeMappings('b', f.toHotCueBeats);
        for (const auto& [k, v] : hotCuesX) s += kvLine(k, v);
        s += QLatin1Char('\n');
    }

    for (const auto& [name, kvs] : otherX) {
        s += QStringLiteral("[%1]\n").arg(name);
        for (const auto& [k, v] : kvs) s += kvLine(k, v);
        s += QLatin1Char('\n');
    }

    if (!f.cues.empty()) {
        s += QStringLiteral("[cues]\n");
        s += QStringLiteral("; beat = label\n");
        std::vector<GvtCue> cues = f.cues;
        std::stable_sort(cues.begin(), cues.end(),
                         [](const GvtCue& a, const GvtCue& b) { return a.beat < b.beat; });
        for (const GvtCue& cue : cues) {
            QString label = cue.label;
            label.replace(QLatin1Char('\n'), QLatin1Char(' '));
            label.replace(QLatin1Char('\r'), QLatin1Char(' '));
            s += QStringLiteral("%1 = %2\n")
                     .arg(fmtNum(cue.beat, 3, 9), -11)
                     .arg(label.trimmed());
        }
        s += QLatin1Char('\n');
    }

    s += QStringLiteral("[events]\n");
    s += QStringLiteral("; beat | target | control | value | curve\n");

    std::vector<GvtEvent> evs = f.events;
    std::stable_sort(evs.begin(), evs.end(),
                     [](const GvtEvent& a, const GvtEvent& b) { return a.beat < b.beat; });
    for (const GvtEvent& e : evs) {
        QString line = QStringLiteral("%1 %2 %3")
            .arg(fmtNum(e.beat, 3, 9), -8)
            .arg(QChar::fromLatin1(roleLetter(e.role)), -8)
            .arg(QLatin1String(controlName(e.control)), -11);
        if (!controlIsTrigger(e.control) || triggerNeedsValue(e.control)) {
            line += QStringLiteral(" %1").arg(fmtNum(e.value, 2, 6), -6);
            if (!controlIsTrigger(e.control) && e.curve != Curve::Step)
                line += QStringLiteral(" %1").arg(QLatin1String(curveName(e.curve)));
        }
        // Trim right padding.
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        s += line + QLatin1Char('\n');
    }
    return s;
}

// ------------------------------------------------------------------- file ---

bool gvtLoadFile(const QString& path, GvtFile& out, QString* error,
                 QStringList* warnings) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    const QString text = QString::fromUtf8(file.readAll());
    if (!gvtParse(text, out, error, warnings)) return false;
    out.filePath = path;
    return true;
}

bool gvtSaveFile(const GvtFile& f, const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = gvtSerialize(f).toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (error) *error = QStringLiteral("short write to %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace gvt
