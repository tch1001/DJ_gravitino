#include "History.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>

namespace gvt {

// JSON-lines format, one object per line, e.g.:
// {"startedAt":"2026-08-15T21:04:33","deck":0,"title":"Demo Track 1",
//  "artist":"PioneerDJ","bpm":128.0,"key":"7B"}
// Unknown keys are ignored on load; malformed lines are skipped.

static constexpr int kMaxLoaded = 500;

History::History(QObject* parent) : QObject(parent)
{
    const QString dir = QDir::homePath() + QStringLiteral("/.gravitino");
    QDir().mkpath(dir);
    filePath_ = dir + QStringLiteral("/history.jsonl");
    loadFromDisk();
}

void History::loadFromDisk()
{
    QFile f(filePath_);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return; // no history yet

    // File order is oldest→newest (append-only); collect all valid lines,
    // keep the last kMaxLoaded, expose newest-first.
    std::vector<Entry> loaded;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonParseError err;
        const QJsonDocument doc =
            QJsonDocument::fromJson(line.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        Entry e;
        e.startedAt = QDateTime::fromString(
            o.value(QStringLiteral("startedAt")).toString(), Qt::ISODate);
        e.title = o.value(QStringLiteral("title")).toString();
        e.artist = o.value(QStringLiteral("artist")).toString();
        e.bpm = o.value(QStringLiteral("bpm")).toDouble();
        e.key = o.value(QStringLiteral("key")).toString();
        e.deck = o.value(QStringLiteral("deck")).toInt();
        loaded.push_back(std::move(e));
    }
    if ((int)loaded.size() > kMaxLoaded)
        loaded.erase(loaded.begin(),
                     loaded.end() - kMaxLoaded);
    entries_.assign(loaded.rbegin(), loaded.rend()); // newest first
}

void History::logLoad(int deck, const TrackData& t)
{
    Entry e;
    e.startedAt = QDateTime::currentDateTime();
    e.title = t.title.isEmpty() ? t.filePath : t.title;
    e.artist = t.artist;
    e.bpm = t.bpm;
    e.key = t.camelotKey;
    e.deck = deck;

    QJsonObject o;
    o.insert(QStringLiteral("startedAt"),
             e.startedAt.toString(Qt::ISODate));
    o.insert(QStringLiteral("deck"), e.deck);
    o.insert(QStringLiteral("title"), e.title);
    o.insert(QStringLiteral("artist"), e.artist);
    o.insert(QStringLiteral("bpm"), e.bpm);
    o.insert(QStringLiteral("key"), e.key);

    QFile f(filePath_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append |
               QIODevice::Text)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.write("\n");
    } else {
        qWarning("gvt::History: cannot append to %s",
                 qPrintable(filePath_));
    }

    entries_.insert(entries_.begin(), std::move(e)); // newest first
    emit entryAdded();
}

} // namespace gvt
