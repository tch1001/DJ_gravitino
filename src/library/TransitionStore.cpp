// TransitionStore — manages portable .transition files plus legacy .gvt.
// Owned by claude-analysis. Uses the format-neutral transition file API from
// src/transitions (implemented by the transitions agent); implements
// gvt::matchTrack() here per docs/STATUS.md ownership.

#include "TrackLibrary.h"
#include "../analysis/TrackData.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gvt {

// ---- Track matching tiers (see docs/TRANSITION_FORMAT.md) -------------------

MatchQuality matchTrack(const GvtTrackRef& ref, const TrackData& t)
{
    if (!ref.fingerprint.isEmpty() && ref.fingerprint == t.fingerprint)
        return MatchQuality::Fingerprint;

    for (const TransitionFingerprint& fingerprint : ref.fingerprints) {
        if (fingerprint.algorithm == QLatin1String("gvfp1") &&
            (fingerprint.value == t.fingerprint ||
             QStringLiteral("gvfp1:") + fingerprint.value == t.fingerprint))
            return MatchQuality::Fingerprint;
        if ((fingerprint.algorithm == QLatin1String("gravitino-structure-1") ||
             fingerprint.algorithm == QLatin1String("gravitino-structure-2")) &&
            structureFingerprintSimilarity(fingerprint.value,
                                           t.structureFingerprint) >= 0.88) {
            const double expectedDuration = ref.durationSec;
            const double actualDuration = t.audibleDurationSec > 0.0
                                              ? t.audibleDurationSec
                                              : t.durationSec;
            const bool durationCompatible = expectedDuration <= 0.0 ||
                std::fabs(expectedDuration - actualDuration) <=
                    std::max(2.0, expectedDuration * 0.015);
            const bool bpmCompatible = ref.bpm <= 0.0 || t.bpm <= 0.0 ||
                std::fabs(ref.bpm - t.bpm) <= std::max(0.35, ref.bpm * 0.01);
            if (durationCompatible && bpmCompatible)
                return MatchQuality::Structure;
        }
    }

    const bool sameIsrc = !ref.isrc.isEmpty() && !t.isrc.isEmpty() &&
        ref.isrc.compare(t.isrc, Qt::CaseInsensitive) == 0;
    const bool sameMusicBrainz = !ref.musicBrainzRecording.isEmpty() &&
        !t.musicBrainzRecording.isEmpty() &&
        ref.musicBrainzRecording.compare(t.musicBrainzRecording,
                                          Qt::CaseInsensitive) == 0;
    if (sameIsrc || sameMusicBrainz) {
        const double actualDuration = t.audibleDurationSec > 0.0
                                          ? t.audibleDurationSec
                                          : t.durationSec;
        const double actualBeats = t.bpm > 0.0
                                       ? actualDuration * t.bpm / 60.0 : 0.0;
        const bool bpmCompatible = ref.bpm <= 0.0 || t.bpm <= 0.0 ||
            std::fabs(ref.bpm - t.bpm) <= std::max(0.35, ref.bpm * 0.01);
        const bool durationCompatible = ref.durationSec <= 0.0 ||
            actualDuration <= 0.0 ||
            std::fabs(ref.durationSec - actualDuration) <=
                std::max(2.0, ref.durationSec * 0.015);
        const bool beatsCompatible = ref.durationBeats <= 0.0 ||
            actualBeats <= 0.0 ||
            std::fabs(ref.durationBeats - actualBeats) <=
                std::max(4.0, ref.durationBeats * 0.015);
        if (bpmCompatible && durationCompatible && beatsCompatible)
            return MatchQuality::Identifier;
    }

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

bool transitionTrackMatchReliable(const GvtFile& file,
                                  const GvtTrackRef& ref,
                                  const TrackData& track)
{
    const MatchQuality quality = matchTrack(ref, track);
    if (file.sourceFormat == TransitionSourceFormat::PortableYaml)
        return quality == MatchQuality::Fingerprint ||
               quality == MatchQuality::Structure ||
               quality == MatchQuality::Identifier;
    return isReliableTrackMatch(quality);
}

// ---- Store ------------------------------------------------------------------

struct TransitionStore::Impl {
    QString dir;
    std::vector<GvtFile> files;
    SongCatalog* catalog = nullptr;
};

namespace {

QString displayIdentity(const GvtFile& file)
{
    return file.legacySourceId.isEmpty() ? file.id : file.legacySourceId;
}

GvtFile migratedPortableCopy(const GvtFile& source)
{
    GvtFile portable = source;
    portable.legacySourceId = source.legacySourceId.isEmpty()
                                  ? source.id : source.legacySourceId;
    portable.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    portable.sourceFormat = TransitionSourceFormat::PortableYaml;
    QJsonObject legacy = portable.extensions
        .value(QStringLiteral("gravitino.legacy")).toObject();
    legacy.insert(QStringLiteral("source_id"), portable.legacySourceId);
    portable.extensions.insert(QStringLiteral("gravitino.legacy"), legacy);
    migrateSavedLoopsFromInitialState(portable);
    return portable;
}

} // namespace

TransitionStore::TransitionStore(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->dir = qEnvironmentVariable("GRAVITINO_TRANSITIONS_DIR");
    if (impl_->dir.isEmpty())
        impl_->dir = QDir::homePath() +
                     QStringLiteral("/Music/Gravitino/Transitions");
    impl_->dir = QFileInfo(impl_->dir).absoluteFilePath();
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

void TransitionStore::setSongCatalog(SongCatalog* catalog)
{
    impl_->catalog = catalog;
}

bool TransitionStore::matchesEndpoint(const GvtFile& file, bool outgoing,
                                      const TrackData& track) const
{
    if (impl_->catalog && !track.songId.isEmpty()) {
        QString bound = impl_->catalog->songIdForEndpoint(file.id, outgoing);
        if (bound.isEmpty() && !file.legacySourceId.isEmpty())
            bound = impl_->catalog->songIdForEndpoint(
                file.legacySourceId, outgoing);
        if (!bound.isEmpty()) return bound == track.songId;
    }
    const GvtTrackRef& ref = outgoing ? file.from : file.to;
    return transitionTrackMatchReliable(file, ref, track);
}

void TransitionStore::reload()
{
    impl_->files.clear();
    const QDir d(impl_->dir);
    for (const QFileInfo& fi :
         d.entryInfoList({QStringLiteral("*.transition"), QStringLiteral("*.gvt")},
                         QDir::Files | QDir::Readable, QDir::Name)) {
        GvtFile f;
        QString error;
        QStringList warnings;
        if (loadTransitionFile(fi.absoluteFilePath(), f, &error, &warnings)) {
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
        // Duration alone is far too collision-prone to authorize Perform,
        // Tutorial, auto-load, or hot-cue expectations for another song.
        if (!matchesEndpoint(f, true, from) ||
            !matchesEndpoint(f, false, to)) continue;
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
    const QByteArray bytes = (f.id + QLatin1Char('|') + f.name + QLatin1Char('|') + f.from.fingerprint +
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

QString pathFor(const QString& dir, const GvtFile& f)
{
    return dir + QLatin1Char('/') + sanitizeName(f.name) + QLatin1Char('-') +
           hash4(f) + QStringLiteral(".transition");
}

bool isManagedPath(const QString& dir, const QString& path)
{
    if (path.isEmpty()) return false;
    const QFileInfo fi(path);
    const QString suffix = fi.suffix().toLower();
    return !fi.isSymLink() &&
           (suffix == QLatin1String("gvt") ||
            suffix == QLatin1String("transition")) &&
           QDir::cleanPath(fi.absolutePath()) ==
               QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
}

} // namespace

int TransitionStore::convertAllLegacy(QStringList* convertedPaths,
                                      QStringList* errors)
{
    QDir().mkpath(impl_->dir);
    QSet<QString> alreadyConverted;
    for (const GvtFile& file : impl_->files) {
        if (file.sourceFormat == TransitionSourceFormat::PortableYaml &&
            !file.legacySourceId.isEmpty())
            alreadyConverted.insert(file.legacySourceId);
    }

    int converted = 0;
    const std::vector<GvtFile> snapshot = impl_->files;
    for (const GvtFile& source : snapshot) {
        if (source.sourceFormat != TransitionSourceFormat::LegacyGvt)
            continue;
        const QString sourceId = displayIdentity(source);
        if (alreadyConverted.contains(sourceId)) continue;

        GvtFile portable = migratedPortableCopy(source);
        const QString path = pathFor(impl_->dir, portable);
        QString error;
        if (QFileInfo::exists(path)) {
            error = QStringLiteral("destination already exists: %1").arg(path);
        } else if (!saveTransitionFile(portable, path, &error)) {
            error = QStringLiteral("%1: %2").arg(source.filePath, error);
        }
        if (!error.isEmpty()) {
            if (errors) errors->append(error);
            continue;
        }
        alreadyConverted.insert(sourceId);
        if (convertedPaths) convertedPaths->append(path);
        ++converted;
    }
    if (converted > 0) reload();
    return converted;
}

int TransitionStore::upgradePortableSavedLoops(QStringList* upgradedPaths,
                                                QStringList* errors)
{
    int upgraded = 0;
    const std::vector<GvtFile> snapshot = impl_->files;
    for (GvtFile file : snapshot) {
        if (file.sourceFormat != TransitionSourceFormat::PortableYaml)
            continue;
        QStringList warnings;
        const int loopCount = migrateSavedLoopsFromInitialState(file, &warnings);
        if (loopCount == 0) {
            for (const QString& warning : warnings)
                if (errors) errors->append(
                    QStringLiteral("%1: %2").arg(file.filePath, warning));
            continue;
        }
        QString error;
        if (!transitionSaveFile(file, file.filePath, &error)) {
            if (errors) errors->append(
                QStringLiteral("%1: %2").arg(file.filePath, error));
            continue;
        }
        if (upgradedPaths) upgradedPaths->append(file.filePath);
        for (const QString& warning : warnings)
            if (errors) errors->append(
                QStringLiteral("%1: %2").arg(file.filePath, warning));
        ++upgraded;
    }
    if (upgraded > 0) reload();
    return upgraded;
}

QString TransitionStore::save(GvtFile& f, QString* error)
{
    QDir().mkpath(impl_->dir);
    if (f.id.isEmpty())
        f.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = pathFor(impl_->dir, f);
    if (!saveTransitionFile(f, path, error))
        return {};
    f.filePath = path;
    f.sourceFormat = TransitionSourceFormat::PortableYaml;
    reload();
    return path;
}

bool TransitionStore::update(const GvtFile& f, QString* error)
{
    if (!isManagedPath(impl_->dir, f.filePath) || !QFileInfo::exists(f.filePath)) {
        if (error) *error = QStringLiteral("transition is not a managed file");
        return false;
    }
    const bool legacy = QFileInfo(f.filePath).suffix().compare(
        QStringLiteral("gvt"), Qt::CaseInsensitive) == 0;
    const GvtFile output = legacy ? migratedPortableCopy(f) : f;
    const QString path = legacy ? pathFor(impl_->dir, output) : f.filePath;
    if (!saveTransitionFile(output, path, error))
        return false;
    reload();
    return true;
}

QString TransitionStore::renameTransition(const GvtFile& f,
                                          const QString& newName,
                                          QString* error)
{
    if (!isManagedPath(impl_->dir, f.filePath) || !QFileInfo::exists(f.filePath)) {
        if (error) *error = QStringLiteral("transition is not a managed file");
        return {};
    }
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty()) {
        if (error) *error = QStringLiteral("transition name cannot be empty");
        return {};
    }

    const bool legacy = QFileInfo(f.filePath).suffix().compare(
        QStringLiteral("gvt"), Qt::CaseInsensitive) == 0;
    GvtFile renamed = legacy ? migratedPortableCopy(f) : f;
    renamed.name = trimmed;
    const QString oldPath = f.filePath;
    const QString newPath = pathFor(impl_->dir, renamed);
    if (newPath != oldPath && QFileInfo::exists(newPath)) {
        if (error) *error = QStringLiteral("a transition with that name already exists");
        return {};
    }
    if (!saveTransitionFile(renamed, newPath, error))
        return {};
    if (!legacy && newPath != oldPath && !QFile::remove(oldPath)) {
        QFile::remove(newPath); // roll back the newly written copy
        if (error) *error = QStringLiteral("could not remove the old transition file");
        return {};
    }
    reload();
    return newPath;
}

bool TransitionStore::deleteTransition(const GvtFile& f, QString* error)
{
    if (!isManagedPath(impl_->dir, f.filePath) || !QFileInfo::exists(f.filePath)) {
        if (error) *error = QStringLiteral("transition is not a managed file");
        return false;
    }
    if (!QFile::remove(f.filePath)) {
        if (error) *error = QStringLiteral("could not delete %1").arg(f.filePath);
        return false;
    }
    reload();
    return true;
}

} // namespace gvt
