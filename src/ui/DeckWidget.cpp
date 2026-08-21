#include "DeckWidget.h"
#include "FitButton.h"
#include "Theme.h"

#include <QComboBox>
#include <QDial>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
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

// Base style for the compact loop/beat-jump buttons; the "lit" highlight
// styles append a background to this so the compact metrics survive.
static constexpr const char* kLoopBtnBase =
    "QPushButton { padding: 1px 3px; font-size: 9px; }";

// FxBeats display: "1/4", "1/2", "1", "2", "4".
static QString formatFxBeats(double beats)
{
    if (beats < 0.99) {
        int denom = (int)std::lround(1.0 / beats);
        return QStringLiteral("1/%1").arg(denom);
    }
    return QString::number((int)std::lround(beats));
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
    setFixedHeight(44); // compact Serato-style overview strip
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

void WaveformView::setTransitionEntry(double sec)
{
    transitionEntrySec_ = sec;
    update();
}

void WaveformView::setTransitionCues(const QList<double>& seconds,
                                     const QStringList& labels)
{
    transitionCueSecs_ = seconds;
    transitionCueLabels_ = labels;
    update();
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
    // Band-colored (low/mid/high, same palette as DetailWaveformView) when
    // the analysis filled overviewLow/Mid/High; gray/accent fallback else.
    // The unplayed part is drawn dimmed so progress stays readable.
    const auto& lowB = t->overviewLow;
    const auto& midB = t->overviewMid;
    const auto& highB = t->overviewHigh;
    const bool banded = !lowB.empty() && lowB.size() == midB.size() &&
                        lowB.size() == highB.size();
    const int nb = banded ? (int)lowB.size() : n;
    const int mid = h / 2;
    const QColor unplayed(0x6a, 0x70, 0x7d);
    const QColor lowC = waveLowColor(), midC = waveMidColor(),
                 highC = waveHighColor();
    const QColor lowD = lowC.darker(190), midD = midC.darker(190),
                 highD = highC.darker(190);
    for (int x = 0; x < w; ++x) {
        int i0 = (int)((int64_t)x * nb / w);
        int i1 = (int)((int64_t)(x + 1) * nb / w);
        i1 = std::max(i1, i0 + 1);
        const bool played = x <= playedX;
        if (banded) {
            float l = 0, m = 0, hi = 0;
            for (int i = i0; i < i1 && i < nb; ++i) {
                l = std::max(l, lowB[i]);
                m = std::max(m, midB[i]);
                hi = std::max(hi, highB[i]);
            }
            const int lh = (int)(l * (h / 2 - 3));
            const int mh = (int)(m * (h / 2 - 3));
            const int hh = (int)(hi * (h / 2 - 3));
            if (lh > 0) { // low tallest in back, high in front
                p.setPen(played ? lowC : lowD);
                p.drawLine(x, mid - lh, x, mid + lh);
            }
            if (mh > 0) {
                p.setPen(played ? midC : midD);
                p.drawLine(x, mid - mh, x, mid + mh);
            }
            if (hh > 0) {
                p.setPen(played ? highC : highD);
                p.drawLine(x, mid - hh, x, mid + hh);
            }
        } else {
            float peak = 0.0f;
            for (int i = i0; i < i1 && i < nb; ++i)
                peak = std::max(peak, peaks[i]);
            int half = std::max(1, (int)(peak * (h / 2 - 3)));
            p.setPen(played ? accent : unplayed);
            p.drawLine(x, mid - half, x, mid + half);
        }
    }

    // Loop display.  Before OUT is set, the highlighted range grows from the
    // armed IN marker to the live playhead.  Once complete, the fixed loop
    // range remains shaded and the playhead visibly wraps through it.
    {
        const double ls = deck_->loopStartSec.load();
        const double le = deck_->loopEndSec.load();
        const bool active = deck_->loopActive.load();
        const bool complete = ls >= 0.0 && le > ls;
        const double visibleEnd = complete ? le : std::max(ls, deck_->positionSec());
        if (ls >= 0.0 && visibleEnd > ls && ls < t->durationSec) {
            const int x0 = (int)(ls / t->durationSec * w);
            const int x1 = (int)(std::min(visibleEnd, t->durationSec) /
                                 t->durationSec * w);
            QColor fill = accent;
            fill.setAlpha(active ? 64 : (complete ? 28 : 46));
            p.fillRect(QRect(x0, 0, std::max(1, x1 - x0), h), fill);
        }

        if (ls >= 0.0 && ls <= t->durationSec) {
            const int x = (int)(ls / t->durationSec * w);
            QColor edge = accent.lighter(140);
            edge.setAlpha(active || !complete ? 240 : 130);
            QPen inPen(edge, complete ? 1 : 2);
            if (!complete) inPen.setStyle(Qt::DashLine);
            p.setPen(inPen);
            p.drawLine(x, 0, x, h);

            p.setFont(QFont(font().family(), 7, QFont::Bold));
            QRect tag(std::clamp(x + 1, 0, std::max(0, w - 17)), 1, 17, 10);
            p.fillRect(tag, edge);
            p.setPen(Qt::black);
            p.drawText(tag, Qt::AlignCenter, QStringLiteral("IN"));
        }

        if (complete && le <= t->durationSec) {
            const int x = (int)(le / t->durationSec * w);
            QColor edge = accent.lighter(140);
            edge.setAlpha(active ? 230 : 110);
            p.setPen(QPen(edge, 1));
            p.drawLine(x, 0, x, h);
        }
    }

    // Cue point marker (Deck::cuePointSec) — small accent notch.
    {
        const double cue = deck_->cuePointSec.load();
        if (cue >= 0.0 && cue <= t->durationSec) {
            int x = (int)(cue / t->durationSec * w);
            p.setPen(QPen(accent.lighter(140), 1));
            p.drawLine(x, 0, x, h);
            QPolygon tri;
            tri << QPoint(x - 3, 0) << QPoint(x + 3, 0) << QPoint(x, 5);
            p.setBrush(accent.lighter(140));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            p.setBrush(Qt::NoBrush);
        }
    }

    // Hotcue flags in slot colors.
    p.setFont(QFont(font().family(), 7, QFont::Bold));
    for (int i = 0; i < 8; ++i) {
        if (t->hotCues[i] < 0) continue;
        int x = (int)(t->hotCues[i] / t->durationSec * w);
        QColor flag = hotCueColor(i);
        p.setPen(flag);
        p.drawLine(x, 0, x, h);
        QRect flagRect(x + 1, 1, 10, 10);
        p.fillRect(flagRect, flag);
        p.setPen(Qt::black);
        p.drawText(flagRect, Qt::AlignCenter, QString::number(i + 1));
    }

    // Transition entry marker: orange line + "T" tag.
    if (transitionEntrySec_ >= 0.0 && transitionEntrySec_ <= t->durationSec) {
        int x = (int)(transitionEntrySec_ / t->durationSec * w);
        const QColor c = transitionEntryColor();
        p.setPen(QPen(c, 2));
        p.drawLine(x, 0, x, h);
        QRect tag(x + 2, h - 11, 10, 10);
        p.fillRect(tag, c);
        p.setPen(Qt::black);
        p.drawText(tag, Qt::AlignCenter, QStringLiteral("T"));
    }


    // Selected transition's labeled cue points.  The overview stays compact;
    // full labels are rendered in the center detail waveform.
    const QColor cueColor(0xf1, 0xc7, 0x5b);
    p.setFont(QFont(font().family(), 7, QFont::Bold));
    for (qsizetype i = 0; i < transitionCueSecs_.size(); ++i) {
        const double sec = transitionCueSecs_.at(i);
        if (sec < 0.0 || sec > t->durationSec) continue;
        const int x = (int)(sec / t->durationSec * w);
        p.setPen(QPen(cueColor, 1));
        p.drawLine(x, 0, x, h);
        QRect tag(x + 1, h - 9, 8, 8);
        p.fillRect(tag, cueColor);
        p.setPen(Qt::black);
        p.drawText(tag, Qt::AlignCenter, QStringLiteral("C"));
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
    loadPerformancePadSettings();
    engine_->deck(deckIndex_).quantizeHotCues.store(
        QSettings().value(
            QStringLiteral("decks/%1/quantizeHotCues").arg(deckIndex_), true)
            .toBool(),
        std::memory_order_release);

    // The main column (waveform/info/transport/pads) sits next to a narrow
    // vertical tempo slider on the OUTER edge: left for deck A, right for
    // deck B (mirrored like Serato).
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 4, 6, 4);
    outer->setSpacing(4);

    auto* mainCol = new QVBoxLayout;
    mainCol->setSpacing(3);

    auto* header = new QLabel(deckIndex_ == 0 ? tr("DECK A") : tr("DECK B"));
    header->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                         "letter-spacing:2px;")
                              .arg(accent.name()));
    mainCol->addWidget(header);

    waveform_ = new WaveformView(deckIndex_, &engine_->deck(deckIndex_), this);
    mainCol->addWidget(waveform_);

    titleLabel_ = new QLabel(tr("—"));
    titleLabel_->setStyleSheet("font-size:12px; font-weight:bold;");
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
    mainCol->addLayout(info);

    // Transport row: PLAY, CUE, one-shot phase SYNC, per-deck QUANTIZE.
    auto* transport = new QHBoxLayout;
    playBtn_ = new FitPushButton(tr("PLAY"));
    playBtn_->setCheckable(true);
    cueBtn_ = new FitPushButton(tr("CUE")); // hold-to-preview: NOT checkable
    syncBtn_ = new FitPushButton(tr("SYNC"));
    syncBtn_->setToolTip(
        tr("Align this deck to the other deck's beat phase once (does not change BPM)"));
    quantizeBtn_ = new FitPushButton(tr("QUANT"));
    gridBtn_ = new FitToolButton(this);
    gridBtn_->setText(tr("GRID ▾"));
    gridBtn_->setPopupMode(QToolButton::InstantPopup);
    gridBtn_->setToolTip(tr("Correct this track's beat grid"));
    auto* gridMenu = new QMenu(gridBtn_);
    QAction* setDownbeat = gridMenu->addAction(tr("Set downbeat here"));
    connect(setDownbeat, &QAction::triggered, this, [this] {
        emit beatGridEditRequested(
            deckIndex_, BeatGridCommand::SetDownbeat,
            engine_->deck(deckIndex_).positionSec());
    });
    gridMenu->addSeparator();
    QAction* gridEarlier = gridMenu->addAction(tr("Shift grid earlier 10 ms"));
    connect(gridEarlier, &QAction::triggered, this, [this] {
        emit beatGridEditRequested(
            deckIndex_, BeatGridCommand::Nudge, -0.010);
    });
    QAction* gridLater = gridMenu->addAction(tr("Shift grid later 10 ms"));
    connect(gridLater, &QAction::triggered, this, [this] {
        emit beatGridEditRequested(
            deckIndex_, BeatGridCommand::Nudge, 0.010);
    });
    gridMenu->addSeparator();
    QAction* halfBpm = gridMenu->addAction(tr("BPM ÷ 2"));
    connect(halfBpm, &QAction::triggered, this, [this] {
        emit beatGridEditRequested(
            deckIndex_, BeatGridCommand::HalveBpm, 0.0);
    });
    QAction* doubleBpm = gridMenu->addAction(tr("BPM × 2"));
    connect(doubleBpm, &QAction::triggered, this, [this] {
        emit beatGridEditRequested(
            deckIndex_, BeatGridCommand::DoubleBpm, 0.0);
    });
    QAction* editBpm = gridMenu->addAction(tr("Edit BPM…"));
    connect(editBpm, &QAction::triggered, this, [this] {
        TrackDataPtr track = engine_->deck(deckIndex_).track();
        if (!track) {
            showPadFeedback(tr("Load a track before editing its beat grid"));
            return;
        }
        bool ok = false;
        const double bpm = QInputDialog::getDouble(
            this, tr("Edit beat-grid BPM"), tr("BPM:"), track->bpm,
            20.0, 400.0, 3, &ok, Qt::WindowFlags(), 0.001);
        if (ok) {
            emit beatGridEditRequested(
                deckIndex_, BeatGridCommand::SetBpm, bpm);
        }
    });
    gridBtn_->setMenu(gridMenu);
    quantizeBtn_->setCheckable(true);
    quantizeBtn_->setChecked(
        engine_->deck(deckIndex_).quantizeHotCues.load());
    quantizeBtn_->setToolTip(
        tr("Quantize hot cues and manual loop IN/OUT to whole beats"));
    cueBtn_->setToolTip(
        tr("Hold to preview from cue; press PLAY while held to continue playing"));
    for (QPushButton* b : {playBtn_, cueBtn_, syncBtn_, quantizeBtn_})
        b->setMinimumHeight(26);
    playBtn_->setFixedWidth(48);
    cueBtn_->setFixedWidth(44);
    syncBtn_->setFixedWidth(46);
    quantizeBtn_->setFixedWidth(52);
    gridBtn_->setMinimumHeight(26);
    gridBtn_->setFixedWidth(54);
    playBtn_->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:black; "
                       "font-weight:bold; }")
            .arg(accent.name()));
    quantizeBtn_->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:black; "
                       "font-weight:bold; }")
            .arg(accent.name()));
    transport->addWidget(playBtn_);
    transport->addWidget(cueBtn_);
    transport->addWidget(syncBtn_);
    transport->addWidget(quantizeBtn_);
    transport->addWidget(gridBtn_);
    mainCol->addLayout(transport);

    // Eight FLX4-style performance pads. The normal mode keys remain visible
    // next to the pads; shifted modes are in the SHIFT MODES popup. HOT CUE
    // preserves hold-preview and PLAY-latch semantics from the deck engine.
    auto* padsSection = new QVBoxLayout;
    padsSection->setSpacing(2);
    auto* padsAndModes = new QHBoxLayout;
    padsAndModes->setSpacing(6);
    auto* cues = new QGridLayout;
    cues->setSpacing(3);
    for (int i = 0; i < 8; ++i) {
        auto* b = new FitPushButton(QString::number(i + 1));
        b->setFixedSize(48, 28);
        b->setFocusPolicy(Qt::NoFocus);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        hotcueBtns_[i] = b;
        cues->addWidget(b, i / 4, i % 4, Qt::AlignLeft);
        connect(b, &QPushButton::pressed, this,
                [this, i] { handlePerformancePad(i, true); });
        connect(b, &QPushButton::released, this,
                [this, i] { handlePerformancePad(i, false); });
        connect(b, &QPushButton::customContextMenuRequested, this,
                [this, b, i](const QPoint& pos) {
            configurePerformancePad(i, b->mapToGlobal(pos));
        });
    }
    padsAndModes->addLayout(cues);

    auto* modes = new QGridLayout;
    modes->setSpacing(2);
    static constexpr PerformancePadMode kNormalModes[4] = {
        PerformancePadMode::HotCue, PerformancePadMode::PadFx1,
        PerformancePadMode::BeatJump, PerformancePadMode::Sampler};
    for (int i = 0; i < 4; ++i) {
        auto* b = new FitPushButton(
            QLatin1String(performancePadModeLabel(kNormalModes[i])), this);
        b->setFixedHeight(24);
        b->setFixedWidth(58);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tr("Select %1 performance pads")
                          .arg(QLatin1String(
                              performancePadModeLabel(kNormalModes[i]))));
        const PerformancePadMode mode = kNormalModes[i];
        connect(b, &QPushButton::clicked, this,
                [this, mode] { setPerformancePadMode(mode); });
        normalModeBtns_[i] = b;
        modes->addWidget(b, i / 2, i % 2);
    }

    shiftedModesBtn_ = new FitToolButton(this);
    shiftedModesBtn_->setText(tr("SHIFT MODES ▾"));
    shiftedModesBtn_->setPopupMode(QToolButton::InstantPopup);
    shiftedModesBtn_->setToolTip(
        tr("Show KEYBOARD, PAD FX2, BEAT LOOP, and KEY SHIFT"));
    shiftedModesBtn_->setFocusPolicy(Qt::NoFocus);
    shiftedModesBtn_->setFixedHeight(22);
    auto* shiftedMenu = new QMenu(shiftedModesBtn_);
    static constexpr PerformancePadMode kShiftModes[4] = {
        PerformancePadMode::Keyboard, PerformancePadMode::PadFx2,
        PerformancePadMode::BeatLoop, PerformancePadMode::KeyShift};
    for (const PerformancePadMode mode : kShiftModes) {
        QAction* action = shiftedMenu->addAction(
            QLatin1String(performancePadModeLabel(mode)));
        connect(action, &QAction::triggered, this,
                [this, mode] { setPerformancePadMode(mode); });
    }
    shiftedModesBtn_->setMenu(shiftedMenu);
    modes->addWidget(shiftedModesBtn_, 2, 0, 1, 2);
    padsAndModes->addLayout(modes);
    padsAndModes->addStretch(1);
    padsSection->addLayout(padsAndModes);

    padStatusLabel_ = new QLabel(this);
    padStatusLabel_->setMinimumHeight(13);
    padStatusLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:9px;").arg(themeDimText().name()));
    padsSection->addWidget(padStatusLabel_);
    mainCol->addLayout(padsSection);
    syncPerformancePadUi();

    // Loop / beat-jump section: one compact row below the hot cues.
    // [1/2][1][2][4][8] auto-loop · [IN][OUT][EXIT] manual · [<½][2×>]
    // resize · [◀8][◀4][◀1][1▶][4▶][8▶] beat jump. All fire on press via
    // the bus; active loop length + IN/OUT state highlighted from refresh().
    auto* loopRow = new QHBoxLayout;
    loopRow->setSpacing(2);
    auto mkLoopBtn = [&](const QString& text, const QString& tip) {
        auto* b = new FitPushButton(text, this);
        b->setFixedHeight(18);
        b->setMinimumWidth(24);
        b->setStyleSheet(QString::fromLatin1(kLoopBtnBase));
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        loopRow->addWidget(b);
        return b;
    };
    auto mkGroupLabel = [&](const QString& text) {
        auto* l = new QLabel(text, this);
        l->setStyleSheet(QStringLiteral("color:%1; font-size:8px;")
                             .arg(themeDimText().name()));
        loopRow->addWidget(l);
    };
    mkGroupLabel(tr("LOOP"));
    static const char* kAutoTexts[5] = {"1/2", "1", "2", "4", "8"};
    for (int i = 0; i < 5; ++i) {
        const double beats = kAutoLoopBeats[i];
        autoLoopBtns_[i] = mkLoopBtn(
            QLatin1String(kAutoTexts[i]),
            tr("Auto loop %1 beat(s)").arg(beats));
        connect(autoLoopBtns_[i], &QPushButton::pressed, this,
                [this, beats] { dispatch(ControlId::LoopAuto, beats); });
    }
    loopRow->addSpacing(5);
    loopInBtn_ = mkLoopBtn(tr("IN"), tr("Set loop in point"));
    loopOutBtn_ = mkLoopBtn(tr("OUT"), tr("Set loop out point + activate"));
    loopExitBtn_ = mkLoopBtn(tr("EXIT"), tr("Exit the active loop"));
    connect(loopInBtn_, &QPushButton::pressed, this,
            [this] { dispatch(ControlId::LoopIn); });
    connect(loopOutBtn_, &QPushButton::pressed, this,
            [this] { dispatch(ControlId::LoopOut); });
    connect(loopExitBtn_, &QPushButton::pressed, this,
            [this] { dispatch(ControlId::LoopExit); });
    loopRow->addSpacing(5);
    auto* halveBtn = mkLoopBtn(QStringLiteral("<½"), tr("Halve loop length"));
    auto* doubleBtn = mkLoopBtn(QStringLiteral("2×>"),
                                tr("Double loop length"));
    connect(halveBtn, &QPushButton::pressed, this,
            [this] { dispatch(ControlId::LoopHalve); });
    connect(doubleBtn, &QPushButton::pressed, this,
            [this] { dispatch(ControlId::LoopDouble); });
    loopRow->addSpacing(7);
    mkGroupLabel(tr("JUMP"));
    static const struct { const char* text; double beats; } kJumps[6] = {
        {"◀8", -8}, {"◀4", -4}, {"◀1", -1},
        {"1▶", 1},  {"4▶", 4},  {"8▶", 8},
    };
    for (const auto& j : kJumps) {
        auto* b = mkLoopBtn(QString::fromUtf8(j.text),
                            tr("Beat jump %1 beats").arg(j.beats));
        const double beats = j.beats;
        connect(b, &QPushButton::pressed, this,
                [this, beats] { dispatch(ControlId::BeatJump, beats); });
    }
    loopRow->addStretch(1);
    mainCol->addLayout(loopRow);

    // FX strip: one compact ~20 px row below the loop/jump row.
    // FX [ECHO|REVERB|FLANGER] [ON] WET(dial) BEATS [<][1/2][>]
    // All dispatch through the bus (Origin::Ui); state mirrors the deck's
    // fx* atomics on the 30 Hz refresh under QSignalBlocker.
    auto* fxRow = new QHBoxLayout;
    fxRow->setSpacing(2);
    auto mkFxLabel = [&](const QString& text) {
        auto* l = new QLabel(text, this);
        l->setStyleSheet(QStringLiteral("color:%1; font-size:8px;")
                             .arg(themeDimText().name()));
        fxRow->addWidget(l);
        return l;
    };
    mkFxLabel(tr("FX"));
    fxTypeCombo_ = new QComboBox(this);
    fxTypeCombo_->addItems({tr("ECHO"), tr("REVERB"), tr("FLANGER")});
    fxTypeCombo_->setFixedHeight(18);
    fxTypeCombo_->setStyleSheet(
        QStringLiteral("QComboBox { font-size: 9px; padding: 0px 4px; }"));
    fxTypeCombo_->setFocusPolicy(Qt::NoFocus);
    fxTypeCombo_->setToolTip(tr("FX type"));
    fxRow->addWidget(fxTypeCombo_);
    connect(fxTypeCombo_, &QComboBox::activated, this,
            [this](int index) { dispatch(ControlId::FxType, index); });

    fxOnBtn_ = new FitPushButton(tr("ON"), this);
    fxOnBtn_->setCheckable(true);
    fxOnBtn_->setFixedHeight(18);
    fxOnBtn_->setMinimumWidth(26);
    fxOnBtn_->setStyleSheet(QString::fromLatin1(kLoopBtnBase));
    fxOnBtn_->setFocusPolicy(Qt::NoFocus);
    fxOnBtn_->setToolTip(tr("Engage / disengage the deck FX"));
    fxRow->addWidget(fxOnBtn_);
    connect(fxOnBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (!fxOnBtn_->signalsBlocked())
            dispatch(ControlId::FxOn, checked ? 1.0 : 0.0);
    });

    fxRow->addSpacing(4);
    mkFxLabel(tr("WET"));
    fxWetDial_ = new QDial(this);
    fxWetDial_->setRange(0, 100);
    fxWetDial_->setValue(50);
    fxWetDial_->setWrapping(false);
    fxWetDial_->setNotchesVisible(false);
    fxWetDial_->setFixedSize(20, 20);
    fxWetDial_->setFocusPolicy(Qt::NoFocus);
    fxWetDial_->setToolTip(tr("FX dry/wet"));
    fxRow->addWidget(fxWetDial_);
    connect(fxWetDial_, &QDial::valueChanged, this, [this](int v) {
        if (!fxWetDial_->signalsBlocked())
            dispatch(ControlId::FxWet, v / 100.0);
    });

    fxRow->addSpacing(4);
    mkFxLabel(tr("BEATS"));
    auto mkBeatsBtn = [&](const QString& text, const QString& tip) {
        auto* b = new FitPushButton(text, this);
        b->setFixedHeight(18);
        b->setFixedWidth(18);
        b->setStyleSheet(QString::fromLatin1(kLoopBtnBase));
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        fxRow->addWidget(b);
        return b;
    };
    auto* beatsDn = mkBeatsBtn(QStringLiteral("<"), tr("Halve FX beats"));
    fxBeatsLabel_ = new QLabel(QStringLiteral("1/2"), this);
    fxBeatsLabel_->setAlignment(Qt::AlignCenter);
    fxBeatsLabel_->setMinimumWidth(22);
    fxBeatsLabel_->setStyleSheet(
        QStringLiteral("font-family:monospace; font-size:9px;"));
    fxRow->addWidget(fxBeatsLabel_);
    auto* beatsUp = mkBeatsBtn(QStringLiteral(">"), tr("Double FX beats"));
    auto dispatchBeats = [this](double factor) {
        const double cur = engine_->deck(deckIndex_).fxBeats.load();
        const double next = std::clamp(cur * factor, 0.25, 4.0);
        dispatch(ControlId::FxBeats, next);
        fxBeatsLabel_->setText(formatFxBeats(next)); // instant; refresh mirrors
    };
    connect(beatsDn, &QPushButton::pressed, this,
            [dispatchBeats] { dispatchBeats(0.5); });
    connect(beatsUp, &QPushButton::pressed, this,
            [dispatchBeats] { dispatchBeats(2.0); });
    fxRow->addStretch(1);
    mainCol->addLayout(fxRow);

    // STEMS row: [STEMS] request button + four checkable pads
    // [VOCAL][MELODY][BASS][DRUMS] + stage/status label. States are driven
    // by MainWindow (setStemsIdle/InProgress/Ready) from StemSeparator
    // signals; pad toggles dispatch stem levels via the bus, and refresh()
    // mirrors the deck's stem* atomics back under QSignalBlocker.
    auto* stemsRow = new QHBoxLayout;
    stemsRow->setSpacing(2);
    {
        auto* l = new QLabel(tr("STEMS"), this);
        l->setStyleSheet(QStringLiteral("color:%1; font-size:8px;")
                             .arg(themeDimText().name()));
        stemsRow->addWidget(l);
    }
    stemsBtn_ = new FitPushButton(tr("STEMS"), this);
    stemsBtn_->setFixedHeight(18);
    stemsBtn_->setStyleSheet(QString::fromLatin1(kLoopBtnBase));
    stemsBtn_->setFocusPolicy(Qt::NoFocus);
    stemsBtn_->setToolTip(tr("Separate this track into stems (demucs — "
                             "takes a few minutes; cached afterwards)"));
    stemsRow->addWidget(stemsBtn_);
    connect(stemsBtn_, &QPushButton::clicked, this, [this] {
        if (stemsState_ == StemsState::Idle)
            emit stemsRequested(deckIndex_);
    });
    stemsRow->addSpacing(4);
    static const struct {
        const char* text;
        const char* color; // Serato stem pad colors
        ControlId id;
    } kStemPads[4] = {
        {"VOCAL", "#38c9b8", ControlId::StemVocals},
        {"MELODY", "#e8a13a", ControlId::StemMelody},
        {"BASS", "#7a5ae8", ControlId::StemBass},
        {"DRUMS", "#e05a8a", ControlId::StemDrums},
    };
    for (int i = 0; i < 4; ++i) {
        auto* b = new FitPushButton(QLatin1String(kStemPads[i].text), this);
        b->setCheckable(true);
        b->setChecked(true);   // all stems audible by default
        b->setEnabled(false);  // until separation finishes
        b->setFixedHeight(18);
        b->setMinimumWidth(38);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(QString::fromLatin1(kLoopBtnBase) +
                         QStringLiteral("QPushButton:checked { background:%1; "
                                        "color:black; font-weight:bold; }")
                             .arg(QLatin1String(kStemPads[i].color)));
        const ControlId id = kStemPads[i].id;
        connect(b, &QPushButton::toggled, this, [this, b, id](bool checked) {
            if (!b->signalsBlocked())
                dispatch(id, checked ? 1.0 : 0.0);
        });
        stemPads_[i] = b;
        stemsRow->addWidget(b);
    }
    stemsStatusLabel_ = new QLabel(this);
    stemsStatusLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:9px;").arg(themeDimText().name()));
    stemsRow->addSpacing(4);
    stemsRow->addWidget(stemsStatusLabel_);
    stemsRow->addStretch(1);
    mainCol->addLayout(stemsRow);
    setStemsIdle();
    mainCol->addStretch(1);

    // Narrow vertical tempo slider column.
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

    // Mirror: tempo on the outer edge.
    if (deckIndex_ == 0) {
        outer->addLayout(tempoCol);
        outer->addLayout(mainCol, 1);
    } else {
        outer->addLayout(mainCol, 1);
        outer->addLayout(tempoCol);
    }

    // --- wiring: user actions -> bus (Origin::Ui) ---
    // toggled (not clicked): fires for mouse, keyboard, and accessibility
    // toggles alike; refresh() blocks signals when mirroring engine state.
    connect(playBtn_, &QPushButton::toggled, this, [this](bool checked) {
        Deck& deck = engine_->deck(deckIndex_);
        if (deck.previewActive()) {
            // The preview already made the transport look checked. A user's
            // PLAY click is a takeover gesture, not a request to stop.
            if (!checked) {
                QSignalBlocker block(playBtn_);
                playBtn_->setChecked(true);
            }
            dispatch(ControlId::Play);
            return;
        }
        dispatch(checked ? ControlId::Play : ControlId::Stop);
    });
    // CUE: press 1.0 / release 0.0 — hold-to-preview lives in the engine.
    connect(cueBtn_, &QPushButton::pressed, this,
            [this] { dispatch(ControlId::Cue, 1.0); });
    connect(cueBtn_, &QPushButton::released, this,
            [this] { dispatch(ControlId::Cue, 0.0); });
    connect(syncBtn_, &QPushButton::clicked, this,
            [this] { dispatch(ControlId::TempoSync); });
    connect(quantizeBtn_, &QPushButton::toggled, this, [this](bool enabled) {
        dispatch(ControlId::Quantize, enabled ? 1.0 : 0.0);
    });
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

