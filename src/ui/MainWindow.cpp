#include "MainWindow.h"

#include "DeckWidget.h"
#include "LibraryWidget.h"
#include "MixerWidget.h"
#include "Theme.h"
#include "TransitionPanel.h"

#include <QDesktopServices>
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
* { font-size: 12px; }
QMainWindow, QDialog { background: #16181d; }
QWidget { color: #d8dce4; background: #16181d; }
QWidget[panel="true"] { background: #2a2e37; border-radius: 6px; }
QLabel { background: transparent; }
QPushButton {
    background: #383d48; border: 1px solid #4a505c; border-radius: 4px;
    padding: 4px 10px; color: #d8dce4;
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
    border-right: 1px solid #383d48; padding: 4px 6px;
}
QSlider::groove:vertical { background: #16181d; width: 6px; border-radius: 3px; }
QSlider::groove:horizontal { background: #16181d; height: 6px; border-radius: 3px; }
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
                       MidiEngine* midi, QWidget* parent)
    : QMainWindow(parent), bus_(bus), engine_(engine), library_(library),
      store_(store), midi_(midi)
{
    setWindowTitle(tr("Gravitino DJ"));
    setMinimumSize(1280, 800);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // Row 1: Deck A | Mixer | Deck B.
    deckA_ = new DeckWidget(0, bus_, engine_);
    deckB_ = new DeckWidget(1, bus_, engine_);
    mixer_ = new MixerWidget(bus_);
    auto* deckRow = new QHBoxLayout;
    deckRow->setSpacing(8);
    deckRow->addWidget(deckA_, 4);
    deckRow->addWidget(mixer_, 2);
    deckRow->addWidget(deckB_, 4);
    root->addLayout(deckRow);

    // Row 2: transition panel.
    transitionPanel_ =
        new TransitionPanel(bus_, engine_, store_, recorder, player);
    root->addWidget(transitionPanel_);

    // Row 3: library (takes the remaining space).
    libraryWidget_ = new LibraryWidget(library_, engine_);
    root->addWidget(libraryWidget_, 1);

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
            [this](int deck) {
                (deck == 0 ? deckA_ : deckB_)->trackChanged();
                transitionPanel_->refreshMatches();
            });
    connect(libraryWidget_, &LibraryWidget::statusMessage, this,
            [this](const QString& msg, int timeoutMs) {
                statusBar()->showMessage(msg, timeoutMs);
            });
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

} // namespace gvt
