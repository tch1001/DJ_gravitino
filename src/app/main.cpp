#include <QApplication>
#include <QMessageBox>
#include <QTimer>

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
    const bool audioOk = engine->start(&audioError);

    gvt::MainWindow win(bus, engine, library, store, recorder, player, midi,
                        masterRec, stems);
    win.show();

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
