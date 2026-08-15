#include "MixerWidget.h"
#include "Theme.h"

#include <QDial>
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
    auto addKnob = [&](QDial*& dial, ControlId id, const QString& name) {
        auto* col = new QVBoxLayout;
        col->setSpacing(1);
        dial = new QDial(w);
        wireDial(dial, deck, id, 0.5);
        col->addStretch(1);
        col->addWidget(dial, 0, Qt::AlignHCenter);
        col->addWidget(caption(name));
        col->addStretch(1);
        row->addLayout(col);
    };
    addKnob(s.trim, ControlId::Trim, tr("TRIM"));
    addKnob(s.eqHigh, ControlId::EqHigh, tr("HI"));
    addKnob(s.eqMid, ControlId::EqMid, tr("MID"));
    addKnob(s.eqLow, ControlId::EqLow, tr("LOW"));

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
    case ControlId::Fader:  target = s.fader; break;
    default: return;
    }
    QSignalBlocker block(target);
    if (auto* dial = qobject_cast<QDial*>(target))
        dial->setValue(toSteps(e.value));
    else if (auto* slider = qobject_cast<QSlider*>(target))
        slider->setValue(toSteps(e.value));
}

} // namespace gvt