void DeckWidget::setTransitionEntry(double sec)
{
    waveform_->setTransitionEntry(sec);
}

void DeckWidget::setTransitionCues(const QList<double>& seconds,
                                   const QStringList& labels)
{
    waveform_->setTransitionCues(seconds, labels);
}

QWidget* DeckWidget::controlWidget(ControlId control) const
{
    return control == ControlId::Tempo ? tempoSlider_ : nullptr;
}

void DeckWidget::loadPerformancePadSettings()
{
    QSettings settings;
    for (int modeIndex = 0; modeIndex < kPerformanceModeCount; ++modeIndex) {
        const auto mode = static_cast<PerformancePadMode>(modeIndex);
        for (int pad = 0; pad < kPerformancePadCount; ++pad) {
            PerformancePadAssignment assignment =
                defaultPerformancePadAssignment(mode, pad);
            const QString base = QStringLiteral("performancePads/deck%1/%2/pad%3/")
                .arg(deckIndex_)
                .arg(QLatin1String(performancePadModeKey(mode)))
                .arg(pad + 1);
            assignment.value = settings.value(base + QStringLiteral("value"),
                                                assignment.value).toDouble();
            assignment.fxType = settings.value(base + QStringLiteral("fxType"),
                                                 assignment.fxType).toInt();
            assignment.fxWet = settings.value(base + QStringLiteral("fxWet"),
                                                assignment.fxWet).toDouble();
            assignment.fxBeats = settings.value(base + QStringLiteral("fxBeats"),
                                                  assignment.fxBeats).toDouble();
            assignment.label = settings.value(
                base + QStringLiteral("label"),
                QString::fromStdString(assignment.label)).toString().toStdString();
            assignment.resource = settings.value(
                base + QStringLiteral("resource"),
                QString::fromStdString(assignment.resource)).toString().toStdString();
            padAssignments_[modeIndex][pad] =
                sanitizePerformancePadAssignment(mode, pad, assignment);
        }
    }

    const int storedMode = settings.value(
        QStringLiteral("performancePads/deck%1/selectedMode").arg(deckIndex_),
        static_cast<int>(PerformancePadMode::HotCue)).toInt();
    if (storedMode >= 0 && storedMode < kPerformanceModeCount) {
        padMode_ = static_cast<PerformancePadMode>(storedMode);
        // The former host-only SAVED LOOP bank now shares the FLX4's real
        // SAMPLER bank. Preserve the user's selected function across upgrade.
        if (padMode_ == PerformancePadMode::SavedLoop)
            padMode_ = PerformancePadMode::Sampler;
    }
}

