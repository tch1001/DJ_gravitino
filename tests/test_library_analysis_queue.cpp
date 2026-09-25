// Protects interactive analysis priority, truthful progress, and safe deferred
// deck loads using gated workers and isolated audio/cache fixtures.
#include "library/TrackLibrary.h"
#include "library/LibraryAnalysisInternal.h"
#include "ui/LibraryWidget.h"
#include "ui/Theme.h"
#include "control/ControlBus.h"
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <atomic>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; \
} } while (0)

bool until(const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

QString fixtures(const QString& root, const QString& folder, int begin, int end)
{
    const QString dir = root + '/' + folder;
    CHECK(QDir().mkpath(dir));
    for (int i = begin; i < end; ++i) {
        QFile file(dir + QStringLiteral("/%1.wav").arg(i, 2, 10, QLatin1Char('0')));
        CHECK(file.open(QIODevice::WriteOnly));
        file.write("synthetic worker fixture");
    }
    return dir;
}

struct Workers {
    std::array<std::atomic<int>, 32> calls{};
    std::array<std::atomic<bool>, 32> released{};
    std::array<std::atomic<bool>, 32> fail{};
    std::array<std::atomic<double>, 32> fraction{};
    gvt::detail::LibraryAnalyzer analyzer()
    {
        return [this](const QString& path, QString* error,
                      const gvt::detail::AnalysisProgress& progress) {
            const int n = QFileInfo(path).baseName().toInt();
            ++calls[n];
            while (!released[n]) {
                progress(fraction[n], QStringLiteral("Waveform"));
                QThread::msleep(1);
            }
            if (fail[n]) {
                *error = QStringLiteral("Broken test audio");
                return gvt::TrackDataPtr{};
            }
            auto track = std::make_shared<gvt::TrackData>();
            track->filePath = path;
            track->title = QStringLiteral("Track %1").arg(n);
            track->pcm.resize(128, .1f);
            track->bpm = 120;
            track->fingerprint = QStringLiteral("gvfp1:queue-%1").arg(n);
            progress(.99, QStringLiteral("Finishing"));
            return track;
        };
    }
    void releaseAll() { for (auto& release : released) release = true; }
};

void load_requests_skip_background_work_and_do_not_duplicate(const QString& root)
{
    Workers work;
    gvt::TrackLibrary library;
    gvt::detail::setLibraryAnalyzerForTesting(library, work.analyzer());
    library.scanFolder(fixtures(root, "priority", 0, 8));
    CHECK(until([&] { return work.calls[0] && work.calls[1] && work.calls[2]; }));
    CHECK(!work.calls[3]);
    CHECK(library.index(7, 5).data().toString().contains("Queued"));
    CHECK(library.index(7, 5).data(gvt::TrackLibrary::AnalysisProgressRole).toDouble() == 0);
    CHECK(!library.prioritizeAnalysis(-1));
    CHECK(!library.prioritizeAnalysis(99));
    CHECK(library.prioritizeAnalysis(7));
    CHECK(until([&] { return work.calls[7] == 1; }));
    CHECK(!work.calls[3]); // fourth slot really is available to the clicked song
    CHECK(library.prioritizeAnalysis(7));
    CHECK(work.calls[7] == 1);
    work.fraction[7] = .47;
    CHECK(until([&] { return library.index(7, 5).data(
        gvt::TrackLibrary::AnalysisProgressRole).toDouble() == .47; }));
    work.fraction[7] = .1; // regressing reports cannot make the UI go backwards
    QElapsedTimer throttle;
    throttle.start();
    CHECK(until([&] { return throttle.elapsed() > 150; }));
    CHECK(library.index(7, 5).data(gvt::TrackLibrary::AnalysisProgressRole).toDouble() == .47);
    CHECK(library.prioritizeAnalysis(5));
    CHECK(library.prioritizeAnalysis(6));
    CHECK(library.prioritizeAnalysis(6));
    work.released[7] = true;
    CHECK(until([&] { return work.calls[6] == 1; }));
    CHECK(!work.calls[5]);
    CHECK(library.trackAt(7));
    CHECK(library.index(7, 5).data(gvt::TrackLibrary::AnalysisProgressRole).toDouble() == 1);
    work.released[6] = true;
    CHECK(until([&] { return work.calls[5] == 1; }));
    work.releaseAll();
    CHECK(until([&] {
        for (int i = 0; i < 8; ++i) if (!library.trackAt(i)) return false;
        return true;
    }));
    for (int i = 0; i < 8; ++i) CHECK(work.calls[i] == 1);
}

void transition_click_prioritizes_both_songs_and_waits_for_verified_audio(const QString& root)
{
    using namespace gvt;
    Workers work;
    ControlBus bus; AudioEngine engine(&bus);
    TrackLibrary library; TransitionStore transitions;
    const auto dir = fixtures(root, "transition-priority", 0, 9);
    for (int n : {7, 8}) {
        TrackData profile;
        profile.filePath = dir + QStringLiteral("/%1.wav").arg(n, 2, 10, QLatin1Char('0'));
        profile.title = QStringLiteral("Track %1").arg(n);
        profile.fingerprint = QStringLiteral("gvfp1:queue-%1").arg(n);
        profile.bpm = 120; profile.durationSec = 16;
        CHECK(!library.songCatalog()->registerAsset(profile).isEmpty());
    }
    detail::setLibraryAnalyzerForTesting(library, work.analyzer());
    library.scanFolder(dir);
    GvtFile file;
    file.name = "Priority both sides"; file.masterBpm = 120; file.endBeat = 8;
    file.from.title = "Track 7"; file.from.fingerprint = "gvfp1:queue-7";
    file.to.title = "Track 8"; file.to.fingerprint = "gvfp1:queue-8";
    file.from.bpm = file.to.bpm = 120; file.from.durationSec = file.to.durationSec = 16;
    QString error; CHECK(!transitions.save(file, &error).isEmpty());
    LibraryWidget widget(&library, &engine, &transitions);
    auto* tab = widget.findChild<QPushButton*>("transitionLibraryTab");
    auto* table = widget.findChild<QTableView*>("transitionLibraryTable");
    CHECK(tab && table);
    if (!tab || !table) { work.releaseAll(); return; }
    tab->click();
    int selected = 0;
    QObject::connect(&widget, &LibraryWidget::transitionSelected, &widget,
        [&](const QString& path) { if (!path.isEmpty()) ++selected; });
    const auto click = [&] {
        CHECK(QMetaObject::invokeMethod(&widget, "onTransitionClicked", Qt::DirectConnection,
            Q_ARG(QModelIndex, table->model()->index(0, 0))));
    };
    CHECK(table->model()->headerData(7, Qt::Horizontal).toString() == "Status");
    CHECK(table->model()->index(0, 7).data().toString().contains("Queued"));
    click();
    CHECK(until([&] { return work.calls[7] == 1; }));
    CHECK(!engine.deck(0).track() && !engine.deck(1).track() && selected == 0);
    CHECK(table->model()->index(0, 7).data().toString().contains("priority"));
    work.released[7] = true;
    CHECK(until([&] { return work.calls[8] == 1; }));
    CHECK(!engine.deck(0).track() && !engine.deck(1).track());
    work.released[8] = true;
    CHECK(until([&] { return selected == 1; }));
    CHECK(engine.deck(0).track() == library.trackAt(7));
    CHECK(engine.deck(1).track() == library.trackAt(8));
    CHECK(table->model()->index(0, 7).data().toString() == "Ready (2/2 songs)");
    work.releaseAll();
    CHECK(until([&] { return bool(library.trackAt(6)); }));
    QString removeError; CHECK(transitions.deleteTransition(file, &removeError));
}

void request_import_keeps_existing_tracks_and_workers(const QString& root)
{
    Workers work;
    gvt::TrackLibrary library;
    gvt::detail::setLibraryAnalyzerForTesting(library,work.analyzer());
    const auto old=fixtures(root,"import-old",0,4);
    library.scanFolder(old);
    CHECK(until([&]{return work.calls[0] && work.calls[1] && work.calls[2];}));
    const auto request=fixtures(root,"import-new",10,12);
    const auto path=request+"/10.wav";
    QStringList errors;
    CHECK(library.addAudioFiles({path,path,request+"/missing.wav"},&errors)==1);
    CHECK(errors.size()==1); CHECK(library.trackCount()==5);
    CHECK(library.pathAt(0)==old+"/00.wav");
    CHECK(until([&]{return work.calls[10]==1;}));
    CHECK(work.calls[0]==1 && !work.calls[3]);
    work.released[10]=true;
    CHECK(until([&]{return bool(library.trackAt(4));}));
    const auto ready=library.trackAt(4);
    CHECK(library.addAudioFiles({path,request+"/11.wav"})==1);
    CHECK(library.trackAt(4)==ready && library.trackCount()==6);
    work.releaseAll();
    CHECK(until([&]{return bool(library.trackAt(5));}));
}

void errors_retry_and_rescans_cancel_old_results(const QString& root)
{
    Workers work;
    gvt::TrackLibrary library;
    gvt::detail::setLibraryAnalyzerForTesting(library, work.analyzer());
    library.scanFolder(fixtures(root, "errors", 0, 4));
    work.fail[3] = true;
    work.released[3] = true;
    library.prioritizeAnalysis(3);
    CHECK(until([&] { return !library.index(3, 5).data(
        gvt::TrackLibrary::AnalysisErrorRole).toString().isEmpty(); }));
    CHECK(!library.trackAt(3));
    CHECK(library.index(3, 5).data(Qt::ToolTipRole).toString().contains("Broken test audio"));
    work.fail[3] = false;
    library.prioritizeAnalysis(3);
    CHECK(until([&] { return bool(library.trackAt(3)); }));
    CHECK(work.calls[3] == 2);
    int finished = 0;
    QObject::connect(&library, &gvt::TrackLibrary::trackReady, &library,
        [&](int row) { ++finished; CHECK(library.pathAt(row).contains("new-scan")); });
    library.scanFolder(fixtures(root, "new-scan", 10, 12));
    work.released[10] = work.released[11] = true;
    CHECK(until([&] { return library.trackAt(0) && library.trackAt(1); }));
    CHECK(finished == 2);
    CHECK(library.trackAt(0)->title == "Track 10");
    // Destructor must cancel active work and must not destroy the pool on a worker.
    QElapsedTimer shutdown;
    shutdown.start();
    {
        gvt::TrackLibrary quitting;
        gvt::detail::setLibraryAnalyzerForTesting(quitting, work.analyzer());
        quitting.scanFolder(fixtures(root, "shutdown", 20, 24));
        CHECK(until([&] { return work.calls[20] == 1; }));
    }
    CHECK(shutdown.elapsed() < 2000);
    QCoreApplication::processEvents();
}

void queued_loads_follow_the_requested_song_not_selection(const QString& root)
{
    Workers work;
    gvt::ControlBus bus;
    gvt::AudioEngine engine(&bus); // no audio device
    gvt::TrackLibrary library;
    gvt::TransitionStore transitions;
    gvt::detail::setLibraryAnalyzerForTesting(library, work.analyzer());
    library.scanFolder(fixtures(root, "ui", 0, 9));
    gvt::LibraryWidget widget(&library, &engine, &transitions);
    widget.resize(1000, 420);
    widget.show();
    auto* table = widget.findChild<QTableView*>("trackLibraryTable");
    auto* load = widget.findChild<QPushButton*>("libraryLoadA");
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(table->model());
    const auto select = [&](int row) { table->setCurrentIndex(proxy->mapFromSource(library.index(row, 0))); };
    select(7);
    CHECK(load->isEnabled());
    load->click();
    CHECK(until([&] { return work.calls[7] == 1; }));
    work.fraction[7] = .47;
    CHECK(until([&] { return library.index(7, 5).data(
        gvt::TrackLibrary::AnalysisProgressRole).toDouble() == .47; }));
    if (!qgetenv("GRAVITINO_ANALYSIS_SCREENSHOT").isEmpty())
        CHECK(widget.grab().save(QString::fromUtf8(qgetenv("GRAVITINO_ANALYSIS_SCREENSHOT"))));
    select(8); // selecting another row alone must not redirect the pending load
    table->sortByColumn(0, Qt::DescendingOrder);
    work.released[7] = true;
    CHECK(until([&] { return engine.deck(0).track() == library.trackAt(7) && library.trackAt(7); }));
    CHECK(!engine.deck(0).playing.load());
    CHECK(!engine.deck(1).track());
    const auto previous = engine.deck(0).track();
    select(6);
    widget.loadSelectedTo(0);
    CHECK(until([&] { return work.calls[6] == 1; }));
    engine.deck(0).playing.store(true);
    work.released[6] = true;
    CHECK(until([&] { return bool(library.trackAt(6)); }));
    CHECK(engine.deck(0).track() == previous);
    engine.deck(0).playing.store(false);
    // Replacing a queued request wins, regardless of which worker finishes first.
    select(5);
    CHECK(QMetaObject::invokeMethod(&widget, "onDoubleClicked", Qt::DirectConnection,
                                   Q_ARG(QModelIndex, table->currentIndex())));
    select(4);
    widget.loadSelectedTo(0);
    work.released[5] = true;
    CHECK(until([&] { return bool(library.trackAt(5)); }));
    CHECK(engine.deck(0).track() == previous);
    work.released[4] = true;
    CHECK(until([&] { return engine.deck(0).track() == library.trackAt(4) && library.trackAt(4); }));
    select(8);
    widget.loadSelectedTo(1);
    widget.setEnabled(false); // e.g. editor MASTER lease disables the live UI
    work.released[8] = true;
    CHECK(until([&] { return bool(library.trackAt(8)); }));
    CHECK(!engine.deck(1).track());
    widget.setEnabled(true);
    // The engine lease itself must guard deferred loads, even without disabled UI.
    struct Preview : gvt::AudioPreviewSource {
        void read(float*, int) noexcept override {}
    } preview;
    select(0);
    widget.loadSelectedTo(1);
    CHECK(engine.acquireExclusivePreview(&preview));
    work.released[0] = true;
    CHECK(until([&] { return bool(library.trackAt(0)); }));
    CHECK(!engine.deck(1).track());
    engine.releaseExclusivePreview(&preview);
    select(1);
    widget.loadSelectedTo(1);
    library.scanFolder(fixtures(root, "ui-rescan", 10, 12));
    work.released[10] = work.released[11] = true;
    CHECK(until([&] { return library.trackAt(0) && library.trackAt(1); }));
    CHECK(!engine.deck(1).track());
    work.releaseAll();
}

void real_analysis_progress_does_not_change_audio_or_grids(const QString& root)
{
    const QString path = root + "/real.wav";
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        QDataStream out(&file);
        out.setByteOrder(QDataStream::LittleEndian);
        constexpr int frames = 48000 * 6;
        file.write("RIFF", 4); out << quint32(36 + frames * 4);
        file.write("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
            << quint32(192000) << quint16(4) << quint16(16);
        file.write("data", 4); out << quint32(frames * 4);
        for (int i = 0; i < frames; ++i) {
            const qint16 sample = (i % 24000 < 200) ? 16000 : 0;
            out << sample << sample;
        }
    }
    double previous = 0;
    int updates = 0;
    QStringList stages;
    QString error;
    const auto measured = gvt::detail::loadAndAnalyzeWithProgress(path, &error,
        [&](double p, const QString& stage) {
            CHECK(p >= previous && p <= 1.0);
            previous = p;
            ++updates;
            if (!stages.contains(stage)) stages << stage;
        });
    const auto ordinary = gvt::loadAndAnalyzeTrack(path, &error);
    CHECK(measured && ordinary);
    if (measured && ordinary) {
        CHECK(measured->pcm == ordinary->pcm);
        CHECK(measured->overviewPeaks == ordinary->overviewPeaks);
        CHECK(measured->overviewLow == ordinary->overviewLow);
        CHECK(measured->bpm == ordinary->bpm);
        CHECK(measured->firstBeatSec == ordinary->firstBeatSec);
        CHECK(measured->camelotKey == ordinary->camelotKey);
        CHECK(measured->structureFingerprint == ordinary->structureFingerprint);
    }
    CHECK(updates > 20);
    for (const char* stage : {"Decoding", "Waveform", "Fingerprint", "BPM", "Key", "Finishing"})
        CHECK(stages.contains(stage));
    // Cancelling a decode safely releases its native decoder, then it can reopen.
    bool cancelled = false;
    std::vector<float> pcm;
    try {
        gvt::detail::decodeAudioStereo48k(path, pcm, &error, [](double p) { if (p > 0) throw 1; });
    } catch (int) { cancelled = true; }
    CHECK(cancelled);
    CHECK(gvt::detail::decodeAudioStereo48k(path, pcm, &error));
}
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(gvt::appStyleSheet());
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (!temporary.isValid()) return 1;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temporary.path());
    qputenv("GRAVITINO_CACHE_DIR", temporary.filePath("cache").toUtf8());
    qputenv("GRAVITINO_CATALOG_PATH", temporary.filePath("catalog.json").toUtf8());
    qputenv("GRAVITINO_TRANSITIONS_DIR", temporary.filePath("transitions").toUtf8());
    load_requests_skip_background_work_and_do_not_duplicate(temporary.path());
    transition_click_prioritizes_both_songs_and_waits_for_verified_audio(temporary.path());
    request_import_keeps_existing_tracks_and_workers(temporary.path());
    errors_retry_and_rescans_cancel_old_results(temporary.path());
    queued_loads_follow_the_requested_song_not_selection(temporary.path());
    real_analysis_progress_does_not_change_audio_or_grids(temporary.path());
    std::printf("library analysis queue: %d failures\n", failures);
    return failures ? 1 : 0;
}
