#include <QApplication>
#include <QMessageBox>

#include "../audio/AudioEngine.h"
#include "../control/ControlBus.h"
#include "../library/TrackLibrary.h"
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

    QString audioError;
    const bool audioOk = engine->start(&audioError);

    gvt::MainWindow win(bus, engine, library, store, recorder, player, midi);
    win.show();

    if (!audioOk) {
        QMessageBox::warning(
            &win, QObject::tr("Audio device unavailable"),
            QObject::tr("Couldn't start audio output:\n%1\n\n"
                        "The UI stays usable; fix the device and restart.")
                .arg(audioError));
    }

    return app.exec();
}
