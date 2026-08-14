#include "DeckWidget.h"
#include "Theme.h"

#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gvt {

// Tempo slider: integer -800..800 <-> ratio 0.92..1.08 (+/-8%).
static constexpr int kTempoSteps = 800;
static constexpr double kTempoRange = 0.08;

static int ratioToSlider(double ratio)
{
    double frac = (ratio - 1.0) / kTempoRange; // -1..1
    return (int)std::lround(std::clamp(frac, -1.0, 1.0) * kTempoSteps);
}
static double sliderToRatio(int v)
{
    return 1.0 + (double)v / kTempoSteps * kTempoRange;
}

static QString formatTime(double sec)
{
    if (sec < 0) sec = 0;
    int total = (int)sec;
    return QString::asprintf("%d:%02d.%d", total / 60, total % 60,
                             (int)((sec - total) * 10));
}

// ---------------------------------------------------------------- WaveformView

WaveformView::WaveformView(int deckIndex, Deck* deck, QWidget* parent)
    : QWidget(parent), deckIndex_(deckIndex), deck_(deck)
{
    setMinimumHeight(72);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

void WaveformView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const int w = width(), h = height();
    p.fillRect(rect(), themeBackground().darker(110));

    TrackDataPtr t = deck_->track();
    const QColor accent = deckAccent(deckIndex_);
    if (!t || t->overviewPeaks.empty() || t->durationSec <= 0.0) {
        p.setPen(themeDimText());
        p.drawText(rect(), Qt::AlignCenter, tr("no track loaded"));
        p.setPen(QPen(accent.darker(200), 1));
        p.drawRect(0, 0, w - 1, h - 1);
        return;
    }

    const auto& peaks = t->overviewPeaks;
    const int n = (int)peaks.size();
    const double posFrac =
        std::clamp(deck_->positionSec() / t->durationSec, 0.0, 1.0);
    const int playedX = (int)(posFrac * w);

    // Beatgrid ticks every 4 beats (faint, behind the peaks).
    if (t->bpm > 0.0) {
        p.setPen(QColor(255, 255, 255, 22));
        const double secPer4Beats = 4.0 * 60.0 / t->bpm;
        for (double s = t->firstBeatSec; s < t->durationSec; s += secPer4Beats) {
            int x = (int)(s / t->durationSec * w);
            p.drawLine(x, 0, x, h);
        }
    }

    // Vertical peak bars: max of the bins covered by each pixel column.
    const int mid = h / 2;
    const QColor unplayed(0x6a, 0x70, 0x7d);
    for (int x = 0; x < w; ++x) {
        int i0 = (int)((int64_t)x * n / w);
        int i1 = (int)((int64_t)(x + 1) * n / w);
        i1 = std::max(i1, i0 + 1);
        float peak = 0.0f;
        for (int i = i0; i < i1 && i < n; ++i)
            peak = std::max(peak, peaks[i]);
        int half = std::max(1, (int)(peak * (h / 2 - 3)));
        p.setPen(x <= playedX ? accent : unplayed);
        p.drawLine(x, mid - half, x, mid + half);
    }

    // Hotcue flags.
    p.setFont(QFont(font().family(), 8, QFont::Bold));
    for (int i = 0; i < 8; ++i) {
        if (t->hotCues[i] < 0) continue;
        int x = (int)(t->hotCues[i] / t->durationSec * w);
        QColor flag = accent.lighter(125);
        p.setPen(flag);
        p.drawLine(x, 0, x, h);
        QRect flagRect(x + 1, 1, 12, 12);
        p.fillRect(flagRect, flag);
        p.setPen(Qt::black);
        p.drawText(flagRect, Qt::AlignCenter, QString::number(i + 1));
    }

    // Playhead.
    p.setPen(QPen(Qt::white, 2));
    p.drawLine(playedX, 0, playedX, h);

    p.setPen(QPen(accent.darker(160), 1));
    p.drawRect(0, 0, w - 1, h - 1);
}

void WaveformView::mousePressEvent(QMouseEvent* ev)
{
    TrackDataPtr t = deck_->track();
    if (!t || t->durationSec <= 0.0 || width() <= 0) return;
    double frac = std::clamp(ev->position().x() / width(), 0.0, 1.0);
    deck_->seekSec(frac * t->durationSec); // direct API per contract
    update();
}

// ------------------------------------------------------------------ DeckWidget

