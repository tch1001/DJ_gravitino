#pragma once

#include <QPainter>
#include <QPaintEvent>
#include <QWidget>

namespace gvt {

// A mouse-transparent white static veil placed directly over one virtual
// control while its FLX4 counterpart is waiting for pickup.
class PickupFuzzOverlay final : public QWidget {
public:
    explicit PickupFuzzOverlay(QWidget* target)
        : QWidget(target)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(target->rect());
        raise();
        show();
    }

    void setPulse(bool pulse)
    {
        if (pulse_ == pulse) return;
        pulse_ = pulse;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(255, 255, 255, pulse_ ? 92 : 58));

        // Deterministic high-contrast speckle reads as white "fuzz" without
        // allocating an image or depending on a random generator per frame.
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

} // namespace gvt
