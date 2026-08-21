#include "Flx4TutorialWidget.h"

#include "Theme.h"
#include "../performance/PerformancePads.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace gvt {
namespace {

constexpr qreal kDesignWidth = 1000.0;
constexpr qreal kDesignHeight = 620.0;
constexpr double kPi = 3.14159265358979323846;

QRectF padRect(int deck, int pad)
{
    const qreal startX = deck == 0 ? 158.0 : 758.0;
    return QRectF(startX + (pad % 4) * 49.0,
                  465.0 + (pad / 4) * 42.0, 44.0, 34.0);
}

QRectF padModeRect(int deck, int mode)
{
    const qreal startX = deck == 0 ? 158.0 : 758.0;
    return QRectF(startX + mode * 49.0, 431.0, 44.0, 24.0);
}

QRectF deckButtonRect(int deck, int slot)
{
    const qreal offset = deck == 0 ? 0.0 : 600.0;
    if (slot == 0)
        return QRectF(offset + 24.0, 44.0, 55.0, 27.0);
    return QRectF(offset + (slot == 1 ? 55.0 : 107.0),
                  470.0, 44.0, 44.0);
}

QRectF loopButtonRect(int deck, int slot)
{
    const qreal start = deck == 0 ? 88.0 : 688.0;
    return QRectF(start + slot * 52.0, 44.0, 47.0, 27.0);
}

qreal channelX(int deck) { return deck == 0 ? 414.0 : 536.0; }

bool isAnimatedContinuous(Flx4SurfaceControl surface)
{
    switch (surface) {
    case Flx4SurfaceControl::TempoFader:
    case Flx4SurfaceControl::ChannelFader:
    case Flx4SurfaceControl::Trim:
    case Flx4SurfaceControl::EqHigh:
    case Flx4SurfaceControl::EqMid:
    case Flx4SurfaceControl::EqLow:
    case Flx4SurfaceControl::Filter:
    case Flx4SurfaceControl::Crossfader:
    case Flx4SurfaceControl::HeadphoneMix:
    case Flx4SurfaceControl::BeatFxWet:
        return true;
    default:
        return false;
    }
}

double surfaceValue(ControlId control, double value)
{
    if (control == ControlId::Tempo)
        return std::clamp((value - 0.92) / 0.16, 0.0, 1.0);
    return std::clamp(value, 0.0, 1.0);
}

int physicalPadModeButton(int mode)
{
    if (mode >= static_cast<int>(PerformancePadMode::Keyboard) &&
        mode <= static_cast<int>(PerformancePadMode::KeyShift))
        return mode - static_cast<int>(PerformancePadMode::Keyboard);
    if (mode == static_cast<int>(PerformancePadMode::SavedLoop))
        return 3;
    return std::clamp(mode, 0, 3);
}

} // namespace

Flx4TutorialWidget::Flx4TutorialWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("flx4TutorialOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
    auto* pulseTimer = new QTimer(this);
    pulseTimer->setInterval(360);
    connect(pulseTimer, &QTimer::timeout, this, [this] {
        pulse_ = !pulse_;
        animationPhase_ = (animationPhase_ + 1) % 8;
        update();
    });
    pulseTimer->start();
}

void Flx4TutorialWidget::setWaiting(const QString& transitionName,
                                    const QString& warning)
{
    expected_.reset();
    mapping_.reset();
    transitionName_ = transitionName;
    instruction_ = tr("Get ready — the next FLX4 control will light up");
    detail_ = tr("Use the physical controller or highlighted virtual control");
    warning_ = warning;
    feedback_.clear();
    hardwareValue_.reset();
    activationEnabled_ = false;
    gesturePadMode_ = -1;
    update();
}

void Flx4TutorialWidget::setExpected(
    const ControlEvent& event, const QString& instruction,
    const QString& detail, double beatsAhead, bool activationEnabled,
    const QString& warning,
    const std::optional<Flx4TutorialMapping>& mapping, int gesturePadMode)
{
    const bool samePhysicalControl = expected_ &&
        expected_->deck == event.deck && expected_->id == event.id;
    expected_ = event;
    mapping_ = mapping;
    instruction_ = instruction;
    detail_ = detail;
    beatsAhead_ = beatsAhead;
    activationEnabled_ = activationEnabled && mapping_.has_value();
    gesturePadMode_ = gesturePadMode;
    warning_ = warning;
    if (!samePhysicalControl) hardwareValue_.reset();
    update();
}

