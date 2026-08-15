#include "MainWindow.h"

#include "DeckWidget.h"
#include "DetailWaveformView.h"
#include "LibraryWidget.h"
#include "MixerWidget.h"
#include "Theme.h"
#include "TransitionPanel.h"
#include "../audio/MasterRecorder.h"
#include "../library/History.h"

#include <QDesktopServices>
#include <QPushButton>
#include <QTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

namespace gvt {

QString appStyleSheet()
{
    return QStringLiteral(R"(
* { font-size: 11px; }
QMainWindow, QDialog { background: #16181d; }
QWidget { color: #d8dce4; background: #16181d; }
QWidget[panel="true"] { background: #2a2e37; border-radius: 6px; }
QLabel { background: transparent; }
QPushButton {
    background: #383d48; border: 1px solid #4a505c; border-radius: 4px;
    padding: 2px 8px; color: #d8dce4;
}
QPushButton:hover { background: #434956; }
QPushButton:pressed { background: #2a2e37; }
QPushButton:disabled { color: #6a707d; background: #2e323b; }
QLineEdit {
    background: #16181d; border: 1px solid #4a505c; border-radius: 4px;
    padding: 4px 6px; selection-background-color: #35c8e8;
    selection-color: black;
}
QTableView, QListWidget {
    background: #1c1f25; alternate-background-color: #21242b;
    border: 1px solid #383d48; gridline-color: #2a2e37;
    selection-background-color: #35c8e8; selection-color: black;
}
QHeaderView::section {
    background: #2a2e37; color: #8a909c; border: none;
    border-right: 1px solid #383d48; padding: 2px 5px;
}
QSlider:vertical { min-height: 84px; min-width: 22px; }
QSlider:horizontal { min-width: 120px; min-height: 22px; }
QSlider::groove:vertical { background: #383d48; width: 6px; border-radius: 3px; }
QSlider::groove:horizontal { background: #383d48; height: 6px; border-radius: 3px; }
QSlider::handle:vertical {
    background: #b8bec9; height: 14px; margin: 0 -6px; border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #b8bec9; width: 14px; margin: -6px 0; border-radius: 3px;
}
QSlider::handle:hover { background: #ffffff; }
QDial { background: #383d48; }
QProgressBar {
    background: #16181d; border: 1px solid #383d48; border-radius: 4px;
}
QProgressBar::chunk { background: #35c8e8; border-radius: 3px; }
QMenuBar { background: #16181d; }
QMenuBar::item:selected { background: #2a2e37; }
QMenu { background: #2a2e37; border: 1px solid #4a505c; }
QMenu::item:selected { background: #35c8e8; color: black; }
QStatusBar { background: #1c1f25; color: #8a909c; }
QToolTip { background: #2a2e37; color: #d8dce4; border: 1px solid #4a505c; }
QScrollBar { background: #16181d; }
)");
}

MainWindow::MainWindow(ControlBus* bus, AudioEngine* engine,
                       TrackLibrary* library, TransitionStore* store,
                       TransitionRecorder* recorder, TransitionPlayer* player,
                       MidiEngine* midi, MasterRecorder* rec, QWidget* parent)
    : QMainWindow(parent), bus_(bus), engine_(engine), library_(library),
      store_(store), midi_(midi), rec_(rec)
{
    setWindowTitle(tr("Gravitino DJ"));
    setMinimumSize(1200, 720);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    // Row 1: Deck A | Deck B — exactly equal halves (Serato-style, no
    // center mixer column; tempo sliders sit on the outer edges).
    deckA_ = new DeckWidget(0, bus_, engine_);
    deckB_ = new DeckWidget(1, bus_, engine_);
    auto* deckRow = new QHBoxLayout;
    deckRow->setSpacing(6);
    deckRow->addWidget(deckA_, 1);
    deckRow->addWidget(deckB_, 1);
    root->addLayout(deckRow);

    // Row 2: full-width scrolling detail waveforms (A lane over B lane,
    // fixed center playhead).
    detailWave_ = new DetailWaveformView(engine_);
    root->addWidget(detailWave_);

    // Row 3: compact horizontal mixer strip | transition panel.
    mixer_ = new MixerWidget(bus_);
    transitionPanel_ =
        new TransitionPanel(bus_, engine_, store_, recorder, player);
    auto* mixRow = new QHBoxLayout;
    mixRow->setSpacing(6);
    mixRow->addWidget(mixer_);
    mixRow->addWidget(transitionPanel_, 2);
    root->addLayout(mixRow);

    // Row 4: library (takes the remaining space). The load history is
    // constructed here (persisted at ~/.gravitino/history.jsonl) and shown
    // in the library's History tab.
    history_ = new History(this);
    libraryWidget_ = new LibraryWidget(library_, engine_, history_);
    root->addWidget(libraryWidget_, 2);

    setCentralWidget(central);

    // Menus.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("Open Music Folder…"), this,
                        &MainWindow::openMusicFolder);
    QMenu* transMenu = menuBar()->addMenu(tr("&Transitions"));
    transMenu->addAction(tr("Open Transitions Folder"), this,
                         &MainWindow::openTransitionsFolder);
    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("About Gravitino"), this, &MainWindow::about);

    // Status-bar LEFT: master-record toggle (hidden when no recorder was
    // provided — the button is simply never created).
    if (rec_) {
        recBtn_ = new QPushButton(tr("● REC MASTER"));
        recBtn_->setToolTip(
            tr("Record the master output to a WAV file"));
        recBtn_->setCursor(Qt::PointingHandCursor);
        statusBar()->addWidget(recBtn_); // left side
        connect(recBtn_, &QPushButton::clicked, this,
                &MainWindow::onRecClicked);
        connect(rec_, &MasterRecorder::recordingChanged, this,
                &MainWindow::onRecordingChanged);
        recTimer_ = new QTimer(this);
        recTimer_->setInterval(1000);
        connect(recTimer_, &QTimer::timeout, this, [this] {
            const int s = (int)rec_->recordedSec();
            recBtn_->setText(
                QString::asprintf("● %02d:%02d", s / 60, s % 60));
        });
        onRecordingChanged(rec_->isRecording(), rec_->currentPath());
    }

    // Status bar: MIDI indicator, sample rate, transient messages.
    midiLabel_ = new QLabel;
    rateLabel_ = new QLabel(tr("%1 Hz").arg(kSampleRate));
    statusBar()->addPermanentWidget(midiLabel_);
    statusBar()->addPermanentWidget(rateLabel_);
    onMidiConnection(midi_->controllerConnected(), midi_->controllerName());
    connect(midi_, &MidiEngine::connectionChanged, this,
            &MainWindow::onMidiConnection);

    // Cross-widget wiring.
    connect(libraryWidget_, &LibraryWidget::trackLoaded, this,
            [this](int deck) { notifyTrackLoaded(deck); });
    connect(libraryWidget_, &LibraryWidget::statusMessage, this,
            [this](const QString& msg, int timeoutMs) {
                statusBar()->showMessage(msg, timeoutMs);
            });
    connect(transitionPanel_, &TransitionPanel::entryMarkerChanged, this,
            &MainWindow::setTransitionEntryMarker);
    connect(transitionPanel_, &TransitionPanel::statusMessage, this,
            [this](const QString& msg, int timeoutMs) {
                statusBar()->showMessage(msg, timeoutMs);
            });
}

void MainWindow::openMusicFolder()
{
    QString start =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Music Folder"), start);
    if (dir.isEmpty()) return;
    library_->scanFolder(dir);
    statusBar()->showMessage(tr("Scanning %1…").arg(dir), 4000);
}

void MainWindow::openTransitionsFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(store_->directory()));
}

void MainWindow::about()
{
    QMessageBox::about(
        this, tr("About Gravitino"),
        tr("<b>Gravitino DJ</b><br>Record a mixing transition once, "
           "replay or teach it beat-perfectly.<br><br>"
           "Open source, Qt %1.")
            .arg(QLatin1String(qVersion())));
}

void MainWindow::onMidiConnection(bool connected, const QString& name)
{
    if (connected) {
        midiLabel_->setText(
            tr("%1 connected").arg(name.isEmpty() ? tr("controller") : name));
        midiLabel_->setStyleSheet("color:#4cd964; font-weight:bold;");
    } else {
        midiLabel_->setText(tr("no controller — plug in any time"));
        midiLabel_->setStyleSheet(
            QStringLiteral("color:%1;").arg(themeDimText().name()));
    }
}

void MainWindow::onRecClicked()
{
    if (!rec_) return;
    if (rec_->isRecording()) {
        rec_->stop();
        return;
    }
    QString err;
    if (!rec_->start(QString(), &err))
        statusBar()->showMessage(
            tr("Master recording failed: %1").arg(err), 6000);
    // Button state follows recordingChanged, not the click.
}

void MainWindow::onRecordingChanged(bool active, const QString& path)
{
    if (!recBtn_) return;
    if (active) {
        recBtn_->setText(QStringLiteral("● 00:00"));
        recBtn_->setStyleSheet(
            "QPushButton { background:#c8322e; color:white; "
            "font-weight:bold; }");
        recBtn_->setToolTip(tr("Recording master → %1 — click to stop")
                                .arg(path));
        recTimer_->start();
        statusBar()->showMessage(tr("Recording master → %1").arg(path),
                                 4000);
    } else {
        recTimer_->stop();
        recBtn_->setText(tr("● REC MASTER"));
        recBtn_->setStyleSheet(QString());
        recBtn_->setToolTip(
            tr("Record the master output to a WAV file"));
        if (!path.isEmpty())
            statusBar()->showMessage(tr("Saved recording: %1").arg(path),
                                     6000);
    }
}

void MainWindow::notifyTrackLoaded(int deck)
{
    // Log to the session history (covers both library loads and the
    // --autoload dev hook, which land here alike).
    if (history_ && (deck == 0 || deck == 1)) {
        if (TrackDataPtr t = engine_->deck(deck).track())
            history_->logLoad(deck, *t);
    }
    (deck == 0 ? deckA_ : deckB_)->trackChanged();
    detailWave_->update();
    transitionPanel_->refreshMatches();
}

void MainWindow::setTransitionEntryMarker(int deck, double sec)
{
    if (deck != 0 && deck != 1) return;
    (deck == 0 ? deckA_ : deckB_)->setTransitionEntry(sec);
    detailWave_->setTransitionEntry(deck, sec);
}

} // namespace gvt
