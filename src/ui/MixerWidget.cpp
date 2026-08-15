#include "MixerWidget.h"
#include "Theme.h"

#include <QDial>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gvt {

static constexpr int kSteps = 1000; // slider/dial resolution for 0..1

static int toSteps(double v)
{
    return (int)std::lround(std::clamp(v, 0.0, 1.0) * kSteps);
}
static double fromSteps(int v) { return (double)v / kSteps; }

static QLabel* caption(const QString& text)
{
    auto* l = new QLabel(text);
    l->setAlignment(Qt::AlignHCenter);
    l->setStyleSheet(QStringLiteral("color:%1; font-size:9px;")
                         .arg(themeDimText().name()));
    return l;
}

MixerWidget::MixerWidget(ControlBus* bus, QWidget* parent)
    : QWidget(parent), bus_(bus)
{
    setObjectName(QStringLiteral("mixerWidget"));
    setProperty("panel", true);
    setMaximumHeight(110);

    // One horizontal row: [channel A] | crossfader | [channel B].
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(10);

    row->addWidget(buildStrip(0));

    // Center: crossfader (~200 px).
    auto* xfCol = new QVBoxLayout;
    xfCol->setSpacing(2);
    xfCol->addStretch(1);
    crossfader_ = new QSlider(Qt::Horizontal);
    crossfader_->setRange(0, kSteps);
    crossfader_->setValue(0);
    crossfader_->setFixedWidth(200);
    crossfader_->setToolTip(tr("Crossfader: A ↔ B"));
    connect(crossfader_, &QSlider::valueChanged, this, [this](int v) {
        if (!crossfader_->signalsBlocked())
            bus_->dispatch(
                ControlEvent{kNoDeck, ControlId::Crossfader, fromSteps(v)},
                Origin::Ui);
    });
    auto* xfRow = new QHBoxLayout;
    xfRow->setSpacing(4);
    auto* aLbl = new QLabel(QStringLiteral("A"));
    aLbl->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                            .arg(deckAccent(0).name()));
    auto* bLbl = new QLabel(QStringLiteral("B"));
    bLbl->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                            .arg(deckAccent(1).name()));
    xfRow->addWidget(aLbl);
    xfRow->addWidget(crossfader_);
    xfRow->addWidget(bLbl);
    xfCol->addLayout(xfRow);
    xfCol->addWidget(caption(tr("CROSSFADER")));
    xfCol->addStretch(1);
    row->addLayout(xfCol);

    row->addWidget(buildStrip(1));
    row->addStretch(1);

    connect(bus_, &ControlBus::eventDispatched, this,
            &MixerWidget::onBusEvent);
}

void MixerWidget::wireDial(QDial* d, int deck, ControlId id, double initial)
{
    d->setRange(0, kSteps);
    d->setValue(toSteps(initial));
    d->setNotchesVisible(true);
    d->setFixedSize(32, 32);
    connect(d, &QDial::valueChanged, this, [this, d, deck, id](int v) {
        if (!d->signalsBlocked())
            bus_->dispatch(ControlEvent{deck, id, fromSteps(v)}, Origin::Ui);
    });
}

