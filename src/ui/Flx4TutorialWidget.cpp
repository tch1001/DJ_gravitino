#include "Flx4TutorialWidget.h"

#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace gvt {
namespace {

constexpr qreal kDesignWidth = 1000.0;
constexpr qreal kDesignHeight = 650.0;

QRectF padRect(int deck, int pad)
{
    const qreal startX = deck == 0 ? 78.0 : 702.0;
    return QRectF(startX + (pad % 4) * 54.0,
                  472.0 + (pad / 4) * 38.0, 48.0, 30.0);
}

QRectF deckButtonRect(int deck, int slot)
{
    const qreal start = deck == 0 ? 112.0 : 726.0;
    return QRectF(start + slot * 61.0, 550.0, 54.0, 38.0);
}

QRectF loopButtonRect(int deck, int slot)
{
    const qreal start = deck == 0 ? 83.0 : 703.0;
    return QRectF(start + slot * 44.0, 169.0, 39.0, 24.0);
}

qreal channelX(int deck) { return deck == 0 ? 414.0 : 546.0; }

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
    detail_ = tr("Use the physical controller or click the highlighted virtual control");
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
    detail_ = tr("Keep the outgoing track running and follow the recorded sequence");
    warning_.clear();
    activationEnabled_ = false;
    gesturePadMode_ = -1;
    hardwareValue_.reset();
    update();
}

void Flx4TutorialWidget::setHardwareValue(const ControlEvent& event)
{
    if (!expected_ || expected_->deck != event.deck ||
        expected_->id != event.id || !std::isfinite(event.value)) {
        return;
    }
    hardwareValue_ = event.value;
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
    return QRectF(deck == 0 ? 594.0 : 634.0, 285.0, 34.0, 22.0);
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
        return QRectF(deck == 0 ? 388.0 : 538.0, 164.0, 54.0, 24.0);
    case Flx4SurfaceControl::PerformancePad:return padRect(deck, mapping.pad);
    case Flx4SurfaceControl::LoopIn:        return loopButtonRect(deck, 0);
    case Flx4SurfaceControl::LoopOut:       return loopButtonRect(deck, 1);
    case Flx4SurfaceControl::FourBeatExit:  return loopButtonRect(deck, 2);
    case Flx4SurfaceControl::LoopHalve:     return loopButtonRect(deck, 3);
    case Flx4SurfaceControl::LoopDouble:    return loopButtonRect(deck, 4);
    case Flx4SurfaceControl::TempoFader:
        return QRectF(deck == 0 ? 45.0 : 931.0, 225.0, 24.0, 190.0);
    case Flx4SurfaceControl::JogWheel:
        return QRectF(deck == 0 ? 93.0 : 707.0, 224.0, 200.0, 200.0);
    case Flx4SurfaceControl::ChannelFader:  return QRectF(chX - 13.0, 435.0, 26.0, 112.0);
    case Flx4SurfaceControl::Trim:          return QRectF(chX - 16.0, 207.0, 32.0, 32.0);
    case Flx4SurfaceControl::EqHigh:        return QRectF(chX - 16.0, 250.0, 32.0, 32.0);
    case Flx4SurfaceControl::EqMid:         return QRectF(chX - 16.0, 293.0, 32.0, 32.0);
    case Flx4SurfaceControl::EqLow:         return QRectF(chX - 16.0, 336.0, 32.0, 32.0);
    case Flx4SurfaceControl::Filter:        return QRectF(chX - 17.0, 379.0, 34.0, 34.0);
    case Flx4SurfaceControl::Crossfader:    return QRectF(414.0, 565.0, 132.0, 24.0);
    case Flx4SurfaceControl::ChannelCue:    return QRectF(chX - 23.0, 414.0, 46.0, 20.0);
    case Flx4SurfaceControl::MasterCue:     return QRectF(468.0, 214.0, 54.0, 22.0);
    case Flx4SurfaceControl::HeadphoneMix:  return QRectF(477.0, 251.0, 36.0, 36.0);
    case Flx4SurfaceControl::BeatFxSelect:  return QRectF(590.0, 211.0, 82.0, 24.0);
    case Flx4SurfaceControl::BeatFxOn:      return QRectF(590.0, 319.0, 82.0, 26.0);
    case Flx4SurfaceControl::BeatFxWet:     return QRectF(614.0, 355.0, 36.0, 36.0);
    case Flx4SurfaceControl::BeatFxBeats:   return QRectF(590.0, 247.0, 82.0, 25.0);
    }
    return {};
}

