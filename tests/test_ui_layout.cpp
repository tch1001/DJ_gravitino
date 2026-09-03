#include "audio/AudioEngine.h"
#include "audio/MasterRecorder.h"
#include "control/ControlBus.h"
#include "library/TrackLibrary.h"
#include "midi/MidiEngine.h"
#include "transitions/TransitionEngine.h"
#include "ui/LibraryWidget.h"
#include "ui/MainWindow.h"
#include "ui/TransitionPanel.h"

#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

QString headerText(QTableWidget* table, int column)
{
    QTableWidgetItem* item = table ? table->horizontalHeaderItem(column)
                                   : nullptr;
    return item ? item->text() : QString();
}

gvt::TrackDataPtr makeTrack(const QString& title,
                            const QString& fingerprint)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->title = title;
    track->fingerprint = fingerprint;
    track->durationSec = 16.0;
    track->bpm = 120.0;
    track->firstBeatSec = 0.0;
    track->pcm.resize(
        static_cast<std::size_t>(gvt::kSampleRate) * 16U * 2U);
    return track;
}

void sendMouse(QWidget* receiver, QEvent::Type type,
               const QPoint& localPosition, const QPoint& globalPosition,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(localPosition), QPointF(globalPosition),
                      button, buttons, Qt::NoModifier);
    QApplication::sendEvent(receiver, &event);
}
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("GravitinoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_ui_layout"));
    QTemporaryDir settingsDirectory;
    CHECK(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());
    qputenv("GRAVITINO_TRANSITIONS_DIR",
            (settingsDirectory.path() + QStringLiteral("/transitions"))
                .toUtf8());

    gvt::ControlBus bus;
    gvt::ControlId lastMousePlatterControl = gvt::ControlId::Count;
    double lastMousePlatterValue = 0.0;
    int mousePlatterTouchEvents = 0;
    QObject::connect(
        &bus, &gvt::ControlBus::eventDispatched,
        [&lastMousePlatterControl, &lastMousePlatterValue,
         &mousePlatterTouchEvents](
            const gvt::ControlEvent& event, gvt::Origin origin) {
            if (origin != gvt::Origin::Ui || event.deck != 0 ||
                (event.id != gvt::ControlId::Jog &&
                 event.id != gvt::ControlId::PlatterScratch &&
                 event.id != gvt::ControlId::PlatterTouch))
                return;
            lastMousePlatterControl = event.id;
            lastMousePlatterValue = event.value;
            if (event.id == gvt::ControlId::PlatterTouch)
                ++mousePlatterTouchEvents;
        });
    gvt::AudioEngine engine(&bus);
    gvt::TrackLibrary library;
    gvt::TransitionStore store;
    gvt::TransitionRecorder transitionRecorder(&bus, &engine);
    gvt::TransitionPlayer transitionPlayer(&bus, &engine);
    gvt::MidiEngine midi(&bus, &engine);

    gvt::GvtFile sample;
    sample.name = QStringLiteral("Progress UI fixture");
    sample.from.title = QStringLiteral("Outgoing fixture");
    sample.from.fingerprint = QStringLiteral("outgoing-fingerprint");
    sample.to.title = QStringLiteral("Incoming fixture");
    sample.to.fingerprint = QStringLiteral("incoming-fingerprint");
    sample.anchorFromBeat = 100.0;
    sample.anchorToBeat = 200.0;
    sample.events = {
        {2.0, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0,
         gvt::Curve::Step},
        {4.0, gvt::Role::Mixer, gvt::ControlId::Crossfader, 1.0,
         gvt::Curve::Step},
    };
    QString saveError;
    CHECK(!store.save(sample, &saveError).isEmpty());
    engine.deck(0).loadTrack(
        makeTrack(sample.from.title, sample.from.fingerprint));
    engine.deck(1).loadTrack(
        makeTrack(sample.to.title, sample.to.fingerprint));
    engine.deck(0).track()->hotCues[0] = 1.0;

    {
        gvt::MainWindow window(&bus, &engine, &library, &store,
                               &transitionRecorder, &transitionPlayer, &midi);
        auto* libraryToggle = window.findChild<QPushButton*>(
            QStringLiteral("libraryVisibilityToggle"));
        CHECK(libraryToggle != nullptr);
        CHECK(libraryToggle && window.statusBar()->isAncestorOf(libraryToggle));
        CHECK(window.findChild<QPushButton*>(
                  QStringLiteral("masterRecordButton")) == nullptr);
        auto* libraryPanel = window.findChild<gvt::LibraryWidget*>();
        window.resize(1200, 720);
        window.show();
        app.processEvents();
        CHECK(libraryPanel != nullptr);
        CHECK(libraryPanel && !libraryPanel->isHidden());
        if (libraryToggle) libraryToggle->click();
        CHECK(libraryPanel && libraryPanel->isHidden());
        CHECK(libraryToggle && libraryToggle->text().contains(
                                   QStringLiteral("SHOW LIBRARY")));
        if (libraryToggle) libraryToggle->click();
        CHECK(libraryPanel && !libraryPanel->isHidden());
        CHECK(libraryToggle && libraryToggle->text().contains(
                                   QStringLiteral("HIDE LIBRARY")));
        auto* midiStatus = window.findChild<QLabel*>(
            QStringLiteral("midiConnectionStatus"));
        CHECK(midiStatus != nullptr);
        const int libraryRight = libraryToggle
                                     ? libraryToggle
                                           ->mapTo(window.statusBar(),
                                                   libraryToggle->rect().topRight())
                                           .x()
                                     : 0;
        const int midiLeft = midiStatus
                                 ? midiStatus
                                       ->mapTo(window.statusBar(),
                                               midiStatus->rect().topLeft())
                                       .x()
                                 : 0;
        CHECK(libraryToggle && midiStatus && libraryRight <= midiLeft);
        window.statusBar()->showMessage(
            QStringLiteral("Tutor view opened — temporary status text"));
        app.processEvents();
        CHECK(libraryToggle && libraryToggle->isVisible());
        window.statusBar()->clearMessage();

        // A mouse can reproduce the controller's HOT CUE hold + PLAY latch:
        // press a mapped pad, drag onto PLAY, then release there.
        auto* pad = window.findChild<QPushButton*>(
            QStringLiteral("deck0PerformancePad1"));
        auto* play = window.findChild<QPushButton*>(
            QStringLiteral("deck0PlayButton"));
        auto* wheel = window.findChild<QWidget*>(
            QStringLiteral("deck0JogWheel"));
        CHECK(pad != nullptr);
        CHECK(play != nullptr);
        CHECK(wheel != nullptr);
        engine.deck(0).stop();
        engine.deck(0).seekSec(0.25);
        app.processEvents();
        if (pad && play) {
            const QPoint padLocal = pad->rect().center();
            const QPoint padGlobal = pad->mapToGlobal(padLocal);
            sendMouse(pad, QEvent::MouseButtonPress, padLocal, padGlobal,
                      Qt::LeftButton, Qt::LeftButton);
            CHECK(engine.deck(0).previewActive());
            const QPoint playGlobal = play->mapToGlobal(play->rect().center());
            const QPoint overPlay = pad->mapFromGlobal(playGlobal);
            sendMouse(pad, QEvent::MouseMove, overPlay, playGlobal,
                      Qt::NoButton, Qt::LeftButton);
            CHECK(play->property("hotCueDropTarget").toBool());
            sendMouse(pad, QEvent::MouseButtonRelease, overPlay, playGlobal,
                      Qt::LeftButton, Qt::NoButton);
            CHECK(!engine.deck(0).previewActive());
            CHECK(engine.deck(0).playing.load());
            CHECK(!play->property("hotCueDropTarget").toBool());

            // Releasing on the pad retains ordinary momentary-preview
            // behavior; only a drop over PLAY latches it.
            engine.deck(0).stop();
            engine.deck(0).seekSec(0.25);
            sendMouse(pad, QEvent::MouseButtonPress, padLocal, padGlobal,
                      Qt::LeftButton, Qt::LeftButton);
            CHECK(engine.deck(0).previewActive());
            sendMouse(pad, QEvent::MouseButtonRelease, padLocal, padGlobal,
                      Qt::LeftButton, Qt::NoButton);
            CHECK(!engine.deck(0).previewActive());
            CHECK(!engine.deck(0).playing.load());
        }

        // While playing, the mouse wheel performs an immediate fine position
        // adjustment without touch-gating or interrupting transport.
        if (wheel) {
            engine.deck(0).seekSec(1.0);
            engine.deck(0).play();
            const QPoint top(wheel->width() / 2, 7);
            const QPoint right(wheel->width() - 7, wheel->height() / 2);
            lastMousePlatterControl = gvt::ControlId::Count;
            sendMouse(wheel, QEvent::MouseButtonPress, top,
                      wheel->mapToGlobal(top), Qt::LeftButton,
                      Qt::LeftButton);
            CHECK(engine.deck(0).playing.load());
            sendMouse(wheel, QEvent::MouseMove, right,
                      wheel->mapToGlobal(right), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterControl ==
                  gvt::ControlId::PlatterScratch);
            CHECK(lastMousePlatterValue > 4.4);
            CHECK(lastMousePlatterValue < 4.6);
            const double playingAdjustment =
                engine.deck(0).positionSec() - 1.0;
            CHECK(playingAdjustment > 0.04);
            CHECK(playingAdjustment < 0.05);
            CHECK(engine.deck(0).playing.load());
            float scratchAudio[512 * 2] {};
            engine.renderOffline(scratchAudio, 512);
            CHECK(engine.deck(0).positionSec() >
                  1.04 + 512.0 / gvt::kSampleRate);
            CHECK(wheel->property("rotationDegrees").toDouble() > 0.0);
            sendMouse(wheel, QEvent::MouseButtonRelease, right,
                      wheel->mapToGlobal(right), Qt::LeftButton,
                      Qt::NoButton);
            CHECK(engine.deck(0).playing.load());

            // A paused deck gets the same direct fine positioning and remains
            // paused throughout the gesture.
            engine.deck(0).stop();
            engine.deck(0).seekSec(1.0);
            lastMousePlatterControl = gvt::ControlId::Count;
            sendMouse(wheel, QEvent::MouseButtonPress, top,
                      wheel->mapToGlobal(top), Qt::LeftButton,
                      Qt::LeftButton);
            CHECK(!engine.deck(0).playing.load());
            sendMouse(wheel, QEvent::MouseMove, right,
                      wheel->mapToGlobal(right), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterControl ==
                  gvt::ControlId::PlatterScratch);
            CHECK(lastMousePlatterValue > 4.4);
            CHECK(lastMousePlatterValue < 4.6);
            const double mouseAdjustment =
                engine.deck(0).positionSec() - 1.0;
            CHECK(mouseAdjustment > 0.04);
            CHECK(mouseAdjustment < 0.05);
            sendMouse(wheel, QEvent::MouseButtonRelease, right,
                      wheel->mapToGlobal(right), Qt::LeftButton,
                      Qt::NoButton);
            CHECK(!engine.deck(0).playing.load());
            CHECK(mousePlatterTouchEvents == 0);
        }

        auto* table = window.findChild<QTableWidget*>(
            QStringLiteral("transitionEventSequence"));
        auto* human = window.findChild<QPushButton*>(
            QStringLiteral("humanSequenceMode"));
        auto* raw = window.findChild<QPushButton*>(
            QStringLiteral("rawSequenceMode"));
        CHECK(table != nullptr);
        CHECK(human && human->isChecked());
        CHECK(raw && !raw->isChecked());
        CHECK(headerText(table, 0) == QStringLiteral("Outgoing Beat"));
        CHECK(headerText(table, 1) == QStringLiteral("Outgoing Action"));
        CHECK(headerText(table, 2) == QStringLiteral("Incoming Action"));
        CHECK(headerText(table, 3) == QStringLiteral("Incoming Beat"));
        CHECK(headerText(table, 4) == QStringLiteral("Label"));
        CHECK(table && table->rowCount() == 3);
        CHECK(table && table->item(0, 1) &&
              table->item(0, 1)->text().contains(
                  QStringLiteral("TRANSITION STARTS")));
        CHECK(table && table->item(0, 0) &&
              table->item(0, 0)->data(Qt::UserRole).toInt() == -1);

        auto* transitionPanel = window.findChild<gvt::TransitionPanel*>();
        CHECK(transitionPanel != nullptr);
        CHECK(transitionPanel && QMetaObject::invokeMethod(
              transitionPanel, "onProgress", Qt::DirectConnection,
              Q_ARG(double, 1.0), Q_ARG(double, 4.0)));
        CHECK(table && table->property("timelineProgressRow").toInt() == 0);
        CHECK(table && std::fabs(
                  table->property("timelineProgressFraction").toDouble() -
                  0.5) < 0.01);

        if (raw) raw->click();
        CHECK(raw && raw->isChecked());
        CHECK(headerText(table, 0) == QStringLiteral("Beat"));
        CHECK(headerText(table, 1) == QStringLiteral("Target"));
        CHECK(headerText(table, 2) == QStringLiteral("Action"));
        CHECK(headerText(table, 3) == QStringLiteral("Value"));
        CHECK(headerText(table, 4) == QStringLiteral("Cue label"));
        CHECK(table && table->rowCount() == 3);
        CHECK(table && table->item(0, 2) &&
              table->item(0, 2)->text() ==
                  QStringLiteral("Transition starts"));
        CHECK(table && table->property("timelineProgressRow").toInt() == 0);
        CHECK(transitionPanel && QMetaObject::invokeMethod(
              transitionPanel, "onProgress", Qt::DirectConnection,
              Q_ARG(double, 2.0), Q_ARG(double, 4.0)));
        CHECK(table && table->property("timelineProgressRow").toInt() == 1);

        if (human) human->click();
        CHECK(human && human->isChecked());
        CHECK(headerText(table, 0) == QStringLiteral("Outgoing Beat"));
    }

    {
        gvt::MasterRecorder masterRecorder;
        gvt::MainWindow window(&bus, &engine, &library, &store,
                               &transitionRecorder, &transitionPlayer, &midi,
                               &masterRecorder);
        auto* master = window.findChild<QPushButton*>(
            QStringLiteral("masterRecordButton"));
        auto* libraryToggle = window.findChild<QPushButton*>(
            QStringLiteral("libraryVisibilityToggle"));
        CHECK(master != nullptr);
        CHECK(libraryToggle != nullptr);
        CHECK(master && window.statusBar()->isAncestorOf(master));
        CHECK(libraryToggle && window.statusBar()->isAncestorOf(libraryToggle));
        window.resize(1200, 720);
        window.show();
        app.processEvents();
        const int masterRight = master
                                    ? master->mapTo(window.statusBar(),
                                                    master->rect().topRight())
                                          .x()
                                    : 0;
        const int libraryLeft = libraryToggle
                                    ? libraryToggle
                                          ->mapTo(window.statusBar(),
                                                  libraryToggle->rect().topLeft())
                                          .x()
                                    : 0;
        CHECK(master && libraryToggle && masterRight <= libraryLeft);
    }

    if (failures) return 1;
    std::puts("test_ui_layout: layout, sequence, and mouse deck controls passed");
    return 0;
}