void Flx4TutorialWidget::updateCountdown(double beatsAhead)
{
    beatsAhead_ = beatsAhead;
    update();
}

void Flx4TutorialWidget::setFeedback(const QString& text, const QColor& color)
{
    feedback_ = text;
    feedbackColor_ = color;
    update();
}

void Flx4TutorialWidget::clearExpected()
{
    expected_.reset();
    mapping_.reset();
    instruction_ = tr("Good — watch for the next highlighted control");
    detail_ = tr("Keep the outgoing track running and follow the sequence");
    warning_.clear();
    activationEnabled_ = false;
    gesturePadMode_ = -1;
    hardwareValue_.reset();
    update();
}

void Flx4TutorialWidget::setHardwareValue(const ControlEvent& event)
{
    if (!expected_ || expected_->deck != event.deck ||
        expected_->id != event.id || !std::isfinite(event.value))
        return;
    hardwareValue_ = event.value;
    update();
}

void Flx4TutorialWidget::setLiveState(const Flx4TutorialLiveState& state)
{
    live_ = state;
    update();
}

void Flx4TutorialWidget::setTakeovers(
    const std::vector<SoftTakeoverState>& states)
{
    takeovers_ = states;
    update();
}

QRectF Flx4TutorialWidget::fittedDesignRect() const
{
    const qreal scale = std::min(width() / kDesignWidth,
                                 height() / kDesignHeight);
    const QSizeF fitted(kDesignWidth * scale, kDesignHeight * scale);
    return QRectF((width() - fitted.width()) * 0.5,
                  (height() - fitted.height()) * 0.5,
                  fitted.width(), fitted.height());
}

QPointF Flx4TutorialWidget::toDesign(const QPointF& point) const
{
    const QRectF fitted = fittedDesignRect();
    if (fitted.width() <= 0.0 || fitted.height() <= 0.0) return {};
    return QPointF((point.x() - fitted.left()) * kDesignWidth / fitted.width(),
                   (point.y() - fitted.top()) * kDesignHeight / fitted.height());
}

QRectF Flx4TutorialWidget::fxAssignmentRect(int deck) const
{
    return QRectF(deck == 0 ? 571.0 : 603.0, 270.0, 28.0, 20.0);
}

