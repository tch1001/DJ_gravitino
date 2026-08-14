#include "MixerWidget.h"
#include "Theme.h"

#include <QDial>
#include <QGridLayout>
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
    l->setStyleSheet(QStringLiteral("color:%1; font-size:10px;")
                         .arg(themeDimText().name()));
    return l;
}

MixerWidget::MixerWidget(ControlBus* bus, QWidget* parent)
    : QWidget(parent), bus_(bus)
{
    setObjectName(QStringLiteral("mixerWidget"));
    setProperty("panel", true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* header = new QLabel(tr("MIXER"));
    header->setAlignment(Qt::AlignHCenter);
    header->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                         "letter-spacing:2px;")
                              .arg(themeText().name()));
    root->addWidget(header);

    auto* stripsRow = new QHBoxLayout;
    stripsRow->setSpacing(10);
    stripsRow->addWidget(buildStrip(0));
    stripsRow->addWidget(buildStrip(1));
    root->addLayout(stripsRow, 1);

    // Crossfader.
    crossfader_ = new QSlider(Qt::Horizontal);
    crossfader_->setRange(0, kSteps);
    crossfader_->setValue(0);
    crossfader_->setToolTip(tr("Crossfader: A ↔ B"));
    connect(crossfader_, &QSlider::valueChanged, this, [this](int v) {
        if (!crossfader_->signalsBlocked())
            bus_->dispatch(
                ControlEvent{kNoDeck, ControlId::Crossfader, fromSteps(v)},
                Origin::Ui);
    });
    auto* xfRow = new QHBoxLayout;
    auto* aLbl = new QLabel(QStringLiteral("A"));
    aLbl->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                            .arg(deckAccent(0).name()));
    auto* bLbl = new QLabel(QStringLiteral("B"));
    bLbl->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                            .arg(deckAccent(1).name()));
    xfRow->addWidget(aLbl);
    xfRow->addWidget(crossfader_, 1);
    xfRow->addWidget(bLbl);
    root->addLayout(xfRow);
    root->addWidget(caption(tr("CROSSFADER")));

    connect(bus_, &ControlBus::eventDispatched, this,
            &MixerWidget::onBusEvent);
}

void MixerWidget::wireDial(QDial* d, int deck, ControlId id, double initial)
{
    d->setRange(0, kSteps);
    d->setValue(toSteps(initial));
    d->setNotchesVisible(true);
    d->setFixedSize(44, 44);
    connect(d, &QDial::valueChanged, this, [this, d, deck, id](int v) {
        if (!d->signalsBlocked())
            bus_->dispatch(ControlEvent{deck, id, fromSteps(v)}, Origin::Ui);
    });
}

QWidget* MixerWidget::buildStrip(int deck)
{
    auto* w = new QWidget(this);
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);

    auto* deckLbl = new QLabel(deck == 0 ? QStringLiteral("A")
                                         : QStringLiteral("B"));
    deckLbl->setAlignment(Qt::AlignHCenter);
    deckLbl->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                               .arg(deckAccent(deck).name()));
    col->addWidget(deckLbl);

    Strip& s = strips_[deck];
    s.trim = new QDial(w);
    wireDial(s.trim, deck, ControlId::Trim, 0.5);
    col->addWidget(s.trim, 0, Qt::AlignHCenter);
    col->addWidget(caption(tr("TRIM")));

    s.eqHigh = new QDial(w);
    wireDial(s.eqHigh, deck, ControlId::EqHigh, 0.5);
    col->addWidget(s.eqHigh, 0, Qt::AlignHCenter);
    col->addWidget(caption(tr("HI")));

    s.eqMid = new QDial(w);
    wireDial(s.eqMid, deck, ControlId::EqMid, 0.5);
    col->addWidget(s.eqMid, 0, Qt::AlignHCenter);
    col->addWidget(caption(tr("MID")));

    s.eqLow = new QDial(w);
    wireDial(s.eqLow, deck, ControlId::EqLow, 0.5);
    col->addWidget(s.eqLow, 0, Qt::AlignHCenter);
    col->addWidget(caption(tr("LOW")));

    s.fader = new QSlider(Qt::Vertical, w);
    s.fader->setRange(0, kSteps);
    s.fader->setValue(kSteps);
    s.fader->setMinimumHeight(90);
    connect(s.fader, &QSlider::valueChanged, this, [this, deck](int v) {
        QSlider* f = strips_[deck].fader;
        if (!f->signalsBlocked())
            bus_->dispatch(ControlEvent{deck, ControlId::Fader, fromSteps(v)},
                           Origin::Ui);
    });
    col->addWidget(s.fader, 1, Qt::AlignHCenter);
    col->addWidget(caption(tr("FADER")));
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
