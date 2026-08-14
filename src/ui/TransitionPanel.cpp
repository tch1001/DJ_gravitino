#include "TransitionPanel.h"
#include "../transitions/TransitionPlayerExt.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gvt {

static QString qualityBadge(MatchQuality q)
{
    switch (q) {
    case MatchQuality::Fingerprint:  return QStringLiteral("● exact");
    case MatchQuality::TitleArtist:  return QStringLiteral("◐ title/artist");
    case MatchQuality::DurationOnly: return QStringLiteral("○ duration");
    case MatchQuality::None:         return QStringLiteral("✕ none");
    }
    return {};
}

TransitionPanel::TransitionPanel(ControlBus* bus, AudioEngine* engine,
                                 TransitionStore* store,
                                 TransitionRecorder* recorder,
                                 TransitionPlayer* player, QWidget* parent)
    : QWidget(parent), bus_(bus), engine_(engine), store_(store),
      recorder_(recorder), player_(player)
{
    setObjectName(QStringLiteral("transitionPanel"));
    setProperty("panel", true);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(10);

    // Left: matching transitions list.
    auto* leftCol = new QVBoxLayout;
    auto* header = new QLabel(tr("TRANSITIONS"));
    header->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                         "letter-spacing:2px;")
                              .arg(themeText().name()));
    leftCol->addWidget(header);
    list_ = new QListWidget;
    list_->setMinimumHeight(70);
    list_->setMaximumHeight(120);
    leftCol->addWidget(list_, 1);
    root->addLayout(leftCol, 1);

    // Right: controls.
    auto* rightCol = new QVBoxLayout;
    auto* buttons = new QHBoxLayout;
    recBtn_ = new QPushButton(tr("● REC"));
    recBtn_->setStyleSheet("color:#e85555; font-weight:bold;");
    stopSaveBtn_ = new QPushButton(tr("■ STOP && SAVE"));
    performBtn_ = new QPushButton(tr("▶ PERFORM"));
    performBtn_->setStyleSheet(
        QStringLiteral("color:%1; font-weight:bold;").arg(deckAccent(0).name()));
    tutorialBtn_ = new QPushButton(tr("🎓 TUTORIAL"));
    tutorialBtn_->setToolTip(
        tr("Practice this transition: prompts appear 4 beats ahead,\n"
           "your moves are scored against the recording."));
    abortBtn_ = new QPushButton(tr("⏹ ABORT"));
    for (auto* b : {recBtn_, stopSaveBtn_, performBtn_, tutorialBtn_, abortBtn_})
        buttons->addWidget(b);
    rightCol->addLayout(buttons);

    auto* statusRow = new QHBoxLayout;
    recIndicator_ = new QLabel;
    recIndicator_->setStyleSheet("color:#e85555; font-weight:bold;");
    statusRow->addWidget(recIndicator_);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    statusRow->addWidget(progress_, 1);
    rightCol->addLayout(statusRow);
    rightCol->addStretch(1);
    root->addLayout(rightCol, 1);

    bannerTimer_ = new QTimer(this);
    bannerTimer_->setSingleShot(true);
    connect(bannerTimer_, &QTimer::timeout, this, [this] {
        if (banner_) banner_->hide();
    });

    connect(recBtn_, &QPushButton::clicked, this, &TransitionPanel::onRec);
    connect(stopSaveBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onStopSave);
    connect(performBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onPerform);
    connect(tutorialBtn_, &QPushButton::clicked, this,
            [this] { startReplay(PlayerMode::Tutorial); });
    connect(abortBtn_, &QPushButton::clicked, this, &TransitionPanel::onAbort);

    connect(store_, &TransitionStore::changed, this,
            &TransitionPanel::refreshMatches);
    connect(recorder_, &TransitionRecorder::eventCaptured, this,
            &TransitionPanel::onEventCaptured);
    connect(player_, &TransitionPlayer::progressChanged, this,
            &TransitionPanel::onProgress);
    connect(player_, &TransitionPlayer::finished, this,
            &TransitionPanel::onFinished);
    connect(player_, &TransitionPlayer::tutorialPrompt, this,
            &TransitionPanel::onTutorialPrompt);
    connect(player_, &TransitionPlayer::tutorialScored, this,
            &TransitionPanel::onTutorialScored);

    refreshMatches();
}

