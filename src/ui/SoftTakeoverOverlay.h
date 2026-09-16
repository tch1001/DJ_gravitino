#pragma once

#include <QDial>
#include <QHelpEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QSlider>
#include <QToolTip>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace gvt {

inline void drawControlTargetMarker(QPainter& painter, const QRect& bounds,
                                    QWidget* target, double fraction,
                                    const QColor& color, qreal width = 2.0)
{
    if (!std::isfinite(fraction)) return;
    fraction = std::clamp(fraction, 0.0, 1.0);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, width));
    painter.setBrush(color);
    if (auto* slider = qobject_cast<QSlider*>(target)) {
        if (slider->orientation() == Qt::Vertical) {
            const int y = bounds.bottom() - static_cast<int>(std::lround(
                fraction * std::max(1, bounds.height() - 1)));
            painter.drawLine(bounds.left() + 1, y, bounds.right() - 1, y);
            painter.drawEllipse(QPoint(bounds.center().x(), y), 3, 3);
        } else {
            const int x = bounds.left() + static_cast<int>(std::lround(
                fraction * std::max(1, bounds.width() - 1)));
            painter.drawLine(x, bounds.top() + 1, x, bounds.bottom() - 1);
            painter.drawEllipse(QPoint(x, bounds.center().y()), 3, 3);
        }
        return;
    }
    if (qobject_cast<QDial*>(target)) {
        const QPointF center = bounds.center();
        const double radius = std::max(2.0,
            std::min(bounds.width(), bounds.height()) * 0.42);
        constexpr double kPi = 3.14159265358979323846;
        const double angle = (225.0 - 270.0 * fraction) * kPi / 180.0;
        const QPointF inner(center.x() + std::cos(angle) * (radius - 5.0),
                            center.y() - std::sin(angle) * (radius - 5.0));
        const QPointF outer(center.x() + std::cos(angle) * radius,
                            center.y() - std::sin(angle) * radius);
        painter.drawLine(inner, outer);
        painter.drawEllipse(outer, 2.5, 2.5);
        return;
    }
    // Discrete/state widgets have no continuous groove. Keep a compact
    // marker visible at either side so Off/On (or low/high enum state) still
    // has a target without covering the button label.
    const int x = fraction >= 0.5 ? bounds.right() - 3 : bounds.left() + 3;
    painter.drawEllipse(QPointF(x, bounds.top() + 3), 2.5, 2.5);
}

class PickupFuzzOverlay final : public QWidget {
public:
    explicit PickupFuzzOverlay(QWidget* target) : QWidget(target)
    {
        setObjectName(QStringLiteral("pickupTargetOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(target->rect());
        raise();
        show();
    }

    void setPulse(bool pulse) { pulse_ = pulse; update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(255, 255, 255, pulse_ ? 92 : 58));
        painter.setPen(QColor(255, 255, 255, pulse_ ? 220 : 150));
        const int phase = pulse_ ? 3 : 0;
        for (int y = phase; y < height(); y += 5)
            for (int x = (y + phase) % 7; x < width(); x += 7)
                painter.drawPoint(x, y);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, pulse_ ? 2.5 : 1.5, Qt::DashLine));
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
    }

private:
    bool pulse_ = false;
};

class SetupMismatchOverlay final : public QWidget {
public:
    SetupMismatchOverlay(QWidget* target, double targetFraction,
                         QString targetToolTip = {})
        : QWidget(target), target_(target), targetFraction_(targetFraction),
          targetToolTip_(std::move(targetToolTip))
    {
        setObjectName(QStringLiteral("setupTargetOverlay"));
        setProperty("targetFraction", targetFraction_);
        setProperty("targetText", targetToolTip_);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(target->rect());
        if (!targetToolTip_.isEmpty()) target->installEventFilter(this);
        raise();
        show();
    }

    ~SetupMismatchOverlay() override
    {
        if (target_) target_->removeEventFilter(this);
    }

    void setPulse(bool pulse) { pulse_ = pulse; update(); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == target_ && event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            QToolTip::showText(help->globalPos(), targetToolTip_, target_);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(232, 168, 53, pulse_ ? 92 : 48));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 196, 76), pulse_ ? 3.0 : 1.8,
                            Qt::DashLine));
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        drawControlTargetMarker(painter, rect().adjusted(2, 2, -2, -2),
                                target_, targetFraction_,
                                QColor(255, 214, 92), 2.5);
    }

private:
    QPointer<QWidget> target_;
    double targetFraction_ = 0.0;
    QString targetToolTip_;
    bool pulse_ = false;
};

class HardwareGhostOverlay final : public QWidget {
public:
    HardwareGhostOverlay(QWidget* target, double hardwareFraction, bool known,
                         QString hardwareToolTip = {})
        : QWidget(target), target_(target), fraction_(hardwareFraction),
          known_(known), hardwareToolTip_(std::move(hardwareToolTip))
    {
        setObjectName(QStringLiteral("hardwareGhostOverlay"));
        setProperty("hardwareKnown", known_);
        setProperty("hardwareFraction", fraction_);
        setProperty("hardwareText", hardwareToolTip_);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(target->rect());
        if (!hardwareToolTip_.isEmpty()) target->installEventFilter(this);
        raise();
        show();
    }

    ~HardwareGhostOverlay() override
    {
        if (target_) target_->removeEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == target_ && event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            QToolTip::showText(help->globalPos(), hardwareToolTip_, target_);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        if (known_)
            drawControlTargetMarker(painter, rect().adjusted(1, 1, -1, -1),
                                    target_, fraction_, QColor(83, 210, 242), 2.0);
        else {
            painter.setPen(QColor(120, 132, 148, 210));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("?"));
        }
    }

private:
    QPointer<QWidget> target_;
    double fraction_ = 0.0;
    bool known_ = false;
    QString hardwareToolTip_;
};

class TutorialTargetOverlay final : public QWidget {
public:
    TutorialTargetOverlay(QWidget* target, double targetFraction, bool mismatch,
                          QString targetToolTip = {})
        : QWidget(target), target_(target), fraction_(targetFraction),
          mismatch_(mismatch), targetToolTip_(std::move(targetToolTip))
    {
        setObjectName(QStringLiteral("tutorialTargetOverlay"));
        setProperty("targetFraction", fraction_);
        setProperty("targetMismatch", mismatch_);
        setProperty("targetText", targetToolTip_);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(target->rect());
        if (!targetToolTip_.isEmpty()) target->installEventFilter(this);
        raise();
        show();
    }

    ~TutorialTargetOverlay() override
    {
        if (target_) target_->removeEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == target_ && event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            QToolTip::showText(help->globalPos(), targetToolTip_, target_);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (mismatch_) {
            painter.fillRect(rect(), QColor(76, 217, 100, 34));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(76, 217, 100, 210), 2.0, Qt::DashLine));
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        }
        drawControlTargetMarker(painter, rect().adjusted(2, 2, -2, -2),
                                target_, fraction_,
                                QColor(92, 235, 120, mismatch_ ? 235 : 130),
                                mismatch_ ? 2.5 : 1.5);
    }

private:
    QPointer<QWidget> target_;
    double fraction_ = 0.0;
    bool mismatch_ = false;
    QString targetToolTip_;
};

} // namespace gvt
