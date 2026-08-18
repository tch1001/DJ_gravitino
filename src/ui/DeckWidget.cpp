#include "DeckWidget.h"
#include "Theme.h"

#include <QComboBox>
#include <QDial>
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

    // Loop region: accent shading between loopStartSec/loopEndSec —
    // ~25% alpha while active, dimmer when bounds are set but inactive —
    // with brighter edge lines.
    {
        const double ls = deck_->loopStartSec.load();
        const double le = deck_->loopEndSec.load();
        const bool active = deck_->loopActive.load();
        if (ls >= 0.0 && le > ls && ls < t->durationSec) {
            const int x0 = (int)(ls / t->durationSec * w);
            const int x1 = (int)(std::min(le, t->durationSec) /
                                 t->durationSec * w);
            QColor fill = accent;
            fill.setAlpha(active ? 64 : 28);
            p.fillRect(QRect(x0, 0, std::max(1, x1 - x0), h), fill);
            QColor edge = accent.lighter(140);
            edge.setAlpha(active ? 230 : 110);
            p.setPen(QPen(edge, 1));
            p.drawLine(x0, 0, x0, h);
            p.drawLine(x1, 0, x1, h);
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

    // Transport row: PLAY, CUE, SYNC.
    auto* transport = new QHBoxLayout;
    playBtn_ = new QPushButton(tr("PLAY"));
    playBtn_->setCheckable(true);
    cueBtn_ = new QPushButton(tr("CUE")); // hold-to-preview: NOT checkable
    syncBtn_ = new QPushButton(tr("SYNC"));
    cueBtn_->setToolTip(
        tr("Hold to preview from cue; press PLAY while held to continue playing"));
    for (QPushButton* b : {playBtn_, cueBtn_, syncBtn_})
        b->setMinimumHeight(26);
    playBtn_->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:black; "
                       "font-weight:bold; }")
            .arg(accent.name()));
    transport->addWidget(playBtn_);
    transport->addWidget(cueBtn_);
    transport->addWidget(syncBtn_);
    mainCol->addLayout(transport);

    // 8 hot-cue pads, 2 rows of 4 small squares. Assigned pads preview from
    // the marker while held, then stop/return on release unless PLAY latches it.
    auto* cues = new QGridLayout;
    cues->setSpacing(3);
    for (int i = 0; i < 8; ++i) {
        auto* b = new QPushButton(QString::number(i + 1));
        b->setFixedSize(26, 26);
        b->setToolTip(tr("Hot cue %1 — hold: preview, PLAY: continue, release: return; "
                         "right- or shift-click: clear")
                          .arg(i + 1));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        hotcueBtns_[i] = b;
        cues->addWidget(b, i / 4, i % 4, Qt::AlignLeft);
        const auto hotCueId =
            (ControlId)((int)ControlId::HotCue1 + i);
        connect(b, &QPushButton::pressed, this, [this, i, hotCueId] {
            if (QGuiApplication::keyboardModifiers().testFlag(
                    Qt::ShiftModifier)) {
                if (TrackDataPtr t = engine_->deck(deckIndex_).track()) {
                    t->hotCues[i] = -1; // clear via TrackData hotCues array
                    syncHotCueButtons();
                    waveform_->update();
                }
                return;
            }
            dispatch(hotCueId, 1.0);
            syncHotCueButtons();
            waveform_->update();
        });
        connect(b, &QPushButton::released, this,
                [this, hotCueId] { dispatch(hotCueId, 0.0); });
        connect(b, &QPushButton::customContextMenuRequested, this, [this, i] {
            if (TrackDataPtr t = engine_->deck(deckIndex_).track()) {
                t->hotCues[i] = -1; // clear via TrackData hotCues array
                syncHotCueButtons();
                waveform_->update();
            }
        });
    }
    cues->setColumnStretch(4, 1);
    mainCol->addLayout(cues);

    // Loop / beat-jump section: one compact row below the hot cues.
    // [1/2][1][2][4][8] auto-loop · [IN][OUT][EXIT] manual · [<½][2×>]
    // resize · [◀8][◀4][◀1][1▶][4▶][8▶] beat jump. All fire on press via
    // the bus; active loop length + IN/OUT state highlighted from refresh().
    auto* loopRow = new QHBoxLayout;
    loopRow->setSpacing(2);
    auto mkLoopBtn = [&](const QString& text, const QString& tip) {
        auto* b = new QPushButton(text, this);
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

    fxOnBtn_ = new QPushButton(tr("ON"), this);
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
        auto* b = new QPushButton(text, this);
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
    stemsBtn_ = new QPushButton(tr("STEMS"), this);
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
        auto* b = new QPushButton(QLatin1String(kStemPads[i].text), this);
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
    TrackDataPtr t = engine_->deck(deckIndex_).track();
    for (int i = 0; i < 8; ++i) {
        bool set = t && t->hotCues[i] >= 0;
        hotcueBtns_[i]->setStyleSheet(
            set ? QStringLiteral("background:%1; color:black; font-weight:bold;")
                      .arg(hotCueColor(i).name())
                : QString());
    }
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