void TransitionPanel::refreshMatches()
{
    matches_.clear();
    list_->clear();

    TrackDataPtr a = engine_->deck(0).track();
    TrackDataPtr b = engine_->deck(1).track();
    if (!a || !b) {
        auto* item = new QListWidgetItem(
            tr("load a track on both decks to see matching transitions"));
        item->setFlags(Qt::NoItemFlags);
        list_->addItem(item);
        return;
    }

    // Both orderings: A→B (fromDeck 0) and B→A (fromDeck 1).
    struct Ordering { const TrackData* from; const TrackData* to; int fromDeck; };
    const Ordering orderings[2] = {{a.get(), b.get(), 0},
                                   {b.get(), a.get(), 1}};
    for (const auto& o : orderings) {
        for (const GvtFile* f : store_->matching(*o.from, *o.to)) {
            Match m;
            m.file = f;
            m.fromDeck = o.fromDeck;
            m.quality = std::min(matchTrack(f->from, *o.from),
                                 matchTrack(f->to, *o.to));
            matches_.push_back(m);
        }
    }

    if (matches_.empty()) {
        auto* item =
            new QListWidgetItem(tr("no saved transitions match this pair"));
        item->setFlags(Qt::NoItemFlags);
        list_->addItem(item);
        return;
    }
    for (const Match& m : matches_) {
        QString dir = m.fromDeck == 0 ? QStringLiteral("A→B")
                                      : QStringLiteral("B→A");
        QString author =
            m.file->author.isEmpty() ? tr("unknown") : m.file->author;
        list_->addItem(QStringLiteral("%1  —  %2  [%3]  %4")
                           .arg(m.file->name, author, dir,
                                qualityBadge(m.quality)));
    }
    list_->setCurrentRow(0);
}

int TransitionPanel::selectedMatch() const
{
    int row = list_->currentRow();
    if (row < 0 || row >= (int)matches_.size()) return -1;
    return row;
}

void TransitionPanel::onRec()
{
    if (recorder_->isRecording()) {
        emit statusMessage(tr("Already recording"), 3000);
        return;
    }
    const bool aPlaying = engine_->deck(0).playing.load();
    const bool bPlaying = engine_->deck(1).playing.load();
    if (aPlaying == bPlaying) { // neither, or both
        emit statusMessage(
            aPlaying
                ? tr("Can't arm recording: both decks are playing — the "
                     "outgoing deck must be the only one playing")
                : tr("Can't arm recording: start the outgoing deck first"),
            5000);
        return;
    }
    int fromDeck = aPlaying ? 0 : 1;
    recorder_->start(fromDeck);
    recIndicator_->setText(tr("REC ● 0 events (from deck %1)")
                               .arg(fromDeck == 0 ? QStringLiteral("A")
                                                  : QStringLiteral("B")));
    emit statusMessage(tr("Recording transition…"), 3000);
}

void TransitionPanel::onEventCaptured(int count)
{
    recIndicator_->setText(tr("REC ● %1 events").arg(count));
}

void TransitionPanel::onStopSave()
{
    if (!recorder_->isRecording()) {
        emit statusMessage(tr("Not recording"), 3000);
        return;
    }
    GvtFile f = recorder_->finish();
    recIndicator_->clear();

    bool ok = false;
    QString name = QInputDialog::getText(
        this, tr("Save transition"), tr("Transition name:"),
        QLineEdit::Normal,
        f.name.isEmpty() ? tr("My Transition") : f.name, &ok);
    if (!ok || name.trimmed().isEmpty()) {
        emit statusMessage(tr("Recording discarded"), 3000);
        return;
    }
    f.name = name.trimmed();
    QString error;
    QString path = store_->save(f, &error);
    if (path.isEmpty())
        emit statusMessage(tr("Save failed: %1").arg(error), 6000);
    else
        emit statusMessage(tr("Saved %1").arg(path), 5000);
    // Store emits changed() -> refreshMatches().
}

void TransitionPanel::onPerform() { startReplay(PlayerMode::Perform); }