void DeckWidget::savePerformancePadAssignment(PerformancePadMode mode, int pad)
{
    if (pad < 0 || pad >= kPerformancePadCount) return;
    const int modeIndex = static_cast<int>(mode);
    if (modeIndex < 0 || modeIndex >= kPerformanceModeCount) return;

    const auto& assignment = padAssignments_[modeIndex][pad];
    const QString base = QStringLiteral("performancePads/deck%1/%2/pad%3/")
        .arg(deckIndex_)
        .arg(QLatin1String(performancePadModeKey(mode)))
        .arg(pad + 1);
    QSettings settings;
    settings.setValue(base + QStringLiteral("value"), assignment.value);
    settings.setValue(base + QStringLiteral("fxType"), assignment.fxType);
    settings.setValue(base + QStringLiteral("fxWet"), assignment.fxWet);
    settings.setValue(base + QStringLiteral("fxBeats"), assignment.fxBeats);
    settings.setValue(base + QStringLiteral("label"),
                      QString::fromStdString(assignment.label));
    settings.setValue(base + QStringLiteral("resource"),
                      QString::fromStdString(assignment.resource));
}

void DeckWidget::savePerformancePadMode()
{
    QSettings().setValue(
        QStringLiteral("performancePads/deck%1/selectedMode").arg(deckIndex_),
        static_cast<int>(padMode_));
}