DeckWidget::DeckWidget(int deckIndex, ControlBus* bus, AudioEngine* engine,
                       QWidget* parent)
    : QWidget(parent), deckIndex_(deckIndex), bus_(bus), engine_(engine)
{
    const QColor accent = deckAccent(deckIndex_);
    setObjectName(QStringLiteral("deckWidget%1").arg(deckIndex_));
    setProperty("panel", true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* header = new QLabel(deckIndex_ == 0 ? tr("DECK A") : tr("DECK B"));
    header->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                         "letter-spacing:2px;")
                              .arg(accent.name()));
    root->addWidget(header);

    waveform_ = new WaveformView(deckIndex_, &engine_->deck(deckIndex_), this);
    root->addWidget(waveform_);

    titleLabel_ = new QLabel(tr("—"));
    titleLabel_->setStyleSheet("font-size:13px; font-weight:bold;");
    artistLabel_ = new QLabel(QString());
    artistLabel_->setStyleSheet(
        QStringLiteral("color:%1;").arg(themeDimText().name()));
    bpmLabel_ = new QLabel(tr("BPM —"));
    timeLabel_ = new QLabel(tr("0:00.0 / -0:00.0"));
    timeLabel_->setStyleSheet("font-family:monospace;");

    auto* info = new QGridLayout;
    info->setContentsMargins(0, 0, 0, 0);
    info->addWidget(titleLabel_, 0, 0);
    info->addWidget(bpmLabel_, 0, 1, Qt::AlignRight);
    info->addWidget(artistLabel_, 1, 0);
    info->addWidget(timeLabel_, 1, 1, Qt::AlignRight);
    root->addLayout(info);

    // Transport row + vertical tempo slider on the side.
    auto* middle = new QHBoxLayout;
    middle->setSpacing(6);

    auto* transportCol = new QVBoxLayout;
    auto* transport = new QHBoxLayout;
    playBtn_ = new QPushButton(tr("PLAY"));
    playBtn_->setCheckable(true);
    cueBtn_ = new QPushButton(tr("CUE"));
    syncBtn_ = new QPushButton(tr("SYNC"));
    for (QPushButton* b : {playBtn_, cueBtn_, syncBtn_})
        b->setMinimumHeight(34);
    playBtn_->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:black; "
                       "font-weight:bold; }")
            .arg(accent.name()));
    transport->addWidget(playBtn_);
    transport->addWidget(cueBtn_);
    transport->addWidget(syncBtn_);
    transportCol->addLayout(transport);

    // 8 hotcue buttons, 2 rows of 4.
    auto* cues = new QGridLayout;
    cues->setSpacing(4);
    for (int i = 0; i < 8; ++i) {
        auto* b = new QPushButton(QString::number(i + 1));
        b->setFixedHeight(26);
        b->setToolTip(tr("Hot cue %1 — click: set/jump, right- or "
                         "shift-click: clear")
                          .arg(i + 1));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        hotcueBtns_[i] = b;
        cues->addWidget(b, i / 4, i % 4);
        connect(b, &QPushButton::clicked, this,
                [this, i] { onHotCueClicked(i); });
        connect(b, &QPushButton::customContextMenuRequested, this, [this, i] {
            if (TrackDataPtr t = engine_->deck(deckIndex_).track()) {
                t->hotCues[i] = -1; // clear via TrackData hotCues array
                syncHotCueButtons();
                waveform_->update();
            }
        });
    }
    transportCol->addLayout(cues);
    transportCol->addStretch(1);
    middle->addLayout(transportCol, 1);

    auto* tempoCol = new QVBoxLayout;
    tempoLabel_ = new QLabel(tr("+0.0%"));
    tempoLabel_->setAlignment(Qt::AlignHCenter);
    tempoLabel_->setStyleSheet("font-family:monospace; font-size:11px;");
    tempoSlider_ = new QSlider(Qt::Vertical);
    tempoSlider_->setRange(-kTempoSteps, kTempoSteps);
    tempoSlider_->setValue(0);
    tempoSlider_->setTickPosition(QSlider::TicksBothSides);
    tempoSlider_->setTickInterval(kTempoSteps / 4);
    tempoSlider_->setToolTip(tr("Tempo ±8%"));
    tempoCol->addWidget(tempoLabel_);
    tempoCol->addWidget(tempoSlider_, 1, Qt::AlignHCenter);
    auto* tempoCaption = new QLabel(tr("TEMPO"));
    tempoCaption->setAlignment(Qt::AlignHCenter);
    tempoCaption->setStyleSheet(
        QStringLiteral("color:%1; font-size:10px;").arg(themeDimText().name()));
    tempoCol->addWidget(tempoCaption);
    middle->addLayout(tempoCol);

    root->addLayout(middle, 1);

    // --- wiring: user actions -> bus (Origin::Ui) ---
    // toggled (not clicked): fires for mouse, keyboard, and accessibility
    // toggles alike; refresh() blocks signals when mirroring engine state.
    connect(playBtn_, &QPushButton::toggled, this, [this](bool checked) {
        dispatch(checked ? ControlId::Play : ControlId::Stop);
    });
    connect(cueBtn_, &QPushButton::clicked, this,
            [this] { dispatch(ControlId::Cue); });
    connect(syncBtn_, &QPushButton::clicked, this,
            [this] { dispatch(ControlId::TempoSync); });
    connect(tempoSlider_, &QSlider::valueChanged, this,
            &DeckWidget::onTempoSlider);
    connect(tempoSlider_, &QSlider::sliderReleased, this, [this] {
        // Soft center detent: snap when close to 0.
        if (std::abs(tempoSlider_->value()) <= kTempoSteps / 50)
            tempoSlider_->setValue(0);
    });

    // Mirror events from MIDI/Replay (and other widgets) into our controls.
    connect(bus_, &ControlBus::eventDispatched, this, &DeckWidget::onBusEvent);

    timer_ = new QTimer(this);
    timer_->setInterval(33); // ~30 Hz
    connect(timer_, &QTimer::timeout, this, &DeckWidget::refresh);
    timer_->start();

    trackChanged();
}