void TransitionPanel::startReplay(PlayerMode mode)
{
    int idx = selectedMatch();
    if (idx < 0) {
        emit statusMessage(tr("Select a transition first"), 3000);
        return;
    }
    const Match& m = matches_[(size_t)idx];
    // A stopped from-deck means the beat clock never advances; start it.
    if (!engine_->deck(m.fromDeck).playing.load())
        bus_->dispatch({m.fromDeck, ControlId::Play, 1.0}, Origin::System);
    transitionPlayerSetMode(player_, mode);
    QString error;
    if (!player_->arm(*m.file, m.fromDeck, /*startNow=*/true, &error)) {
        emit statusMessage(tr("Can't start: %1").arg(error), 6000);
        return;
    }
    emit statusMessage(mode == PlayerMode::Tutorial
                           ? tr("Tutorial: \"%1\" — follow the prompts!").arg(m.file->name)
                           : tr("Performing \"%1\"…").arg(m.file->name),
                       4000);
}

void TransitionPanel::onAbort()
{
    if (player_->isActive()) {
        player_->abort();
        emit statusMessage(tr("Transition aborted"), 3000);
    }
    if (recorder_->isRecording()) {
        recorder_->cancel();
        recIndicator_->clear();
        emit statusMessage(tr("Recording cancelled"), 3000);
    }
    progress_->setValue(0);
    if (banner_) banner_->hide();
}

void TransitionPanel::onProgress(double beatsIn, double beatsTotal)
{
    if (beatsTotal <= 0.0) { progress_->setValue(0); return; }
    progress_->setValue(
        (int)std::lround(std::clamp(beatsIn / beatsTotal, 0.0, 1.0) * 1000.0));
}

void TransitionPanel::onFinished(bool completed)
{
    progress_->setValue(completed ? 1000 : 0);
    if (banner_) banner_->hide();
    emit statusMessage(completed ? tr("Transition complete")
                                 : tr("Transition stopped"),
                       3000);
}

void TransitionPanel::showBanner(const QString& text, const QColor& color,
                                 int timeoutMs)
{
    QWidget* host = window();
    if (!host) return;
    if (!banner_) {
        banner_ = new QLabel(host);
        banner_->setAlignment(Qt::AlignCenter);
        banner_->setWordWrap(true);
        banner_->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    banner_->setStyleSheet(
        QStringLiteral("background:rgba(22,24,29,200); color:%1; "
                       "font-size:26px; font-weight:bold; border-radius:12px; "
                       "border:2px solid %1; padding:18px;")
            .arg(color.name()));
    banner_->setText(text);
    banner_->adjustSize();
    int w = std::max(banner_->width(), host->width() * 2 / 3);
    banner_->resize(w, banner_->heightForWidth(w) > 0
                           ? banner_->heightForWidth(w) + 36
                           : banner_->height());
    banner_->move((host->width() - banner_->width()) / 2,
                  host->height() / 6);
    banner_->raise();
    banner_->show();
    bannerTimer_->start(timeoutMs);
}

void TransitionPanel::onTutorialPrompt(const GvtEvent& e, double beatsAhead)
{
    QString what = QString::fromUtf8(controlName(e.control));
    QString target = e.role == Role::Mixer
                         ? tr("mixer")
                         : (e.role == Role::FromDeck ? tr("outgoing deck")
                                                     : tr("incoming deck"));
    QString text;
    if (controlIsTrigger(e.control))
        text = tr("in %1 beats: %2 %3")
                   .arg(QString::number(beatsAhead, 'f', 0), target, what);
    else
        text = tr("in %1 beats: %2 %3 → %4%")
                   .arg(QString::number(beatsAhead, 'f', 0), target, what,
                        QString::number(e.value * 100.0, 'f', 0));
    showBanner(text, themeText(), 4000);
}

void TransitionPanel::onTutorialScored(const GvtEvent& e, double beatError,
                                       double valueError)
{
    Q_UNUSED(valueError);
    const double abs = std::fabs(beatError);
    QColor color;
    QString verdict;
    if (abs < 0.25) { color = QColor(0x4c, 0xd9, 0x64); verdict = tr("nice!"); }
    else if (abs < 1.0) { color = QColor(0xe8, 0xa8, 0x35); verdict = tr("close"); }
    else { color = QColor(0xe8, 0x55, 0x55); verdict = tr("off beat"); }
    showBanner(QStringLiteral("%1 %2%3 beats, %4")
                   .arg(QString::fromUtf8(controlName(e.control)),
                        beatError >= 0 ? QStringLiteral("+")
                                       : QStringLiteral("−"),
                        QString::number(abs, 'f', 2), verdict),
               color, 1800);
}

} // namespace gvt