void DeckWidget::setPerformancePadMode(PerformancePadMode mode)
{
    if (mode == PerformancePadMode::SavedLoop)
        mode = PerformancePadMode::Sampler;
    const int modeIndex = static_cast<int>(mode);
    if (modeIndex < 0 || modeIndex >= kPerformanceModeCount)
        return;

    if (mode == padMode_) {
        syncPerformancePadUi();
        return;
    }

    // A mode change must never strand a held hot-cue preview or a temporary
    // FX insert. Release each pressed pad using the mode it started in.
    for (int pad = 0; pad < kPerformancePadCount; ++pad) {
        if (padIsPressed_[pad]) handlePerformancePad(pad, false);
    }

    padMode_ = mode;
    savePerformancePadMode();
    syncPerformancePadUi();

    const QString modeLabel =
        QLatin1String(performancePadModeLabel(padMode_));
    if (padMode_ == PerformancePadMode::Sampler) {
        showPadFeedback(
            tr("CUSTOM · empty pad captures the active loop; filled loop "
               "starts it; custom audio files remain programmable"));
    } else if (padMode_ == PerformancePadMode::Keyboard ||
               padMode_ == PerformancePadMode::KeyShift) {
        showPadFeedback(tr("%1 is programmable; pitch-shift audio is not available yet")
                            .arg(modeLabel));
    } else {
        showPadFeedback(tr("%1 · right-click a pad to program it").arg(modeLabel));
    }
}

void DeckWidget::triggerPerformancePad(
    PerformancePadMode mode, int pad, bool pressed)
{
    if (pad < 0 || pad >= kPerformancePadCount)
        return;
    // A late release belongs to the layer in which the pad was pressed; it
    // must not switch the UI back after the user selected another layer.
    if (pressed && mode != padMode_)
        setPerformancePadMode(mode);
    handlePerformancePad(pad, pressed);
}

void DeckWidget::clearHotCue(int pad)
{
    if (pad < 0 || pad >= kPerformancePadCount)
        return;
    if (TrackDataPtr track = engine_->deck(deckIndex_).track()) {
        track->hotCues[pad] = -1.0;
        emit trackPerformanceMetadataChanged(deckIndex_);
        syncPerformancePadUi();
        waveform_->update();
        showPadFeedback(tr("Removed hot cue %1").arg(pad + 1));
    }
}

void DeckWidget::requestHotCueClear(int pad)
{
    if (pad < 0 || pad >= kPerformancePadCount)
        return;
    emit hotCueRemovalRequested(deckIndex_, pad);
}

void DeckWidget::dispatchPerformancePadGesture(PerformancePadMode mode, int pad)
{
    if (pad < 0 || pad >= kPerformancePadCount)
        return;
    const auto control = static_cast<ControlId>(
        static_cast<int>(ControlId::PerformancePad1) + pad);
    // The value carries the selected layer. AudioEngine ignores these host-only
    // controls; TransitionRecorder consumes the event as a physical-gesture
    // hint for the immediately following audible state change.
    dispatch(control, static_cast<double>(static_cast<int>(mode)));
}