QRectF Flx4TutorialWidget::targetRect(const Flx4TutorialMapping& mapping,
                                      int deck) const
{
    const qreal chX = channelX(std::clamp(deck, 0, 1));
    switch (mapping.surface) {
    case Flx4SurfaceControl::PlayPause:     return deckButtonRect(deck, 2);
    case Flx4SurfaceControl::Cue:           return deckButtonRect(deck, 1);
    case Flx4SurfaceControl::Sync:          return deckButtonRect(deck, 0);
    case Flx4SurfaceControl::Load:
        return QRectF(deck == 0 ? 378.0 : 536.0, 31.0, 54.0, 24.0);
    case Flx4SurfaceControl::PerformancePad:return padRect(deck, mapping.pad);
    case Flx4SurfaceControl::LoopIn:
    case Flx4SurfaceControl::LoopHalve:     return loopButtonRect(deck, 0);
    case Flx4SurfaceControl::LoopOut:
    case Flx4SurfaceControl::LoopDouble:    return loopButtonRect(deck, 1);
    case Flx4SurfaceControl::FourBeatExit:  return loopButtonRect(deck, 2);
    case Flx4SurfaceControl::TempoFader:
        return QRectF(deck == 0 ? 24.0 : 956.0, 143.0, 25.0, 246.0);
    case Flx4SurfaceControl::JogWheel:
        return QRectF(deck == 0 ? 82.0 : 682.0, 145.0, 220.0, 220.0);
    case Flx4SurfaceControl::ChannelFader:
        return QRectF(chX - 13.0, 429.0, 26.0, 112.0);
    case Flx4SurfaceControl::Trim:          return QRectF(chX - 16.0, 148.0, 32.0, 32.0);
    case Flx4SurfaceControl::EqHigh:        return QRectF(chX - 16.0, 198.0, 32.0, 32.0);
    case Flx4SurfaceControl::EqMid:         return QRectF(chX - 16.0, 248.0, 32.0, 32.0);
    case Flx4SurfaceControl::EqLow:         return QRectF(chX - 16.0, 298.0, 32.0, 32.0);
    case Flx4SurfaceControl::Filter:        return QRectF(chX - 17.0, 348.0, 34.0, 34.0);
    case Flx4SurfaceControl::Crossfader:    return QRectF(405.0, 556.0, 140.0, 27.0);
    case Flx4SurfaceControl::ChannelCue:    return QRectF(chX - 23.0, 397.0, 46.0, 22.0);
    case Flx4SurfaceControl::MasterCue:     return QRectF(462.0, 124.0, 56.0, 22.0);
    case Flx4SurfaceControl::HeadphoneMix:  return QRectF(474.0, 160.0, 36.0, 36.0);
    case Flx4SurfaceControl::BeatFxSelect:  return QRectF(570.0, 160.0, 62.0, 23.0);
    case Flx4SurfaceControl::BeatFxOn:      return QRectF(570.0, 301.0, 62.0, 24.0);
    case Flx4SurfaceControl::BeatFxWet:     return QRectF(583.0, 339.0, 36.0, 36.0);
    case Flx4SurfaceControl::BeatFxBeats:   return QRectF(570.0, 195.0, 62.0, 24.0);
    }
    return {};
}