QWidget* MixerWidget::buildStrip(int deck)
{
    // Inline horizontal channel strip:
    // [A] [TRIM] [HI] [MID] [LOW] [fader]
    auto* w = new QWidget(this);
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    auto* deckLbl = new QLabel(deck == 0 ? QStringLiteral("A")
                                         : QStringLiteral("B"));
    deckLbl->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                               .arg(deckAccent(deck).name()));
    row->addWidget(deckLbl);

    Strip& s = strips_[deck];
    auto addKnob = [&](QDial*& dial, ControlId id,
                       const QString& name) -> QLabel* {
        auto* col = new QVBoxLayout;
        col->setSpacing(1);
        dial = new QDial(w);
        wireDial(dial, deck, id, 0.5);
        col->addStretch(1);
        col->addWidget(dial, 0, Qt::AlignHCenter);
        QLabel* cap = caption(name);
        col->addWidget(cap);
        col->addStretch(1);
        row->addLayout(col);
        return cap;
    };
    addKnob(s.trim, ControlId::Trim, tr("TRIM"));
    addKnob(s.eqHigh, ControlId::EqHigh, tr("HI"));
    addKnob(s.eqMid, ControlId::EqMid, tr("MID"));
    addKnob(s.eqLow, ControlId::EqLow, tr("LOW"));

    // DJ filter knob: 0.5 = off, <0.5 low-pass, >0.5 high-pass. The caption
    // turns cyan (LPF) / orange (HPF) off-center; double-click re-centers.
    s.filterLabel = addKnob(s.filter, ControlId::Filter, tr("FILTER"));
    s.filter->setToolTip(
        tr("DJ filter: left = low-pass, right = high-pass, "
           "double-click = off"));
    s.filter->installEventFilter(this);
    connect(s.filter, &QDial::valueChanged, this, [this, deck](int v) {
        updateFilterLabel(deck, fromSteps(v));
    });
    updateFilterLabel(deck, 0.5);

    auto* faderCol = new QVBoxLayout;
    faderCol->setSpacing(1);
    s.fader = new QSlider(Qt::Vertical, w);
    s.fader->setRange(0, kSteps);
    s.fader->setValue(kSteps);
    s.fader->setFixedHeight(64);
    // The app stylesheet's QSlider:vertical min-height (84px) would defeat
    // the compact strip; override locally.
    s.fader->setStyleSheet(
        QStringLiteral("QSlider:vertical { min-height: 64px; }"));
    connect(s.fader, &QSlider::valueChanged, this, [this, deck](int v) {
        QSlider* f = strips_[deck].fader;
        if (!f->signalsBlocked())
            bus_->dispatch(ControlEvent{deck, ControlId::Fader, fromSteps(v)},
                           Origin::Ui);
    });
    faderCol->addWidget(s.fader, 0, Qt::AlignHCenter);
    faderCol->addWidget(caption(tr("FADER")));
    row->addLayout(faderCol);
    return w;
}

bool MixerWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        for (int deck = 0; deck < 2; ++deck) {
            if (watched == strips_[deck].filter) {
                // Re-center = filter off. Not signal-blocked: this is a
                // user action and must dispatch onto the bus like a turn.
                strips_[deck].filter->setValue(toSteps(0.5));
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MixerWidget::updateFilterLabel(int deck, double value)
{
    QLabel* l = strips_[deck].filterLabel;
    if (!l) return;
    QString color;
    if (value < 0.47)
        color = QStringLiteral("#35c8e8"); // LPF cyan
    else if (value > 0.53)
        color = QStringLiteral("#f08c28"); // HPF orange
    else
        color = themeDimText().name(); // centered = off, gray
    l->setStyleSheet(
        QStringLiteral("color:%1; font-size:9px;").arg(color));
}

void MixerWidget::onBusEvent(const ControlEvent& e, Origin origin)
{
    Q_UNUSED(origin);
    if (e.id == ControlId::Crossfader) {
        QSignalBlocker block(crossfader_);
        crossfader_->setValue(toSteps(e.value));
        return;
    }
    if (e.deck != 0 && e.deck != 1) return;
    Strip& s = strips_[e.deck];
    QWidget* target = nullptr;
    switch (e.id) {
    case ControlId::Trim:   target = s.trim; break;
    case ControlId::EqHigh: target = s.eqHigh; break;
    case ControlId::EqMid:  target = s.eqMid; break;
    case ControlId::EqLow:  target = s.eqLow; break;
    case ControlId::Filter: target = s.filter; break;
    case ControlId::Fader:  target = s.fader; break;
    default: return;
    }
    if (e.id == ControlId::Filter)
        updateFilterLabel(e.deck, e.value); // blocked signals skip the slot
    QSignalBlocker block(target);
    if (auto* dial = qobject_cast<QDial*>(target))
        dial->setValue(toSteps(e.value));
    else if (auto* slider = qobject_cast<QSlider*>(target))
        slider->setValue(toSteps(e.value));
}

} // namespace gvt