unsigned int DeckWidget::performancePadLedMask(
    PerformancePadMode mode) const
{
    const int modeIndex = static_cast<int>(mode);
    if (modeIndex < 0 || modeIndex >= kPerformanceModeCount)
        return 0;

    const TrackDataPtr track = engine_->deck(deckIndex_).track();
    unsigned int mask = 0;
    for (int pad = 0; pad < kPerformancePadCount; ++pad) {
        const auto& assignment = padAssignments_[modeIndex][pad];
        bool enabled = false;
        switch (assignment.action) {
        case PerformancePadAction::HotCue:
            enabled = track && track->hotCues[pad] >= 0.0;
            break;
        case PerformancePadAction::SamplerSlot:
            enabled = !assignment.resource.empty() ||
                      (track && track->savedLoops[pad].isSet());
            break;
        case PerformancePadAction::SavedLoop:
            enabled = track && track->savedLoops[pad].isSet();
            break;
        case PerformancePadAction::FxHold:
        case PerformancePadAction::BeatJump:
        case PerformancePadAction::BeatLoop:
        case PerformancePadAction::KeyboardNote:
        case PerformancePadAction::KeyShift:
            enabled = track != nullptr;
            break;
        }
        if (enabled)
            mask |= 1U << static_cast<unsigned int>(pad);
    }
    return mask;
}

unsigned int DeckWidget::performancePadPressedMask() const
{
    unsigned int mask = 0;
    for (int pad = 0; pad < kPerformancePadCount; ++pad) {
        if (padIsPressed_[pad])
            mask |= 1U << static_cast<unsigned int>(pad);
    }
    return mask;
}

void DeckWidget::beatGridChanged()
{
    syncLoopButtons();
    waveform_->update();
    refresh();
}

void DeckWidget::performanceMetadataChanged()
{
    syncPerformancePadUi();
    waveform_->update();
}

void DeckWidget::syncPerformancePadUi()
{
    const QColor accent = deckAccent(deckIndex_);
    const QString modeBase = QStringLiteral(
        "QPushButton { padding:1px 4px; font-size:8px; }");
    const QString modeLit = modeBase + QStringLiteral(
        "QPushButton { background:%1; color:black; font-weight:bold; }")
        .arg(accent.name());
    static constexpr PerformancePadMode kNormalModes[4] = {
        PerformancePadMode::HotCue, PerformancePadMode::PadFx1,
        PerformancePadMode::BeatJump, PerformancePadMode::Sampler};
    for (int i = 0; i < 4; ++i) {
        if (normalModeBtns_[i])
            normalModeBtns_[i]->setStyleSheet(
                padMode_ == kNormalModes[i] ? modeLit : modeBase);
    }

    if (shiftedModesBtn_) {
        const bool shifted = performancePadModeIsShifted(padMode_);
        shiftedModesBtn_->setText(
            shifted
                ? tr("SHIFT: %1 ▾").arg(
                      QLatin1String(performancePadModeLabel(padMode_)))
                : tr("SHIFT MODES ▾"));
        shiftedModesBtn_->setStyleSheet(
            shifted
                ? QStringLiteral("QToolButton { background:%1; color:black; "
                                 "font-size:8px; font-weight:bold; }")
                      .arg(accent.name())
                : QStringLiteral("QToolButton { font-size:8px; }"));
    }

    const int modeIndex = static_cast<int>(padMode_);
    TrackDataPtr track = engine_->deck(deckIndex_).track();
    const bool hasTrack = track != nullptr;
    for (int pad = 0; pad < kPerformancePadCount; ++pad) {
        QPushButton* button = hotcueBtns_[pad];
        if (!button) continue;
        const auto& assignment = padAssignments_[modeIndex][pad];
        button->setText(QString::fromStdString(assignment.label).left(8));
        // Keep right-click programming available without a loaded track.
        // Left-click execution performs its own track guard with feedback.
        button->setEnabled(true);

        QString color;
        QString tooltip;
        switch (assignment.action) {
        case PerformancePadAction::HotCue: {
            const bool set = track && track->hotCues[pad] >= 0.0;
            if (set) color = hotCueColor(pad).name();
            tooltip = tr("Hot cue %1 — hold: preview, PLAY: continue; "
                         "SHIFT-click or right-click: remove")
                          .arg(pad + 1);
            break;
        }
        case PerformancePadAction::FxHold: {
            static constexpr const char* names[3] = {"ECHO", "REVERB", "FLANGER"};
            static constexpr const char* colors[3] = {"#38c9b8", "#a873e8", "#e8a13a"};
            color = QLatin1String(colors[std::clamp(assignment.fxType, 0, 2)]);
            tooltip = tr("Hold %1 at %2% wet, %3 beat(s); release restores the prior FX. "
                         "Right-click to program")
                .arg(QLatin1String(names[std::clamp(assignment.fxType, 0, 2)]))
                .arg(qRound(assignment.fxWet * 100.0))
                .arg(assignment.fxBeats);
            break;
        }
        case PerformancePadAction::BeatJump:
            color = QStringLiteral("#3977e8");
            tooltip = tr("Jump %1 beat(s), beat-aligned. Right-click to program")
                          .arg(assignment.value);
            break;
        case PerformancePadAction::BeatLoop:
            color = QStringLiteral("#e8a13a");
            tooltip = tr("Start a %1-beat loop. Right-click to program")
                          .arg(assignment.value);
            break;
        case PerformancePadAction::SavedLoop: {
            const SavedLoopSlot* slot = track ? &track->savedLoops[pad] : nullptr;
            const bool set = slot && slot->isSet();
            if (set) {
                color = QStringLiteral("#e8a13a");
                button->setText(slot->label.isEmpty()
                    ? tr("L%1").arg(pad + 1) : slot->label.left(8));
                tooltip = tr("Hold captured loop %1 (%2–%3 s) to preview; "
                             "press PLAY while held to continue. Use LOOP EXIT to leave it. "
                             "Right-click to rename, replace, or clear")
                              .arg(pad + 1)
                              .arg(slot->startSec, 0, 'f', 2)
                              .arg(slot->endSec, 0, 'f', 2);
            } else {
                tooltip = tr("Empty CUSTOM slot %1; activate a loop, then press to capture it")
                              .arg(pad + 1);
            }
            break;
        }
        case PerformancePadAction::SamplerSlot: {
            const SavedLoopSlot* slot = track ? &track->savedLoops[pad] : nullptr;
            if (slot && slot->isSet()) {
                color = QStringLiteral("#e8a13a");
                button->setText(slot->label.isEmpty()
                    ? tr("L%1").arg(pad + 1) : slot->label.left(8));
                tooltip = tr("Hold captured loop %1 (%2–%3 s) to preview; press PLAY "
                             "while held to continue. Use LOOP EXIT to leave it. "
                             "Right-click to edit the CUSTOM assignment")
                              .arg(pad + 1)
                              .arg(slot->startSec, 0, 'f', 2)
                              .arg(slot->endSec, 0, 'f', 2);
            } else {
                color = QStringLiteral("#d15a96");
                tooltip = assignment.resource.empty()
                    ? tr("Empty CUSTOM slot; activate a loop and press to capture it, "
                         "or right-click to assign audio")
                    : tr("Custom audio assigned: %1 (sample playback is not available yet); "
                         "an active loop can still be saved here")
                          .arg(QString::fromStdString(assignment.resource));
            }
            break;
        }
        case PerformancePadAction::KeyboardNote:
            color = QStringLiteral("#7a5ae8");
            tooltip = tr("Programmed note %1 (pitch-play engine unavailable); right-click to change")
                          .arg(assignment.value);
            break;
        case PerformancePadAction::KeyShift:
            color = QStringLiteral("#65b4df");
            tooltip = tr("Programmed key shift %1 semitone(s) (pitch engine unavailable); "
                         "right-click to change")
                          .arg(assignment.value);
            break;
        }
        button->setToolTip(tooltip);

        if (color.isEmpty()) {
            button->setStyleSheet(QStringLiteral(
                "QPushButton { font-size:9px; font-weight:bold; }"));
        } else {
            button->setStyleSheet(QStringLiteral(
                "QPushButton { border:1px solid %1; color:%1; font-size:9px; "
                "font-weight:bold; background:%2; } "
                "QPushButton:pressed { background:%1; color:black; }")
                .arg(color, QColor(color).darker(330).name()));
        }
    }

    emit performancePadStateChanged(
        deckIndex_, static_cast<int>(padMode_),
        performancePadLedMask(padMode_), performancePadPressedMask());
}

