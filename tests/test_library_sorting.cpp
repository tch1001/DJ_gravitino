// Library sort-role regression: displayed BPM/key/duration text must sort by
// musical/numeric value rather than lexicographic formatting.

#include "library/TrackLibrary.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTimer>

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

bool writeSilentWav(const QString& path)
{
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 channels = 2;
    constexpr quint16 bits = 16;
    constexpr quint32 frames = sampleRate * 2;
    constexpr quint32 dataBytes = frames * channels * (bits / 8);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    file.write("RIFF", 4);
    out << quint32(36 + dataBytes);
    file.write("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << channels << sampleRate;
    out << quint32(sampleRate * channels * (bits / 8));
    out << quint16(channels * (bits / 8)) << bits;
    file.write("data", 4);
    out << dataBytes;
    const QByteArray silence(static_cast<qsizetype>(dataBytes), '\0');
    return file.write(silence) == silence.size();
}

bool scanAndWait(gvt::TrackLibrary& library, const QString& directory,
                 int expected)
{
    int ready = 0;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&library, &gvt::TrackLibrary::trackReady, &loop,
                     [&](int) {
                         if (++ready >= expected) loop.quit();
                     });
    timeout.start(15000);
    library.scanFolder(directory);
    loop.exec();
    return ready == expected;
}

QStringList orderedTitles(QSortFilterProxyModel& proxy, int column)
{
    proxy.sort(column, Qt::AscendingOrder);
    QStringList titles;
    for (int row = 0; row < proxy.rowCount(); ++row)
        titles.append(proxy.index(row, 0).data(Qt::DisplayRole).toString());
    return titles;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (!temporary.isValid()) return 1;
    const QString music = temporary.filePath(QStringLiteral("music"));
    CHECK(QDir().mkpath(music));
    qputenv("GRAVITINO_CACHE_DIR",
            temporary.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("GRAVITINO_CATALOG_PATH",
            temporary.filePath(QStringLiteral("catalog.json")).toUtf8());
    for (const QString& name : {QStringLiteral("a.mp3"),
                                QStringLiteral("b.mp3"),
                                QStringLiteral("c.mp3")})
        CHECK(writeSilentWav(music + QLatin1Char('/') + name));

    gvt::TrackLibrary library;
    CHECK(scanAndWait(library, music, 3));
    CHECK(library.trackCount() == 3);
    const double bpms[] = {2.0, 10.0, 100.0};
    const double durations[] = {600.0, 9.0, 65.0};
    const QString keys[] = {QStringLiteral("1A"), QStringLiteral("10A"),
                            QStringLiteral("2A")};
    const QString titles[] = {QStringLiteral("low"), QStringLiteral("middle"),
                              QStringLiteral("high")};
    for (int row = 0; row < 3; ++row) {
        const gvt::TrackDataPtr track = library.trackAt(row);
        CHECK(track != nullptr);
        if (!track) continue;
        track->bpm = bpms[row];
        track->durationSec = durations[row];
        track->camelotKey = keys[row];
        track->title = titles[row];
    }

    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&library);
    proxy.setSortRole(Qt::UserRole);
    CHECK(orderedTitles(proxy, 2) ==
          QStringList({QStringLiteral("low"), QStringLiteral("middle"),
                       QStringLiteral("high")}));
    CHECK(orderedTitles(proxy, 3) ==
          QStringList({QStringLiteral("low"), QStringLiteral("high"),
                       QStringLiteral("middle")}));
    CHECK(orderedTitles(proxy, 4) ==
          QStringList({QStringLiteral("middle"), QStringLiteral("high"),
                       QStringLiteral("low")}));

    if (failures) return 1;
    std::printf("test_library_sorting: BPM, Camelot key, and duration sort naturally\n");
    return 0;
}
