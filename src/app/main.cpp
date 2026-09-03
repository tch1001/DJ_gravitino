#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

#include "../audio/AudioEngine.h"
#include "../control/ControlBus.h"
#include "../library/TrackLibrary.h"
#include "../analysis/StemSeparator.h"
#include "../audio/MasterRecorder.h"
#include "../midi/MidiEngine.h"
#include "../transitions/TransitionEngine.h"
#include "../ui/MainWindow.h"
#include "../ui/Theme.h"
#include "SelfTest.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Gravitino"));
    app.setOrganizationName(QStringLiteral("Gravitino"));

    qRegisterMetaType<gvt::ControlEvent>();
    qRegisterMetaType<gvt::Origin>();

    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--selftest")))
        return gvt::runSelfTest(args); // headless: no window, no live device
    if (args.contains(QStringLiteral("--convert-transitions"))) {
        gvt::TransitionStore store;
        QStringList converted;
        QStringList errors;
        const int count = store.convertAllLegacy(&converted, &errors);
        std::printf("Transition directory: %s\n",
                    qUtf8Printable(store.directory()));
        for (const QString& path : converted)
            std::printf("converted: %s\n", qUtf8Printable(path));
        for (const QString& error : errors)
            std::fprintf(stderr, "conversion failed: %s\n",
                         qUtf8Printable(error));
        std::printf("Converted %d legacy transition%s; %lld error%s.\n",
                    count, count == 1 ? "" : "s",
                    static_cast<long long>(errors.size()),
                    errors.size() == 1 ? "" : "s");
        return errors.isEmpty() ? 0 : 1;
    }

    // Two GUI processes would each open their own CoreAudio stream. A hidden
    // older process can then sound exactly like one deck is playing two tracks
    // at once, even though each process has only one source per deck. Hold a
    // per-user lock for the full GUI lifetime so only one audio engine exists.
    const QString lockPath = QDir(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("gravitino-dj-gui.lock"));
    QLockFile instanceLock(lockPath);
    if (!instanceLock.tryLock(0)) {
        QMessageBox::information(
            nullptr, QObject::tr("Gravitino is already running"),
            QObject::tr("Another Gravitino window already owns the audio "
                        "output. Close or use that window before opening a "
                        "new one."));
        return 0;
    }

    app.setStyleSheet(gvt::appStyleSheet());

    // Engine-side objects are heap-allocated with process lifetime (never
    // deleted): TransitionStore/Recorder/Player use pImpl without a declared
    // destructor in their pinned headers, so destroying them from this TU
    // would not compile — and none of these should die before the UI anyway.
    auto* bus = new gvt::ControlBus;
    auto* engine = new gvt::AudioEngine(bus);
    auto* library = new gvt::TrackLibrary;
    library->scanFolder(QString()); // default: ~/Music
    auto* store = new gvt::TransitionStore;
    store->reload();
    auto* recorder = new gvt::TransitionRecorder(bus, engine);
    auto* player = new gvt::TransitionPlayer(bus, engine);
    auto* midi = new gvt::MidiEngine(bus, engine);
    midi->start();
    auto* masterRec = new gvt::MasterRecorder;
    engine->masterTap.store(masterRec);
    auto* stems = new gvt::StemSeparator;

    QString audioError;
    const QString preferredOutput =
        QSettings().value(QStringLiteral("audio/outputDevice")).toString();
    bool audioOk = engine->start(preferredOutput, &audioError);
    QString savedOutputWarning;
    if (!audioOk && !preferredOutput.isEmpty()) {
        savedOutputWarning = QObject::tr(
            "The saved audio output “%1” is unavailable:\n%2\n\n"
            "Gravitino is using the macOS system default for this session.")
                                 .arg(preferredOutput, audioError);
        audioOk = engine->start(&audioError);
    }

    gvt::MainWindow win(bus, engine, library, store, recorder, player, midi,
                        masterRec, stems);
    win.show();

    if (args.contains(QStringLiteral("--cue-test"))) {
        QTimer::singleShot(1500, &win, [engine] {
            engine->startHeadphoneTest(4000);
        });
        QTimer::singleShot(2200, &win, [engine] {
            qInfo().noquote() << "FLX4 headphone diagnostic signal peak:"
                              << engine->headphoneSignalLevel();
        });
    }

    if (!savedOutputWarning.isEmpty()) {
        QTimer::singleShot(0, &win, [&win, savedOutputWarning] {
            QMessageBox::warning(&win, QObject::tr("Audio output changed"),
                                 savedOutputWarning);
        });
    }

    // Dev flag: --autoload "<substr for deck A>" "<substr for deck B>" loads
    // the first library tracks whose path matches each substring once analyzed
    // (defaults: Demo Track 1 / Demo Track 2). Used for automated UI testing.
    if (args.contains(QStringLiteral("--autoload"))) {
        const int ai = args.indexOf(QStringLiteral("--autoload"));
        const QString wantA = args.value(ai + 1).isEmpty() || args.value(ai + 1).startsWith("--")
                                  ? QStringLiteral("Demo Track 1") : args.value(ai + 1);
        const QString wantB = args.value(ai + 2).isEmpty() || args.value(ai + 2).startsWith("--")
                                  ? QStringLiteral("Demo Track 2") : args.value(ai + 2);
        auto* poll = new QTimer(&win);
        QObject::connect(poll, &QTimer::timeout, [&win, library, engine, poll, wantA, wantB]() {
            static bool loadedA = false, loadedB = false;
            for (int r = 0; r < library->trackCount(); ++r) {
                const QString p = library->pathAt(r);
                gvt::TrackDataPtr t;
                if (!loadedA && p.contains(wantA, Qt::CaseInsensitive) && (t = library->trackAt(r))) {
                    engine->deck(0).loadTrack(t); win.notifyTrackLoaded(0); loadedA = true;
                }
                if (!loadedB && p.contains(wantB, Qt::CaseInsensitive) && (t = library->trackAt(r))) {
                    engine->deck(1).loadTrack(t); win.notifyTrackLoaded(1); loadedB = true;
                }
            }
            if (loadedA && loadedB) poll->stop();
        });
        poll->start(500);
    }

    if (!audioOk) {
        QMessageBox::warning(
            &win, QObject::tr("Audio device unavailable"),
            QObject::tr("Couldn't start audio output:\n%1\n\n"
                        "The UI stays usable; fix the device and restart.")
                .arg(audioError));
    }

    return app.exec();
}