void DeckWidget::handlePerformancePad(int pad, bool pressed)
{
    if (pad < 0 || pad >= kPerformancePadCount) return;

    if (!pressed) {
        if (!padIsPressed_[pad]) return;
        const PerformancePadMode pressedMode = pressedPadModes_[pad];
        const auto& assignment =
            padAssignments_[static_cast<int>(pressedMode)][pad];
        if (assignment.action == PerformancePadAction::HotCue) {
            dispatchPerformancePadGesture(pressedMode, pad);
            const auto id = static_cast<ControlId>(
                static_cast<int>(ControlId::HotCue1) + pad);
            dispatch(id, 0.0);
        } else if (assignment.action == PerformancePadAction::FxHold) {
            dispatchPerformancePadGesture(pressedMode, pad);
            endPadFx(pad);
        } else if (assignment.action == PerformancePadAction::SavedLoop ||
                   assignment.action == PerformancePadAction::SamplerSlot) {
            if (padReleasePending_[pad]) {
                dispatchPerformancePadGesture(pressedMode, pad);
                const auto id = static_cast<ControlId>(
                    static_cast<int>(ControlId::SavedLoop1) + pad);
                dispatch(id, 0.0);
            }
        }
        padReleasePending_[pad] = false;
        padIsPressed_[pad] = false;
        syncPerformancePadUi();
        return;
    }

    if (padIsPressed_[pad]) return;
    const auto& assignment =
        padAssignments_[static_cast<int>(padMode_)][pad];
    TrackDataPtr track = engine_->deck(deckIndex_).track();
    if (!track && performancePadActionIsSupported(assignment.action)) {
        showPadFeedback(tr("Load a track before using performance pads"));
        return;
    }
    if (assignment.action == PerformancePadAction::HotCue &&
        QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
        requestHotCueClear(pad);
        return;
    }

    padIsPressed_[pad] = true;
    padReleasePending_[pad] = false;
    pressedPadModes_[pad] = padMode_;
    switch (assignment.action) {
    case PerformancePadAction::HotCue: {
        dispatchPerformancePadGesture(padMode_, pad);
        const bool wasSet = track->hotCues[pad] >= 0.0;
        const auto id = static_cast<ControlId>(
            static_cast<int>(ControlId::HotCue1) + pad);
        dispatch(id, 1.0);
        if (!wasSet && track->hotCues[pad] >= 0.0)
            emit trackPerformanceMetadataChanged(deckIndex_);
        waveform_->update();
        break;
    }
    case PerformancePadAction::FxHold:
        dispatchPerformancePadGesture(padMode_, pad);
        beginPadFx(pad, assignment);
        break;
    case PerformancePadAction::BeatJump:
        dispatchPerformancePadGesture(padMode_, pad);
        dispatch(ControlId::BeatJump, assignment.value);
        break;
    case PerformancePadAction::BeatLoop:
        dispatchPerformancePadGesture(padMode_, pad);
        dispatch(ControlId::LoopAuto, assignment.value);
        waveform_->update();
        break;
    case PerformancePadAction::SavedLoop:
    case PerformancePadAction::SamplerSlot: {
        Deck& deck = engine_->deck(deckIndex_);
        SavedLoopSlot& slot = track->savedLoops[pad];
        if (!slot.isSet()) {
            const double start = deck.loopStartSec.load();
            const double end = deck.loopEndSec.load();
            if (!deck.loopActive.load() || !std::isfinite(start) ||
                !std::isfinite(end) || !(end > start)) {
                showPadFeedback(
                    assignment.resource.empty()
                        ? tr("Activate a loop to capture it here, or right-click to "
                             "assign a custom audio file")
                        : tr("Custom audio assignment saved, but sample playback is not "
                             "available yet"));
                break;
            }
            slot.startSec = start;
            slot.endSec = end;
            slot.label = tr("L%1").arg(pad + 1);
            emit trackPerformanceMetadataChanged(deckIndex_);
            showPadFeedback(tr("Saved active loop to %1").arg(slot.label));
        } else {
            // Saved-loop pads are release-aware transition events. The deck
            // previews while held and PLAY takes ownership exactly like a hot
            // cue, so releasing after PLAY cannot stop or rewind transport.
            dispatchPerformancePadGesture(padMode_, pad);
            const auto id = static_cast<ControlId>(
                static_cast<int>(ControlId::SavedLoop1) + pad);
            dispatch(id, 1.0);
            padReleasePending_[pad] = true;
            showPadFeedback(tr("Hold %1 to preview · press PLAY to continue")
                                .arg(slot.label.isEmpty()
                                         ? tr("captured loop %1").arg(pad + 1)
                                         : slot.label));
        }
        waveform_->update();
        break;
    }
    case PerformancePadAction::KeyboardNote:
        showPadFeedback(tr("Note %1 is programmed; pitch-play audio is not available yet")
                            .arg(QString::fromStdString(assignment.label)));
        break;
    case PerformancePadAction::KeyShift:
        showPadFeedback(tr("Key shift %1 is programmed; pitch-shift audio is not available yet")
                            .arg(QString::fromStdString(assignment.label)));
        break;
    }

    // Trigger-only pads do not need a later release. HOT CUE, saved-loop
    // previews, and momentary FX do.
    if (assignment.action != PerformancePadAction::HotCue &&
        assignment.action != PerformancePadAction::FxHold &&
        assignment.action != PerformancePadAction::SavedLoop &&
        assignment.action != PerformancePadAction::SamplerSlot)
        padIsPressed_[pad] = false;
    syncPerformancePadUi();
}

void DeckWidget::beginPadFx(
    int pad, const PerformancePadAssignment& assignment)
{
    if (padFxSnapshot_.valid) {
        const int previousPad = padFxSnapshot_.pad;
        endPadFx(previousPad);
        if (previousPad >= 0 && previousPad < kPerformancePadCount)
            padIsPressed_[previousPad] = false;
    }

    Deck& deck = engine_->deck(deckIndex_);
    padFxSnapshot_.valid = true;
    padFxSnapshot_.pad = pad;
    padFxSnapshot_.type = deck.fxType.load();
    padFxSnapshot_.on = deck.fxOn.load();
    padFxSnapshot_.wet = deck.fxWet.load();
    padFxSnapshot_.beats = deck.fxBeats.load();

    dispatch(ControlId::FxType, assignment.fxType);
    dispatch(ControlId::FxWet, assignment.fxWet);
    dispatch(ControlId::FxBeats, assignment.fxBeats);
    dispatch(ControlId::FxOn, 1.0);
}

void DeckWidget::endPadFx(int pad)
{
    if (!padFxSnapshot_.valid || padFxSnapshot_.pad != pad) return;
    const PadFxSnapshot snapshot = padFxSnapshot_;
    padFxSnapshot_.valid = false;
    padFxSnapshot_.pad = -1;

    // Fully disengage the momentary effect before restoring the previous slot.
    // All changes still travel through ControlBus and remain recordable.
    dispatch(ControlId::FxOn, 0.0);
    dispatch(ControlId::FxType, snapshot.type);
    dispatch(ControlId::FxWet, snapshot.wet);
    dispatch(ControlId::FxBeats, snapshot.beats);
    if (snapshot.on) dispatch(ControlId::FxOn, 1.0);
}

void DeckWidget::showPadFeedback(const QString& text)
{
    if (!padStatusLabel_) return;
    const int serial = ++padFeedbackSerial_;
    padStatusLabel_->setText(text);
    QTimer::singleShot(3500, this, [this, serial] {
        if (serial == padFeedbackSerial_) padStatusLabel_->clear();
    });
}

