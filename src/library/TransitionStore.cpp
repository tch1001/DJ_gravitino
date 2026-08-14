// TransitionStore — manages ~/Music/Gravitino/Transitions/*.gvt.
// Owned by claude-analysis. Uses gvtLoadFile/gvtSaveFile from
// src/transitions (implemented by the transitions agent); implements
// gvt::matchTrack() here per docs/STATUS.md ownership.

#include "TrackLibrary.h"
#include "../analysis/TrackData.h"

#include <QDir>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gvt {

// ---- Track matching tiers (see docs/TRANSITION_FORMAT.md) -------------------

MatchQuality matchTrack(const GvtTrackRef& ref, const TrackData& t)
{
    if (!ref.fingerprint.isEmpty() && ref.fingerprint == t.fingerprint)
        return MatchQuality::Fingerprint;

    const QString refTitle  = ref.title.trimmed().toCaseFolded();
    const QString refArtist = ref.artist.trimmed().toCaseFolded();
    if (!refTitle.isEmpty() &&
        refTitle == t.title.trimmed().toCaseFolded() &&
        refArtist == t.artist.trimmed().toCaseFolded())
        return MatchQuality::TitleArtist;

    if (ref.durationSec > 0.0 && std::fabs(ref.durationSec - t.durationSec) <= 1.5)
        return MatchQuality::DurationOnly;

    return MatchQuality::None;
}

// ---- Store ------------------------------------------------------------------

struct TransitionStore::Impl {
    QString dir;
    std::vector<GvtFile> files;
};

TransitionStore::TransitionStore(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->dir = QDir::homePath() + QStringLiteral("/Music/Gravitino/Transitions");
    QDir().mkpath(impl_->dir);
    reload();
}

TransitionStore::~TransitionStore() = default;

QString TransitionStore::directory() const
{
    return impl_->dir;
}

const std::vector<GvtFile>& TransitionStore::all() const
{
    return impl_->files;
}

void TransitionStore::reload()
{
    impl_->files.clear();
    const QDir d(impl_->dir);
    for (const QFileInfo& fi :
         d.entryInfoList({QStringLiteral("*.gvt")}, QDir::Files | QDir::Readable, QDir::Name)) {
        GvtFile f;
        QString error;
        QStringList warnings;
        if (gvtLoadFile(fi.absoluteFilePath(), f, &error, &warnings)) {
            f.filePath = fi.absoluteFilePath();
            impl_->files.push_back(std::move(f));
        } else {
            qWarning("TransitionStore: skipping %s: %s",
                     qUtf8Printable(fi.absoluteFilePath()), qUtf8Printable(error));
        }
        for (const QString& w : warnings)
            qWarning("TransitionStore: %s: %s",
                     qUtf8Printable(fi.absoluteFilePath()), qUtf8Printable(w));
    }
    emit changed();
}

std::vector<const GvtFile*>
TransitionStore::matching(const TrackData& from, const TrackData& to) const
{
    struct Scored { const GvtFile* f; int score; };
    std::vector<Scored> scored;
    for (const GvtFile& f : impl_->files) {
        const MatchQuality qf = matchTrack(f.from, from);
        const MatchQuality qt = matchTrack(f.to, to);
        if (qf == MatchQuality::None || qt == MatchQuality::None) continue;
        const int pair = (int)std::min(qf, qt);          // tier = weaker side
        scored.push_back({&f, pair * 16 + (int)qf + (int)qt});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored& a, const Scored& b) { return a.score > b.score; });
    std::vector<const GvtFile*> out;
    out.reserve(scored.size());
    for (const Scored& s : scored) out.push_back(s.f);
    return out;
}

namespace {

QString sanitizeName(const QString& name)
{
    QString out;
    bool lastDash = true; // suppress leading dash
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) {
            out += c;
            lastDash = false;
        } else if (!lastDash) {
            out += QLatin1Char('-');
            lastDash = true;
        }
    }
    while (out.endsWith(QLatin1Char('-'))) out.chop(1);
    if (out.size() > 60) out.truncate(60);
    if (out.isEmpty()) out = QStringLiteral("transition");
    return out;
}

QString hash4(const GvtFile& f)
{
    // FNV-1a over identifying fields, folded to 16 bits, as 4 hex chars.
    uint64_t h = 14695981039346656037ULL;
    const QByteArray bytes = (f.name + QLatin1Char('|') + f.from.fingerprint +
                              QLatin1Char('|') + f.to.fingerprint + QLatin1Char('|') +
                              f.from.title + QLatin1Char('|') + f.to.title)
                                 .toUtf8();
    for (const char c : bytes) {
        h ^= (uint8_t)c;
        h *= 1099511628211ULL;
    }
    const uint16_t folded = (uint16_t)(h ^ (h >> 16) ^ (h >> 32) ^ (h >> 48));
    return QStringLiteral("%1").arg(folded, 4, 16, QLatin1Char('0'));
}

} // namespace

QString TransitionStore::save(GvtFile& f, QString* error)
{
    QDir().mkpath(impl_->dir);
    const QString path = impl_->dir + QLatin1Char('/') + sanitizeName(f.name) +
                         QLatin1Char('-') + hash4(f) + QStringLiteral(".gvt");
    if (!gvtSaveFile(f, path, error))
        return {};
    f.filePath = path;
    reload();
    return path;
}

} // namespace gvt