void DeckWidget::dispatch(ControlId id, double value)
{
    bus_->dispatch(ControlEvent{deckIndex_, id, value}, Origin::Ui);
}

void DeckWidget::onTempoSlider(int value)
{
    double ratio = sliderToRatio(value);
    tempoLabel_->setText(
        QString::asprintf("%+.1f%%", (ratio - 1.0) * 100.0));
    if (!tempoSlider_->signalsBlocked())
        dispatch(ControlId::Tempo, ratio);
    // Effective BPM label refreshes on the timer tick.
}

void DeckWidget::onHotCueClicked(int i)
{
    Deck& deck = engine_->deck(deckIndex_);
    TrackDataPtr t = deck.track();
    if (!t) return;
    const bool shift =
        QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (shift) {
        t->hotCues[i] = -1; // clear via TrackData hotCues array
    } else if (t->hotCues[i] < 0) {
        deck.setHotCue(i);  // direct API per contract
    } else {
        deck.jumpHotCue(i); // direct API per contract
    }
    syncHotCueButtons();
    waveform_->update();
}

void DeckWidget::syncHotCueButtons()
{
    TrackDataPtr t = engine_->deck(deckIndex_).track();
    const QColor accent = deckAccent(deckIndex_);
    for (int i = 0; i < 8; ++i) {
        bool set = t && t->hotCues[i] >= 0;
        hotcueBtns_[i]->setStyleSheet(
            set ? QStringLiteral("background:%1; color:black; font-weight:bold;")
                      .arg(accent.name())
                : QString());
    }
}

void DeckWidget::trackChanged()
{
    TrackDataPtr t = engine_->deck(deckIndex_).track();
    if (t) {
        titleLabel_->setText(t->title.isEmpty() ? t->filePath : t->title);
        artistLabel_->setText(t->artist);
        bpmLabel_->setText(QString::asprintf("BPM %.2f", t->bpm));
    } else {
        titleLabel_->setText(tr("—"));
        artistLabel_->clear();
        bpmLabel_->setText(tr("BPM —"));
    }
    syncHotCueButtons();
    waveform_->update();
    refresh();
}

void DeckWidget::onBusEvent(const ControlEvent& e, Origin origin)
{
    Q_UNUSED(origin);
    if (e.deck != deckIndex_) return;
    switch (e.id) {
    case ControlId::Tempo: {
        QSignalBlocker block(tempoSlider_);
        tempoSlider_->setValue(ratioToSlider(e.value));
        tempoLabel_->setText(
            QString::asprintf("%+.1f%%", (e.value - 1.0) * 100.0));
        break;
    }
    case ControlId::HotCue1: case ControlId::HotCue2:
    case ControlId::HotCue3: case ControlId::HotCue4:
    case ControlId::HotCue5: case ControlId::HotCue6:
    case ControlId::HotCue7: case ControlId::HotCue8:
        syncHotCueButtons();
        waveform_->update();
        break;
    default:
        break; // Play/Stop/etc. are mirrored from engine state on the timer
    }
}

void DeckWidget::refresh()
{
    Deck& deck = engine_->deck(deckIndex_);
    TrackDataPtr t = deck.track();

    const bool playing = deck.playing.load();
    if (playBtn_->isChecked() != playing) {
        QSignalBlocker block(playBtn_);
        playBtn_->setChecked(playing);
    }

    if (t && t->durationSec > 0.0) {
        double pos = deck.positionSec();
        timeLabel_->setText(formatTime(pos) + " / -" +
                            formatTime(t->durationSec - pos));
        double eff = deck.effectiveBpm();
        bpmLabel_->setText(
            QString::asprintf("BPM %.2f → %.2f", t->bpm, eff));
        // Mirror the engine's tempo ratio when we're not dragging.
        if (!tempoSlider_->isSliderDown()) {
            int sv = ratioToSlider(deck.tempoRatio.load());
            if (sv != tempoSlider_->value()) {
                QSignalBlocker block(tempoSlider_);
                tempoSlider_->setValue(sv);
                tempoLabel_->setText(QString::asprintf(
                    "%+.1f%%", (deck.tempoRatio.load() - 1.0) * 100.0));
            }
        }
        if (playing)
            waveform_->update(); // ~30 Hz repaint while playing
    } else {
        timeLabel_->setText(tr("0:00.0 / -0:00.0"));
    }
}

} // namespace gvt