void DeckWidget::configurePerformancePad(int pad, const QPoint& position)
{
    if (pad < 0 || pad >= kPerformancePadCount) return;
    TrackDataPtr track = engine_->deck(deckIndex_).track();
    if (padMode_ == PerformancePadMode::HotCue) {
        if (track) {
            requestHotCueClear(pad);
        }
        return;
    }

    if (padMode_ == PerformancePadMode::Sampler ||
        padMode_ == PerformancePadMode::SavedLoop) {
        if (!track) {
            showPadFeedback(tr("Load a track before editing CUSTOM pads"));
            return;
        }
        SavedLoopSlot& slot = track->savedLoops[pad];
        auto& sampler = padAssignments_[
            static_cast<int>(PerformancePadMode::Sampler)][pad];
        Deck& deck = engine_->deck(deckIndex_);
        const double activeStart = deck.loopStartSec.load();
        const double activeEnd = deck.loopEndSec.load();
        const bool canCapture = deck.loopActive.load() &&
            std::isfinite(activeStart) && std::isfinite(activeEnd) &&
            activeEnd > activeStart;

        QMenu menu(this);
        QAction* heading = menu.addAction(
            tr("CUSTOM · PAD %1").arg(pad + 1));
        heading->setEnabled(false);
        menu.addSeparator();
        QAction* capture = menu.addAction(
            slot.isSet() ? tr("Replace with active loop") : tr("Save active loop"));
        capture->setEnabled(canCapture);
        connect(capture, &QAction::triggered, this, [this, track, pad,
                                                     activeStart, activeEnd] {
            SavedLoopSlot& changed = track->savedLoops[pad];
            changed.startSec = activeStart;
            changed.endSec = activeEnd;
            if (changed.label.isEmpty()) changed.label = tr("L%1").arg(pad + 1);
            emit trackPerformanceMetadataChanged(deckIndex_);
            syncPerformancePadUi();
            showPadFeedback(tr("Saved active loop to %1").arg(changed.label));
        });
        QAction* rename = menu.addAction(tr("Rename captured loop…"));
        rename->setEnabled(slot.isSet());
        connect(rename, &QAction::triggered, this, [this, track, pad] {
            SavedLoopSlot& changed = track->savedLoops[pad];
            bool ok = false;
            const QString label = QInputDialog::getText(
                this, tr("Rename captured loop"), tr("Short label:"),
                QLineEdit::Normal,
                changed.label.isEmpty() ? tr("L%1").arg(pad + 1)
                                        : changed.label,
                &ok).trimmed();
            if (!ok || label.isEmpty()) return;
            changed.label = label.left(8);
            emit trackPerformanceMetadataChanged(deckIndex_);
            syncPerformancePadUi();
            showPadFeedback(tr("Renamed captured loop to %1").arg(changed.label));
        });
        QAction* clear = menu.addAction(tr("Clear captured loop"));
        clear->setEnabled(slot.isSet());
        connect(clear, &QAction::triggered, this, [this, track, pad] {
            track->savedLoops[pad] = SavedLoopSlot {};
            emit trackPerformanceMetadataChanged(deckIndex_);
            syncPerformancePadUi();
            showPadFeedback(tr("Cleared captured loop %1").arg(pad + 1));
        });

        menu.addSeparator();
        QAction* chooseSample = menu.addAction(tr("Assign custom audio file…"));
        connect(chooseSample, &QAction::triggered, this, [this, pad] {
            auto& changed = padAssignments_[
                static_cast<int>(PerformancePadMode::Sampler)][pad];
            const QString existing = QString::fromStdString(changed.resource);
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Assign custom slot"), existing,
                tr("Audio files (*.wav *.aif *.aiff *.flac *.mp3 *.m4a);;All files (*)"));
            if (path.isEmpty()) return;
            changed.resource = path.toStdString();
            // A saved loop has its own per-track label and takes visual
            // priority; this label is retained for the sampler fallback.
            changed.label = QFileInfo(path).completeBaseName().left(8)
                                .toStdString();
            savePerformancePadAssignment(PerformancePadMode::Sampler, pad);
            syncPerformancePadUi();
            showPadFeedback(tr("Saved custom assignment for pad %1")
                                .arg(pad + 1));
        });
        QAction* clearSample = menu.addAction(tr("Clear custom audio assignment"));
        clearSample->setEnabled(!sampler.resource.empty());
        connect(clearSample, &QAction::triggered, this, [this, pad] {
            auto& changed = padAssignments_[
                static_cast<int>(PerformancePadMode::Sampler)][pad];
            changed = defaultPerformancePadAssignment(
                PerformancePadMode::Sampler, pad);
            savePerformancePadAssignment(PerformancePadMode::Sampler, pad);
            syncPerformancePadUi();
            showPadFeedback(tr("Cleared custom assignment for pad %1")
                                .arg(pad + 1));
        });
        menu.exec(position);
        return;
    }

    const PerformancePadMode mode = padMode_;
    const int modeIndex = static_cast<int>(mode);
    auto& assignment = padAssignments_[modeIndex][pad];
    auto persistAndRefresh = [this, mode, pad] {
        auto& changed = padAssignments_[static_cast<int>(mode)][pad];
        changed = sanitizePerformancePadAssignment(mode, pad, changed);
        savePerformancePadAssignment(mode, pad);
        syncPerformancePadUi();
        showPadFeedback(tr("Saved %1 pad %2")
                            .arg(QLatin1String(performancePadModeLabel(mode)))
                            .arg(pad + 1));
    };

    QMenu menu(this);
    QAction* heading = menu.addAction(
        tr("%1 · PAD %2")
            .arg(QLatin1String(performancePadModeLabel(mode)))
            .arg(pad + 1));
    heading->setEnabled(false);
    menu.addSeparator();

    if (assignment.action == PerformancePadAction::BeatJump) {
        QMenu* beatsMenu = menu.addMenu(tr("Jump beats"));
        static constexpr double choices[] = {
            -64, -32, -16, -8, -4, -2, -1, 1, 2, 4, 8, 16, 32, 64};
        for (const double beats : choices) {
            const QString label = beats > 0
                ? QStringLiteral("+%1").arg(beats)
                : QString::number(beats);
            QAction* action = beatsMenu->addAction(label);
            action->setCheckable(true);
            action->setChecked(std::abs(assignment.value - beats) < 1e-9);
            connect(action, &QAction::triggered, this,
                    [&, beats, label] {
                assignment.value = beats;
                assignment.label = label.toStdString();
                persistAndRefresh();
            });
        }
    } else if (assignment.action == PerformancePadAction::BeatLoop) {
        QMenu* beatsMenu = menu.addMenu(tr("Loop length"));
        static constexpr double choices[] = {
            0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64};
        for (const double beats : choices) {
            const QString label = formatFxBeats(beats);
            QAction* action = beatsMenu->addAction(
                tr("%1 beat(s)").arg(label));
            action->setCheckable(true);
            action->setChecked(std::abs(assignment.value - beats) < 1e-9);
            connect(action, &QAction::triggered, this,
                    [&, beats, label] {
                assignment.value = beats;
                assignment.label = label.toStdString();
                persistAndRefresh();
            });
        }
    } else if (assignment.action == PerformancePadAction::FxHold) {
        auto refreshFxLabel = [&assignment] {
            static constexpr const char* prefixes[3] = {"E", "R", "F"};
            assignment.label =
                (QLatin1String(prefixes[std::clamp(assignment.fxType, 0, 2)]) +
                 formatFxBeats(assignment.fxBeats)).toStdString();
        };

        QMenu* effectMenu = menu.addMenu(tr("Effect"));
        static constexpr const char* names[3] = {"ECHO", "REVERB", "FLANGER"};
        for (int type = 0; type < 3; ++type) {
            QAction* action = effectMenu->addAction(QLatin1String(names[type]));
            action->setCheckable(true);
            action->setChecked(assignment.fxType == type);
            connect(action, &QAction::triggered, this,
                    [&, type] {
                assignment.fxType = type;
                refreshFxLabel();
                persistAndRefresh();
            });
        }

        QMenu* wetMenu = menu.addMenu(tr("Wet level"));
        static constexpr double wetChoices[] = {0.25, 0.5, 0.75, 1.0};
        for (const double wet : wetChoices) {
            QAction* action = wetMenu->addAction(
                tr("%1%").arg(qRound(wet * 100.0)));
            action->setCheckable(true);
            action->setChecked(std::abs(assignment.fxWet - wet) < 1e-9);
            connect(action, &QAction::triggered, this,
                    [&, wet] {
                assignment.fxWet = wet;
                persistAndRefresh();
            });
        }

        QMenu* beatsMenu = menu.addMenu(tr("Effect beats"));
        static constexpr double fxBeatChoices[] = {0.25, 0.5, 1, 2, 4};
        for (const double beats : fxBeatChoices) {
            QAction* action = beatsMenu->addAction(formatFxBeats(beats));
            action->setCheckable(true);
            action->setChecked(std::abs(assignment.fxBeats - beats) < 1e-9);
            connect(action, &QAction::triggered, this,
                    [&, beats] {
                assignment.fxBeats = beats;
                refreshFxLabel();
                persistAndRefresh();
            });
        }
    } else if (assignment.action == PerformancePadAction::SamplerSlot) {
        QAction* choose = menu.addAction(tr("Assign audio file…"));
        connect(choose, &QAction::triggered, this, [&] {
            const QString existing =
                QString::fromStdString(assignment.resource);
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Assign custom slot"), existing,
                tr("Audio files (*.wav *.aif *.aiff *.flac *.mp3 *.m4a);;All files (*)"));
            if (path.isEmpty()) return;
            assignment.resource = path.toStdString();
            assignment.label = QFileInfo(path).completeBaseName().left(8)
                                   .toStdString();
            persistAndRefresh();
        });
        QAction* clear = menu.addAction(tr("Clear audio assignment"));
        clear->setEnabled(!assignment.resource.empty());
        connect(clear, &QAction::triggered, this, [&] {
            assignment = defaultPerformancePadAssignment(mode, pad);
            persistAndRefresh();
        });
    } else if (assignment.action == PerformancePadAction::KeyboardNote) {
        QMenu* noteMenu = menu.addMenu(tr("Keyboard note"));
        static constexpr const char* names[13] = {
            "C4", "C#4", "D4", "D#4", "E4", "F4", "F#4",
            "G4", "G#4", "A4", "A#4", "B4", "C5"};
        for (int note = 0; note < 13; ++note) {
            const int midiNote = 60 + note;
            QAction* action = noteMenu->addAction(QLatin1String(names[note]));
            action->setCheckable(true);
            action->setChecked(qRound(assignment.value) == midiNote);
            connect(action, &QAction::triggered, this,
                    [&, midiNote, note] {
                assignment.value = midiNote;
                assignment.label = names[note];
                persistAndRefresh();
            });
        }
    } else if (assignment.action == PerformancePadAction::KeyShift) {
        QMenu* shiftMenu = menu.addMenu(tr("Semitones"));
        static constexpr int choices[] = {
            -12, -7, -4, -2, -1, 0, 1, 2, 4, 7, 12};
        for (const int semitones : choices) {
            const QString label = QStringLiteral("%1%2")
                .arg(semitones >= 0 ? QStringLiteral("+") : QString())
                .arg(semitones);
            QAction* action = shiftMenu->addAction(label);
            action->setCheckable(true);
            action->setChecked(qRound(assignment.value) == semitones);
            connect(action, &QAction::triggered, this,
                    [&, semitones, label] {
                assignment.value = semitones;
                assignment.label = label.toStdString();
                persistAndRefresh();
            });
        }
    }

    menu.addSeparator();
    QAction* rename = menu.addAction(tr("Custom pad label…"));
    connect(rename, &QAction::triggered, this, [&] {
        bool ok = false;
        const QString current = QString::fromStdString(assignment.label);
        const QString label = QInputDialog::getText(
            this, tr("Performance pad label"), tr("Short label:"),
            QLineEdit::Normal, current, &ok).trimmed();
        if (!ok || label.isEmpty()) return;
        assignment.label = label.left(8).toStdString();
        persistAndRefresh();
    });
    QAction* reset = menu.addAction(tr("Reset this pad"));
    connect(reset, &QAction::triggered, this, [&] {
        assignment = defaultPerformancePadAssignment(mode, pad);
        persistAndRefresh();
    });

    menu.exec(position);
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