void Flx4TutorialWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(8, 10, 13, 248));

    const QRectF fitted = fittedDesignRect();
    p.translate(fitted.left(), fitted.top());
    p.scale(fitted.width() / kDesignWidth, fitted.height() / kDesignHeight);

    p.setPen(QPen(QColor(78, 84, 96), 2));
    p.setBrush(QColor(29, 32, 38));
    p.drawRoundedRect(QRectF(5, 5, 990, 610), 14, 14);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(23, 26, 31));
    p.drawRoundedRect(QRectF(357, 14, 286, 584), 9, 9);

    p.setPen(QPen(QColor(94, 101, 115), 1));
    p.setBrush(QColor(45, 49, 58));
    p.drawRoundedRect(QRectF(961, 13, 25, 23), 5, 5);
    p.setPen(themeText());
    p.drawText(QRectF(961, 13, 25, 23), Qt::AlignCenter, QStringLiteral("×"));

    auto drawButton = [&p](const QRectF& r, const QString& label,
                           const QColor& accent, bool lit = false,
                           bool round = false) {
        const QColor border = lit ? accent : QColor(75, 81, 93);
        p.setPen(QPen(border, lit ? 2.2 : 1.0));
        p.setBrush(lit ? QColor(accent.red(), accent.green(), accent.blue(), 92)
                       : QColor(47, 51, 60));
        if (round) p.drawEllipse(r);
        else p.drawRoundedRect(r, 3, 3);
        QFont f = p.font();
        f.setPixelSize(label.size() > 8 ? 7 : 8);
        f.setBold(true);
        p.setFont(f);
        p.setPen(lit ? QColor(245, 247, 250) : accent);
        p.drawText(r.adjusted(2, 1, -2, -1), Qt::AlignCenter | Qt::TextWordWrap,
                   label);
    };

    auto drawKnob = [&p](qreal x, qreal y, const QString& label,
                         double value) {
        value = std::clamp(value, 0.0, 1.0);
        p.setPen(QPen(QColor(94, 101, 115), 2));
        p.setBrush(QColor(39, 43, 51));
        p.drawEllipse(QPointF(x, y), 13, 13);
        const double radians = (225.0 + value * 270.0) * kPi / 180.0;
        p.setPen(QPen(QColor(232, 236, 241), 2));
        p.drawLine(QPointF(x, y),
                   QPointF(x + std::cos(radians) * 9.0,
                           y + std::sin(radians) * 9.0));
        QFont f = p.font(); f.setPixelSize(7); p.setFont(f);
        p.setPen(themeDimText());
        p.drawText(QRectF(x - 33, y + 14, 66, 12), Qt::AlignCenter, label);
    };

    const QStringList loopLabels {tr("IN / ½×"), tr("OUT / 2×"),
                                  tr("4 BEAT / EXIT"), tr("CALL ◁"),
                                  tr("CALL ▷")};
    const QStringList modeLabels {tr("HOT CUE"), tr("PAD FX1"),
                                  tr("BEAT JUMP"), tr("CUSTOM")};
    const QStringList shiftedModeLabels {tr("⇧ KEYBOARD"), tr("⇧ PAD FX2"),
                                         tr("⇧ BEAT LOOP"), tr("⇧ KEY SHIFT")};
    for (int deck = 0; deck < 2; ++deck) {
        const QColor accent = deckAccent(deck);
        const qreal offset = deck == 0 ? 0.0 : 600.0;
        const qreal jogX = deck == 0 ? 192.0 : 792.0;

        QFont deckFont = p.font();
        deckFont.setPixelSize(11); deckFont.setBold(true);
        p.setFont(deckFont); p.setPen(accent);
        p.drawText(QRectF(offset + 20.0, 12.0, 75.0, 20.0),
                   Qt::AlignCenter, deck == 0 ? tr("DECK A") : tr("DECK B"));

        drawButton(deckButtonRect(deck, 0), tr("BEAT SYNC"), accent);
        drawButton(QRectF(offset + 24.0, 76.0, 55.0, 23.0),
                   tr("MASTER"), accent);
        for (int slot = 0; slot < loopLabels.size(); ++slot)
            drawButton(loopButtonRect(deck, slot), loopLabels[slot],
                       themeDimText(), live_.loopActive[deck] && slot < 3);

        p.setPen(QPen(QColor(82, 89, 102), 3));
        p.setBrush(QColor(36, 39, 47));
        p.drawEllipse(QPointF(jogX, 255.0), 104.0, 104.0);
        p.setPen(QPen(QColor(58, 64, 75), 2));
        p.drawEllipse(QPointF(jogX, 255.0), 71.0, 71.0);
        p.drawEllipse(QPointF(jogX, 255.0), 25.0, 25.0);
        p.setPen(themeDimText());
        p.drawText(QRectF(jogX - 55.0, 244.0, 110.0, 22.0),
                   Qt::AlignCenter, tr("JOG TOP / RIM"));

        const qreal tempoX = deck == 0 ? 36.0 : 968.0;
        const double tempoValue = surfaceValue(ControlId::Tempo,
                                                live_.tempo[deck]);
        p.setPen(QPen(QColor(78, 84, 96), 5));
        p.drawLine(QPointF(tempoX, 153.0), QPointF(tempoX, 379.0));
        p.setPen(Qt::NoPen); p.setBrush(QColor(189, 195, 205));
        const qreal tempoY = 153.0 + tempoValue * 226.0;
        p.drawRoundedRect(QRectF(tempoX - 9.0, tempoY - 11.0, 18.0, 22.0),
                          3, 3);
        p.setPen(themeDimText());
        p.drawText(QRectF(tempoX - 28.0, 130.0, 56.0, 14.0),
                   Qt::AlignCenter, tr("TEMPO"));

        const bool shifted = live_.padMode[deck] >=
                                 static_cast<int>(PerformancePadMode::Keyboard) &&
                             live_.padMode[deck] <=
                                 static_cast<int>(PerformancePadMode::KeyShift);
        const int litMode = physicalPadModeButton(live_.padMode[deck]);
        for (int mode = 0; mode < 4; ++mode) {
            drawButton(padModeRect(deck, mode), modeLabels[mode], accent,
                       mode == litMode);
            QFont shiftedFont = p.font();
            shiftedFont.setPixelSize(5);
            shiftedFont.setBold(mode == litMode && shifted);
            p.setFont(shiftedFont);
            p.setPen(mode == litMode && shifted ? accent : themeDimText());
            const QRectF modeRect = padModeRect(deck, mode);
            p.drawText(QRectF(modeRect.left(), modeRect.bottom(),
                              modeRect.width(), 10.0),
                       Qt::AlignCenter, shiftedModeLabels[mode]);
        }
        for (int pad = 0; pad < 8; ++pad) {
            const unsigned int bit = 1U << static_cast<unsigned int>(pad);
            const bool on = (live_.padEnabledMask[deck] & bit) != 0U;
            const bool pressed = (live_.padPressedMask[deck] & bit) != 0U;
            drawButton(padRect(deck, pad), QString::number(pad + 1),
                       hotCueColor(pad), on || pressed);
            if (pressed) {
                p.setPen(QPen(Qt::white, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(padRect(deck, pad).adjusted(2, 2, -2, -2),
                                  3, 3);
            }
        }

        drawButton(deckButtonRect(deck, 1), tr("CUE"), QColor(240, 240, 240),
                   live_.cueSet[deck], true);
        drawButton(deckButtonRect(deck, 2), tr("PLAY / PAUSE"), accent,
                   live_.playing[deck], true);
        drawButton(QRectF(offset + 55.0, 525.0, 96.0, 24.0), tr("SHIFT"),
                   themeDimText(), shifted);
        drawButton(QRectF(offset + 55.0, 556.0, 96.0, 22.0),
                   tr("VINYL MODE"), themeDimText());
    }

    drawButton(QRectF(378, 31, 54, 24), tr("LOAD A"), deckAccent(0));
    drawButton(QRectF(536, 31, 54, 24), tr("LOAD B"), deckAccent(1));
    drawKnob(484, 43, tr("BROWSE / PRESS"), 0.5);
    drawKnob(484, 91, tr("MASTER LEVEL"), 0.5);
    drawButton(QRectF(462, 124, 56, 22), tr("MASTER CUE"),
               QColor(230, 230, 235), live_.masterCue);
    drawKnob(492, 178, tr("CUE / MASTER"), live_.headphoneMix);
    drawKnob(492, 225, tr("PHONES LEVEL"), 0.5);
    drawKnob(492, 273, tr("MIC LEVEL"), 0.5);
    drawButton(QRectF(459, 306, 66, 22), tr("SMART CFX"), themeDimText());
    drawButton(QRectF(459, 338, 66, 22), tr("SMART FADER"), themeDimText());

    for (int deck = 0; deck < 2; ++deck) {
        const qreal x = channelX(deck);
        drawKnob(x, 164, tr("TRIM"), live_.trim[deck]);
        drawKnob(x, 214, tr("HIGH"), live_.eqHigh[deck]);
        drawKnob(x, 264, tr("MID"), live_.eqMid[deck]);
        drawKnob(x, 314, tr("LOW"), live_.eqLow[deck]);
        drawKnob(x, 365, tr("FILTER / CFX"), live_.filter[deck]);
        drawButton(QRectF(x - 23.0, 397.0, 46.0, 22.0), tr("CH CUE"),
                   deckAccent(deck), live_.channelCue[deck]);
        QFont quantFont = p.font();
        quantFont.setPixelSize(5);
        quantFont.setBold(live_.quantize[deck]);
        p.setFont(quantFont);
        p.setPen(live_.quantize[deck] ? deckAccent(deck) : themeDimText());
        p.drawText(QRectF(x - 23.0, 419.0, 46.0, 9.0), Qt::AlignCenter,
                   tr("⇧ QUANT"));

        const int litSegments = live_.level[deck] <= 0.0001
            ? 0 : std::clamp(static_cast<int>(std::ceil(
                    std::clamp((20.0 * std::log10(live_.level[deck]) + 48.0) /
                                   48.0,
                               0.0, 1.0) * 5.0)),
                             0, 5);
        for (int segment = 0; segment < 5; ++segment) {
            const QColor color = segment < 2 ? QColor(0x4c, 0xd9, 0x64)
                                 : segment < 4 ? QColor(0xe8, 0xa8, 0x35)
                                               : QColor(0xe8, 0x55, 0x55);
            const QRectF bar(x + (deck == 0 ? 20.0 : -25.0),
                             430.0 + (4 - segment) * 13.0, 5.0, 9.0);
            p.setPen(Qt::NoPen);
            p.setBrush(segment < litSegments ? color : QColor(55, 59, 68));
            p.drawRoundedRect(bar, 1, 1);
        }

        p.setPen(QPen(QColor(79, 85, 97), 5));
        p.drawLine(QPointF(x, 435), QPointF(x, 537));
        const qreal faderY = 537.0 - std::clamp(live_.fader[deck], 0.0, 1.0) *
                                          102.0;
        p.setPen(Qt::NoPen); p.setBrush(QColor(188, 194, 204));
        p.drawRoundedRect(QRectF(x - 10.0, faderY - 8.0, 20.0, 16.0), 3, 3);
    }

    p.setPen(QPen(QColor(79, 85, 97), 5));
    p.drawLine(QPointF(414, 570), QPointF(536, 570));
    const qreal crossX = 414.0 + std::clamp(live_.crossfader, 0.0, 1.0) * 122.0;
    p.setPen(Qt::NoPen); p.setBrush(QColor(188, 194, 204));
    p.drawRoundedRect(QRectF(crossX - 11.0, 561.0, 22.0, 18.0), 3, 3);
    p.setPen(themeDimText());
    p.drawText(QRectF(405, 584, 140, 12), Qt::AlignCenter, tr("CROSSFADER"));

    p.setPen(themeDimText());
    p.drawText(QRectF(570, 139, 62, 16), Qt::AlignCenter, tr("BEAT FX"));
    drawButton(QRectF(570, 160, 62, 23), tr("FX SELECT"), themeDimText());
    drawButton(QRectF(570, 195, 29, 24), tr("BEAT ◁"), themeDimText());
    drawButton(QRectF(603, 195, 29, 24), tr("BEAT ▷"), themeDimText());
    drawButton(QRectF(570, 231, 62, 23), tr("FX CH SELECT"), themeDimText());
    drawButton(fxAssignmentRect(0), tr("1"), deckAccent(0));
    drawButton(fxAssignmentRect(1), tr("2"), deckAccent(1));
    drawButton(QRectF(570, 301, 62, 24), tr("FX ON / OFF"),
               QColor(0xe8, 0xa8, 0x35), live_.fxOn[0] || live_.fxOn[1]);
    drawKnob(601, 355, tr("LEVEL / DEPTH"), live_.fxWet);

    auto liveControlValue = [this](DeckId deck, ControlId id) {
        if (id == ControlId::Crossfader) return live_.crossfader;
        if (id == ControlId::HeadphoneMix) return live_.headphoneMix;
        if (deck < 0 || deck > 1) return 0.5;
        switch (id) {
        case ControlId::Tempo: return live_.tempo[deck];
        case ControlId::Fader: return live_.fader[deck];
        case ControlId::Trim: return live_.trim[deck];
        case ControlId::EqHigh: return live_.eqHigh[deck];
        case ControlId::EqMid: return live_.eqMid[deck];
        case ControlId::EqLow: return live_.eqLow[deck];
        case ControlId::Filter: return live_.filter[deck];
        case ControlId::FxWet: return live_.fxWet;
        default: return 0.5;
        }
    };

    auto drawMotion = [&p](Flx4SurfaceControl surface, const QRectF& target,
                           ControlId id, double currentRaw, double targetRaw,
                           const QColor& glow, bool animate) {
        const double current = surfaceValue(id, currentRaw);
        const double wanted = surfaceValue(id, targetRaw);
        QPointF from;
        QPointF to;
        if (surface == Flx4SurfaceControl::Crossfader) {
            from = QPointF(target.left() + current * target.width(),
                           target.center().y());
            to = QPointF(target.left() + wanted * target.width(),
                         target.center().y());
        } else if (surface == Flx4SurfaceControl::ChannelFader) {
            from = QPointF(target.center().x(),
                           target.bottom() - current * target.height());
            to = QPointF(target.center().x(),
                         target.bottom() - wanted * target.height());
        } else if (surface == Flx4SurfaceControl::TempoFader) {
            from = QPointF(target.center().x(),
                           target.top() + current * target.height());
            to = QPointF(target.center().x(),
                         target.top() + wanted * target.height());
        } else {
            const auto dialPoint = [&target](double value) {
                const double radians = (225.0 + value * 270.0) * kPi / 180.0;
                const double radius = std::min(target.width(), target.height()) *
                                      0.42;
                return target.center() + QPointF(std::cos(radians) * radius,
                                                  std::sin(radians) * radius);
            };
            from = dialPoint(current);
            to = dialPoint(wanted);
        }
        p.setPen(QPen(QColor(255, 255, 255, 190), 2, Qt::DashLine));
        p.drawLine(from, to);
        p.setPen(QPen(Qt::white, 3));
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawEllipse(to, 5, 5);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        const QPointF moving = animate ? from + (to - from) * 0.6 : from;
        p.drawEllipse(moving, 4, 4);
    };

    for (const SoftTakeoverState& state : takeovers_) {
        const auto map = flx4TutorialMapping(state.control, state.targetValue);
        if (!map) continue;
        const QRectF target = targetRect(*map, state.deck);
        p.setBrush(QColor(255, 255, 255, pulse_ ? 88 : 45));
        p.setPen(QPen(Qt::white, pulse_ ? 4.0 : 2.5, Qt::DashLine));
        p.drawRoundedRect(target.adjusted(-5, -5, 5, 5), 7, 7);
        if (isAnimatedContinuous(map->surface))
            drawMotion(map->surface, target, state.control,
                       state.hardwareKnown
                           ? state.hardwareValue
                           : liveControlValue(state.deck, state.control),
                       state.targetValue, QColor(255, 255, 255, 210), false);
    }

    if (mapping_ && expected_) {
        const QRectF target = targetRect(*mapping_, expected_->deck);
        const QColor glow = activationEnabled_
                                ? (expected_->deck == 1 ? deckAccent(1)
                                                        : transitionEntryColor())
                                : QColor(0xe8, 0xa8, 0x35);
        p.setBrush(QColor(glow.red(), glow.green(), glow.blue(),
                          pulse_ ? 110 : 56));
        p.setPen(QPen(glow, pulse_ ? 5.0 : 3.0));
        p.drawRoundedRect(target.adjusted(-5, -5, 5, 5), 7, 7);
        if (mapping_->needsFxAssignment && expected_->deck >= 0)
            p.drawRoundedRect(
                fxAssignmentRect(expected_->deck).adjusted(-4, -4, 4, 4),
                6, 6);
        if (isAnimatedContinuous(mapping_->surface))
            drawMotion(mapping_->surface, target, expected_->id,
                       hardwareValue_.value_or(
                           liveControlValue(expected_->deck, expected_->id)),
                       expected_->value, glow, true);
    }
}

void Flx4TutorialWidget::mousePressEvent(QMouseEvent* event)
{
    const QPointF point = toDesign(event->position());
    if (QRectF(961, 13, 25, 23).contains(point)) {
        emit abortRequested();
        event->accept();
        return;
    }
    if (expected_ && mapping_ && activationEnabled_ &&
        targetRect(*mapping_, expected_->deck).adjusted(-7, -7, 7, 7)
            .contains(point)) {
        if (mapping_->surface == Flx4SurfaceControl::PerformancePad &&
            gesturePadMode_ >= 0 && mapping_->pad >= 0) {
            bool pressed = true;
            if (expected_->id == ControlId::Cue ||
                (expected_->id >= ControlId::HotCue1 &&
                 expected_->id <= ControlId::HotCue8) ||
                (expected_->id >= ControlId::SavedLoop1 &&
                 expected_->id <= ControlId::SavedLoop8))
                pressed = expected_->value >= 0.5;
            emit performancePadActivated(expected_->deck, gesturePadMode_,
                                         mapping_->pad, pressed);
        } else {
            emit controlActivated(*expected_);
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void Flx4TutorialWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        emit abortRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace gvt