void Flx4TutorialWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(8, 10, 13, 245));

    const QRectF fitted = fittedDesignRect();
    p.translate(fitted.left(), fitted.top());
    p.scale(fitted.width() / kDesignWidth, fitted.height() / kDesignHeight);

    p.setPen(QPen(QColor(74, 80, 92), 2));
    p.setBrush(QColor(22, 24, 29));
    p.drawRoundedRect(QRectF(3, 3, 994, 644), 14, 14);

    QFont titleFont = p.font();
    titleFont.setBold(true);
    titleFont.setPixelSize(17);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    p.setFont(titleFont);
    p.setPen(themeText());
    p.drawText(QRectF(28, 18, 730, 28), Qt::AlignVCenter,
               tr("FLX4 TRANSITION TUTOR · %1").arg(transitionName_));

    p.setPen(QPen(QColor(74, 80, 92), 1));
    p.setBrush(QColor(42, 46, 55));
    p.drawRoundedRect(QRectF(941, 17, 34, 28), 5, 5);
    p.setPen(themeText());
    p.drawText(QRectF(941, 17, 34, 28), Qt::AlignCenter, QStringLiteral("×"));

    QFont instructionFont = p.font();
    instructionFont.setPixelSize(23);
    instructionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    p.setFont(instructionFont);
    p.setPen(mapping_.has_value() ? themeText() : QColor(0xe8, 0xa8, 0x35));
    p.drawText(QRectF(28, 51, 780, 34), Qt::AlignVCenter, instruction_);

    QFont infoFont = p.font();
    infoFont.setPixelSize(13);
    p.setFont(infoFont);
    p.setPen(themeDimText());
    p.drawText(QRectF(28, 87, 760, 23), Qt::AlignVCenter, detail_);
    if (expected_) {
        const QString countdown = beatsAhead_ > 0.05
            ? tr("IN %1 BEATS").arg(std::max(0.0, beatsAhead_), 0, 'f', 1)
            : tr("NOW");
        QFont countdownFont = p.font();
        countdownFont.setBold(true);
        countdownFont.setPixelSize(21);
        p.setFont(countdownFont);
        p.setPen(beatsAhead_ <= 0.25 ? QColor(0x4c, 0xd9, 0x64)
                                     : transitionEntryColor());
        p.drawText(QRectF(800, 53, 170, 34), Qt::AlignRight | Qt::AlignVCenter,
                   countdown);
    }

    p.setFont(infoFont);
    if (!warning_.isEmpty()) {
        p.setPen(QColor(0xe8, 0xa8, 0x35));
        p.drawText(QRectF(28, 112, 944, 23), Qt::AlignVCenter,
                   QStringLiteral("⚠ ") + warning_);
    } else if (!feedback_.isEmpty()) {
        p.setPen(feedbackColor_);
        p.drawText(QRectF(28, 112, 944, 23), Qt::AlignVCenter, feedback_);
    } else {
        p.setPen(themeDimText());
        p.drawText(QRectF(28, 112, 944, 23), Qt::AlignVCenter,
                   tr("Esc closes the tutorial · highlighted controls are clickable"));
    }

    const QRectF controller(25, 143, 950, 470);
    p.setPen(QPen(QColor(76, 82, 94), 2));
    p.setBrush(QColor(31, 34, 41));
    p.drawRoundedRect(controller, 12, 12);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(25, 28, 34));
    p.drawRoundedRect(QRectF(359, 153, 242, 448), 8, 8);

    auto drawButton = [&p](const QRectF& r, const QString& label,
                           const QColor& text = themeDimText()) {
        p.setPen(QPen(QColor(73, 79, 91), 1));
        p.setBrush(QColor(48, 52, 62));
        p.drawRoundedRect(r, 3, 3);
        QFont f = p.font(); f.setPixelSize(9); f.setBold(true); p.setFont(f);
        p.setPen(text);
        p.drawText(r, Qt::AlignCenter, label);
    };
    auto drawKnob = [&p](qreal x, qreal y, const QString& label) {
        p.setPen(QPen(QColor(92, 99, 113), 2));
        p.setBrush(QColor(40, 44, 52));
        p.drawEllipse(QPointF(x, y), 13, 13);
        p.drawLine(QPointF(x, y), QPointF(x, y - 9));
        QFont f = p.font(); f.setPixelSize(8); p.setFont(f);
        p.setPen(themeDimText());
        p.drawText(QRectF(x - 32, y + 14, 64, 12), Qt::AlignCenter, label);
    };

    for (int deck = 0; deck < 2; ++deck) {
        const QColor accent = deckAccent(deck);
        const qreal jogX = deck == 0 ? 193.0 : 807.0;
        QFont deckFont = p.font(); deckFont.setPixelSize(13); deckFont.setBold(true);
        p.setFont(deckFont); p.setPen(accent);
        p.drawText(QRectF(deck == 0 ? 42.0 : 890.0, 154, 55, 20),
                   Qt::AlignCenter, deck == 0 ? tr("DECK A") : tr("DECK B"));

        const QStringList loopLabels {tr("IN"), tr("OUT"), tr("4/EXIT"),
                                      tr("1/2"), tr("2×")};
        for (int i = 0; i < loopLabels.size(); ++i)
            drawButton(loopButtonRect(deck, i), loopLabels[i]);

        p.setPen(QPen(QColor(79, 86, 99), 3));
        p.setBrush(QColor(37, 40, 48));
        p.drawEllipse(QPointF(jogX, 324), 96, 96);
        p.setPen(QPen(QColor(56, 61, 71), 2));
        p.drawEllipse(QPointF(jogX, 324), 64, 64);
        p.drawEllipse(QPointF(jogX, 324), 24, 24);
        p.setPen(themeDimText());
        p.drawText(QRectF(jogX - 55, 313, 110, 22), Qt::AlignCenter, tr("JOG"));

        const qreal tempoX = deck == 0 ? 57.0 : 943.0;
        p.setPen(QPen(QColor(77, 83, 95), 5));
        p.drawLine(QPointF(tempoX, 235), QPointF(tempoX, 405));
        p.setBrush(QColor(185, 191, 202)); p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(tempoX - 9, 310, 18, 24), 3, 3);
        QFont tiny = p.font(); tiny.setPixelSize(8); p.setFont(tiny);
        p.setPen(themeDimText());
        p.drawText(QRectF(tempoX - 28, 211, 56, 14), Qt::AlignCenter, tr("TEMPO"));

        for (int pad = 0; pad < 8; ++pad) {
            const QColor color = hotCueColor(pad);
            drawButton(padRect(deck, pad), QString::number(pad + 1), color);
        }
        p.setPen(themeDimText());
        p.drawText(QRectF(deck == 0 ? 78.0 : 702.0, 453, 210, 16),
                   Qt::AlignLeft, tr("PERFORMANCE PADS"));

        drawButton(deckButtonRect(deck, 0), tr("SYNC"), accent);
        drawButton(deckButtonRect(deck, 1), tr("CUE"));
        drawButton(deckButtonRect(deck, 2), tr("PLAY"), accent);
    }

    // Browser/load context at the top of the central strip.
    drawButton(QRectF(388, 164, 54, 24), tr("LOAD A"), deckAccent(0));
    drawButton(QRectF(538, 164, 54, 24), tr("LOAD B"), deckAccent(1));
    drawKnob(490, 174, tr("BROWSE"));

    for (int deck = 0; deck < 2; ++deck) {
        const qreal x = channelX(deck);
        drawKnob(x, 223, tr("TRIM"));
        drawKnob(x, 266, tr("HIGH"));
        drawKnob(x, 309, tr("MID"));
        drawKnob(x, 352, tr("LOW"));
        drawKnob(x, 396, tr("FILTER"));
        drawButton(QRectF(x - 23, 414, 46, 20), tr("CH CUE"), deckAccent(deck));
        p.setPen(QPen(QColor(78, 84, 96), 5));
        p.drawLine(QPointF(x, 444), QPointF(x, 540));
        p.setPen(Qt::NoPen); p.setBrush(QColor(186, 192, 202));
        p.drawRoundedRect(QRectF(x - 10, 484, 20, 17), 3, 3);
    }

    drawButton(QRectF(468, 214, 54, 22), tr("MASTER CUE"));
    drawKnob(495, 269, tr("CUE / MASTER"));
    p.setPen(QPen(QColor(78, 84, 96), 5));
    p.drawLine(QPointF(424, 577), QPointF(536, 577));
    p.setPen(Qt::NoPen); p.setBrush(QColor(186, 192, 202));
    p.drawRoundedRect(QRectF(464, 568, 23, 18), 3, 3);
    p.setPen(themeDimText());
    p.drawText(QRectF(414, 590, 132, 12), Qt::AlignCenter, tr("CROSSFADER"));

    // Shared Beat FX panel and deck assignment.
    p.setPen(themeDimText());
    p.drawText(QRectF(590, 190, 82, 17), Qt::AlignCenter, tr("BEAT FX"));
    drawButton(QRectF(590, 211, 82, 24), tr("FX SELECT"));
    drawButton(QRectF(590, 247, 38, 25), tr("◀ BEAT"));
    drawButton(QRectF(634, 247, 38, 25), tr("BEAT ▶"));
    drawButton(fxAssignmentRect(0), tr("A"), deckAccent(0));
    drawButton(fxAssignmentRect(1), tr("B"), deckAccent(1));
    drawButton(QRectF(590, 319, 82, 26), tr("FX ON"));
    drawKnob(632, 373, tr("LEVEL/DEPTH"));

    if (mapping_ && expected_) {
        const QRectF target = targetRect(*mapping_, expected_->deck);
        const QColor glow = activationEnabled_
                                ? (expected_->deck == 1 ? deckAccent(1)
                                                        : transitionEntryColor())
                                : QColor(0xe8, 0xa8, 0x35);
        p.setBrush(QColor(glow.red(), glow.green(), glow.blue(),
                          pulse_ ? 105 : 55));
        p.setPen(QPen(glow, pulse_ ? 5.0 : 3.0));
        p.drawRoundedRect(target.adjusted(-5, -5, 5, 5), 7, 7);
        if (mapping_->needsFxAssignment && expected_->deck >= 0) {
            const QRectF assign = fxAssignmentRect(expected_->deck);
            p.drawRoundedRect(assign.adjusted(-4, -4, 4, 4), 6, 6);
        }
        if (mapping_->padMode != Flx4PadMode::None) {
            p.setPen(glow);
            QFont modeFont = p.font(); modeFont.setBold(true); modeFont.setPixelSize(11);
            p.setFont(modeFont);
            QString mode;
            switch (mapping_->padMode) {
            case Flx4PadMode::HotCue:   mode = tr("HOT CUE"); break;
            case Flx4PadMode::PadFx1:    mode = tr("PAD FX1"); break;
            case Flx4PadMode::BeatJump:  mode = tr("BEAT JUMP"); break;
            case Flx4PadMode::Custom:    mode = tr("CUSTOM"); break;
            case Flx4PadMode::Keyboard:  mode = tr("KEYBOARD"); break;
            case Flx4PadMode::PadFx2:    mode = tr("PAD FX2"); break;
            case Flx4PadMode::BeatLoop:  mode = tr("BEAT LOOP"); break;
            case Flx4PadMode::KeyShift:  mode = tr("KEY SHIFT"); break;
            case Flx4PadMode::None:      break;
            }
            p.drawText(QRectF(expected_->deck == 0 ? 78.0 : 702.0, 434,
                              210, 17), Qt::AlignLeft,
                       tr("PAD MODE: %1").arg(mode));
        }

        // Continuous tutorial moves get an actual motion cue, not just a
        // blinking halo: a white ghost marker is the recorded target and the
        // animated dot travels from the last physical FLX4 value toward it.
        if (isAnimatedContinuous(mapping_->surface)) {
            const double targetValue = surfaceValue(
                expected_->id, expected_->value);
            const double currentValue = surfaceValue(
                expected_->id,
                hardwareValue_.value_or(expected_->id == ControlId::Tempo
                                            ? 1.0 : 0.5));
            QPointF from;
            QPointF to;
            if (mapping_->surface == Flx4SurfaceControl::Crossfader) {
                from = QPointF(target.left() + currentValue * target.width(),
                               target.center().y());
                to = QPointF(target.left() + targetValue * target.width(),
                             target.center().y());
            } else if (mapping_->surface ==
                           Flx4SurfaceControl::ChannelFader) {
                from = QPointF(target.center().x(),
                               target.bottom() - currentValue * target.height());
                to = QPointF(target.center().x(),
                             target.bottom() - targetValue * target.height());
            } else if (mapping_->surface ==
                           Flx4SurfaceControl::TempoFader) {
                from = QPointF(target.center().x(),
                               target.top() + currentValue * target.height());
                to = QPointF(target.center().x(),
                             target.top() + targetValue * target.height());
            } else {
                const auto dialPoint = [&target](double value) {
                    const double radians =
                        (225.0 + value * 270.0) * 3.141592653589793 / 180.0;
                    const double radius = std::min(target.width(),
                                                   target.height()) * 0.42;
                    return target.center() +
                        QPointF(std::cos(radians) * radius,
                                std::sin(radians) * radius);
                };
                from = dialPoint(currentValue);
                to = dialPoint(targetValue);
            }
            const double progress =
                (static_cast<double>(animationPhase_) + 1.0) / 8.0;
            const QPointF moving = from + (to - from) * progress;
            p.setPen(QPen(QColor(255, 255, 255, 180), 2, Qt::DashLine));
            p.drawLine(from, to);
            p.setPen(QPen(Qt::white, 3));
            p.setBrush(QColor(255, 255, 255, pulse_ ? 230 : 150));
            p.drawEllipse(to, 6, 6);
            p.setPen(Qt::NoPen);
            p.drawEllipse(moving, 4, 4);
            QFont targetFont = p.font();
            targetFont.setBold(true);
            targetFont.setPixelSize(9);
            p.setFont(targetFont);
            p.setPen(Qt::white);
            p.drawText(target.adjusted(-34, -22, 34, -4),
                       Qt::AlignCenter, tr("TARGET"));
        }
    }
}

void Flx4TutorialWidget::mousePressEvent(QMouseEvent* event)
{
    const QPointF point = toDesign(event->position());
    if (QRectF(941, 17, 34, 28).contains(point)) {
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
                 expected_->id <= ControlId::HotCue8)) {
                pressed = expected_->value >= 0.5;
            }
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