void DeckWidget::syncHotCueButtons()
{
    syncPerformancePadUi();
}

void DeckWidget::syncLoopButtons()
{
    Deck& deck = engine_->deck(deckIndex_);
    TrackDataPtr t = deck.track();
    const bool active = deck.loopActive.load();
    const double start = deck.loopStartSec.load();
    const double end = deck.loopEndSec.load();
    const bool inSet = start >= 0.0;

    // Active loop length in beats ≈ (end-start)*bpm/60, matched to the
    // nearest standard auto-loop size (±20% tolerance).
    int lenIdx = -2;
    if (active && t && t->bpm > 0.0 && end > start) {
        const double beats = (end - start) * t->bpm / 60.0;
        lenIdx = -1;
        for (int i = 0; i < 5; ++i) {
            if (std::abs(beats / kAutoLoopBeats[i] - 1.0) < 0.2) {
                lenIdx = i;
                break;
            }
        }
    }
    if (lenIdx == shownLoopLenIdx_ && inSet == shownLoopIn_ &&
        active == shownLoopActive_)
        return;
    shownLoopLenIdx_ = lenIdx;
    shownLoopIn_ = inSet;
    shownLoopActive_ = active;

    const QString base = QString::fromLatin1(kLoopBtnBase);
    const QString lit =
        base + QStringLiteral("QPushButton { background:%1; color:black; "
                              "font-weight:bold; }")
                   .arg(deckAccent(deckIndex_).name());
    for (int i = 0; i < 5; ++i)
        autoLoopBtns_[i]->setStyleSheet(i == lenIdx ? lit : base);
    loopInBtn_->setStyleSheet(inSet ? lit : base);
    loopOutBtn_->setStyleSheet(active ? lit : base);
    waveform_->update(); // loop region may have (dis)appeared while paused
}

void DeckWidget::syncFxControls()
{
    Deck& deck = engine_->deck(deckIndex_);

    const int type = std::clamp(deck.fxType.load(), 0, 2);
    if (fxTypeCombo_->currentIndex() != type) {
        QSignalBlocker block(fxTypeCombo_);
        fxTypeCombo_->setCurrentIndex(type);
    }

    const bool on = deck.fxOn.load();
    if (fxOnBtn_->isChecked() != on) {
        QSignalBlocker block(fxOnBtn_);
        fxOnBtn_->setChecked(on);
    }
    if (shownFxOn_ != (on ? 1 : 0)) {
        shownFxOn_ = on ? 1 : 0;
        const QString base = QString::fromLatin1(kLoopBtnBase);
        fxOnBtn_->setStyleSheet(
            on ? base + QStringLiteral(
                            "QPushButton { background:%1; color:black; "
                            "font-weight:bold; }")
                            .arg(deckAccent(deckIndex_).name())
               : base);
    }

    if (!fxWetDial_->isSliderDown()) {
        const int wet = (int)std::lround(
            std::clamp((double)deck.fxWet.load(), 0.0, 1.0) * 100.0);
        if (fxWetDial_->value() != wet) {
            QSignalBlocker block(fxWetDial_);
            fxWetDial_->setValue(wet);
        }
    }

    const QString beatsText = formatFxBeats(
        std::clamp(deck.fxBeats.load(), 0.25, 4.0));
    if (fxBeatsLabel_->text() != beatsText)
        fxBeatsLabel_->setText(beatsText);
}

void DeckWidget::setStemsIdle()
{
    stemsState_ = StemsState::Idle;
    const bool hasTrack = engine_->deck(deckIndex_).track() != nullptr;
    stemsBtn_->setEnabled(hasTrack);
    stemsStatusLabel_->clear();
    for (QPushButton* b : stemPads_) {
        QSignalBlocker block(b);
        b->setChecked(true);
        b->setEnabled(false);
        b->setToolTip(tr("separating stems…"));
    }
}

void DeckWidget::setStemsInProgress(const QString& stage)
{
    stemsState_ = StemsState::InProgress;
    stemsBtn_->setEnabled(false);
    stemsStatusLabel_->setText(stage);
    for (QPushButton* b : stemPads_) {
        b->setEnabled(false);
        b->setToolTip(tr("separating stems…"));
    }
}

void DeckWidget::setStemsReady()
{
    stemsState_ = StemsState::Ready;
    stemsBtn_->setEnabled(false);
    stemsStatusLabel_->setText(tr("ready"));
    static const char* kTips[4] = {
        QT_TR_NOOP("Toggle the vocal stem"),
        QT_TR_NOOP("Toggle the melody stem"),
        QT_TR_NOOP("Toggle the bass stem"),
        QT_TR_NOOP("Toggle the drums stem"),
    };
    for (int i = 0; i < 4; ++i) {
        stemPads_[i]->setEnabled(true);
        stemPads_[i]->setToolTip(tr(kTips[i]));
    }
    syncStemPads();
}

void DeckWidget::syncStemPads()
{
    if (stemsState_ != StemsState::Ready) return;
    Deck& deck = engine_->deck(deckIndex_);
    const float levels[4] = {deck.stemVocals.load(), deck.stemMelody.load(),
                             deck.stemBass.load(), deck.stemDrums.load()};
    for (int i = 0; i < 4; ++i) {
        const bool on = levels[i] >= 0.5f;
        if (stemPads_[i]->isChecked() != on) {
            QSignalBlocker block(stemPads_[i]);
            stemPads_[i]->setChecked(on);
        }
    }
}

void DeckWidget::trackChanged()
{
    TrackDataPtr t = engine_->deck(deckIndex_).track();
    if (gridBtn_)
        gridBtn_->setEnabled(t != nullptr);
    if (t) {
        titleLabel_->setText(t->title.isEmpty() ? t->filePath : t->title);
        artistLabel_->setText(t->artist);
        const QString key = t->camelotKey.isEmpty()
                                ? QString()
                                : t->camelotKey + QStringLiteral(" · ");
        bpmLabel_->setText(key + QString::asprintf("BPM %.2f", t->bpm));
    } else {
        titleLabel_->setText(tr("—"));
        artistLabel_->clear();
        bpmLabel_->setText(tr("BPM —"));
    }
    syncHotCueButtons();
    // New track (or unload): back to the request state. MainWindow
    // auto-requests the cheap cached-decode path right after when
    // StemSeparator::hasCached() hits.
    setStemsIdle();
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
    case ControlId::LoopIn: case ControlId::LoopOut:
    case ControlId::LoopExit: case ControlId::LoopHalve:
    case ControlId::LoopDouble: case ControlId::LoopAuto:
        // ControlBus dispatch is synchronous: by the time this mirror runs,
        // the deck atomics contain the new loop state.  Repaint immediately
        // so MIDI and on-screen loop controls feel equally responsive.
        syncLoopButtons();
        waveform_->update();
        break;
    case ControlId::Quantize: {
        const bool enabled = engine_->deck(deckIndex_).quantizeHotCues.load(
            std::memory_order_acquire);
        QSettings().setValue(
            QStringLiteral("decks/%1/quantizeHotCues").arg(deckIndex_),
            enabled);
        if (quantizeBtn_->isChecked() != enabled) {
            QSignalBlocker block(quantizeBtn_);
            quantizeBtn_->setChecked(enabled);
        }
        break;
    }
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

    const bool quantize = deck.quantizeHotCues.load(std::memory_order_acquire);
    if (quantizeBtn_->isChecked() != quantize) {
        QSignalBlocker block(quantizeBtn_);
        quantizeBtn_->setChecked(quantize);
    }

    if (t && t->durationSec > 0.0) {
        double pos = deck.positionSec();
        timeLabel_->setText(formatTime(pos) + " / -" +
                            formatTime(t->durationSec - pos));
        double eff = deck.effectiveBpm();
        // Camelot key next to BPM ("8A · BPM 128.00 → 128.00") — the key is
        // filled in asynchronously by analysis, so re-check every tick.
        const QString key = t->camelotKey.isEmpty()
                                ? QString()
                                : t->camelotKey + QStringLiteral(" · ");
        bpmLabel_->setText(
            key + QString::asprintf("BPM %.2f → %.2f", t->bpm, eff));
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
    syncLoopButtons();
    syncFxControls();
    syncStemPads();
}

} // namespace gvt
