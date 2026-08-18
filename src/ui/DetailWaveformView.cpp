#include "DetailWaveformView.h"
#include "Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace gvt {

static constexpr int kLaneHeight = 72;
static constexpr int kLaneGap = 2;
static constexpr double kMinWindowSec = 4.0;
static constexpr double kMaxWindowSec = 30.0;

DetailWaveformView::DetailWaveformView(AudioEngine* engine, QWidget* parent)
    : QWidget(parent), engine_(engine)
{
    setObjectName(QStringLiteral("detailWaveformView"));
    setFixedHeight(2 * kLaneHeight + kLaneGap);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);

    auto makeZoomBtn = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setFixedSize(18, 18);
        b->setCursor(Qt::ArrowCursor);
        b->setStyleSheet(
            "QToolButton { background: rgba(42,46,55,190); color: #d8dce4;"
            " border: 1px solid #4a505c; border-radius: 3px; }"
            "QToolButton:hover { background: rgba(67,73,86,220); }");
        return b;
    };
    zoomInBtn_ = makeZoomBtn(QStringLiteral("+"), tr("Zoom in (less time)"));
    zoomOutBtn_ = makeZoomBtn(QStringLiteral("−"),
                              tr("Zoom out (more time)"));
    connect(zoomInBtn_, &QToolButton::clicked, this,
            &DetailWaveformView::zoomIn);
    connect(zoomOutBtn_, &QToolButton::clicked, this,
            &DetailWaveformView::zoomOut);

    // ~30 Hz repaint while either deck plays (or position/track changed,
    // e.g. a seek from the overview while paused).
    timer_ = new QTimer(this);
    timer_->setInterval(33);
    connect(timer_, &QTimer::timeout, this, [this] {
        bool dirty = false;
        for (int d = 0; d < kNumDecks; ++d) {
            Deck& deck = engine_->deck(d);
            TrackDataPtr t = deck.track();
            double pos = deck.positionSec();
            if (deck.playing.load() || pos != lastPaintPos_[d] ||
                t.get() != lastTrack_[d] ||
                deck.loopStartSec.load() != lastLoopStart_[d] ||
                deck.loopEndSec.load() != lastLoopEnd_[d] ||
                deck.loopActive.load() != lastLoopActive_[d])
                dirty = true;
        }
        if (dirty) update();
    });
    timer_->start();
}

QRect DetailWaveformView::laneRect(int deck) const
{
    return QRect(0, deck * (kLaneHeight + kLaneGap), width(), kLaneHeight);
}

void DetailWaveformView::setTransitionEntry(int deck, double sec)
{
    if (deck < 0 || deck >= kNumDecks) return;
    transitionEntrySec_[deck] = sec;
    update();
}

void DetailWaveformView::setTransitionCues(int deck,
                                           const QList<double>& seconds,
                                           const QStringList& labels)
{
    if (deck < 0 || deck >= kNumDecks) return;
    transitionCueSecs_[deck] = seconds;
    transitionCueLabels_[deck] = labels;
    update();
}

void DetailWaveformView::setWindowSec(double sec)
{
    double clamped = std::clamp(sec, kMinWindowSec, kMaxWindowSec);
    if (clamped != windowSec_) {
        windowSec_ = clamped;
        update();
    }
}

void DetailWaveformView::zoomIn() { setWindowSec(windowSec_ / 1.25); }
void DetailWaveformView::zoomOut() { setWindowSec(windowSec_ * 1.25); }

void DetailWaveformView::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    zoomInBtn_->move(width() - 22, 4);
    zoomOutBtn_->move(width() - 44, 4);
}

void DetailWaveformView::wheelEvent(QWheelEvent* ev)
{
    const int dy = ev->angleDelta().y();
    if (dy == 0) return;
    setWindowSec(dy > 0 ? windowSec_ / 1.25 : windowSec_ * 1.25);
    ev->accept();
}

void DetailWaveformView::mousePressEvent(QMouseEvent* ev)
{
    const int deck = ev->position().y() >= kLaneHeight + kLaneGap ? 1 : 0;
    Deck& d = engine_->deck(deck);
    TrackDataPtr t = d.track();
    if (!t || t->durationSec <= 0.0 || width() <= 0) return;
    const double secPerPx = windowSec_ / width();
    double sec = d.positionSec() +
                 (ev->position().x() - width() / 2.0) * secPerPx;
    d.seekSec(std::clamp(sec, 0.0, t->durationSec));
    update();
}

void DetailWaveformView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), themeBackground().darker(115));
    for (int d = 0; d < kNumDecks; ++d)
        drawLane(p, laneRect(d), d);

    // Fixed center playhead across both lanes.
    const int cx = width() / 2;
    p.setPen(QPen(Qt::white, 2));
    p.drawLine(cx, 0, cx, height());

    p.setPen(QPen(QColor(0x38, 0x3d, 0x48), 1));
    p.drawRect(0, 0, width() - 1, height() - 1);
}

void DetailWaveformView::drawLane(QPainter& p, const QRect& r, int deck)
{
    Deck& d = engine_->deck(deck);
    TrackDataPtr t = d.track();
    const QColor accent = deckAccent(deck);
    lastTrack_[deck] = t.get();

    p.fillRect(r, deck == 0 ? QColor(0x14, 0x16, 0x1a)
                            : QColor(0x18, 0x14, 0x17));

    // Deck letter tag.
    p.setFont(QFont(font().family(), 9, QFont::Bold));
    p.setPen(accent);
    p.drawText(QRect(r.left() + 4, r.top() + 2, 20, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               deck == 0 ? QStringLiteral("A") : QStringLiteral("B"));

    if (!t || t->durationSec <= 0.0 || r.width() <= 0) {
        p.setPen(themeDimText());
        p.drawText(r, Qt::AlignCenter, tr("no track loaded"));
        lastPaintPos_[deck] = -1.0;
        lastLoopStart_[deck] = d.loopStartSec.load();
        lastLoopEnd_[deck] = d.loopEndSec.load();
        lastLoopActive_[deck] = d.loopActive.load();
        return;
    }

    const int w = r.width();
    const double pos = d.positionSec();
    lastPaintPos_[deck] = pos;
    const double secPerPx = windowSec_ / w;
    const double leftSec = pos - (w / 2.0) * secPerPx;
    const int mid = r.center().y();
    const int halfMax = r.height() / 2 - 3;

    // Band-colored bins (fall back to gray overviewPeaks when the track has
    // no band data). All vectors share the overviewPeaks bin raster.
    const auto& low = t->overviewLow;
    const auto& midB = t->overviewMid;
    const auto& high = t->overviewHigh;
    const auto& peaks = t->overviewPeaks;
    const bool banded = !low.empty() && low.size() == midB.size() &&
                        low.size() == high.size();
    const int n = (int)(banded ? low.size() : peaks.size());
    if (n > 0) {
        const double binsPerSec = n / t->durationSec;
        const QColor lowC = waveLowColor(), midC = waveMidColor(),
                     highC = waveHighColor();
        const QColor gray(0x8a, 0x90, 0x9c);
        for (int x = 0; x < w; ++x) {
            const double sec = leftSec + x * secPerPx;
            if (sec < 0.0 || sec >= t->durationSec) continue;
            // Max over the bins this pixel column covers (>=1 bin).
            int i0 = (int)(sec * binsPerSec);
            int i1 = std::max(i0 + 1, (int)((sec + secPerPx) * binsPerSec));
            i0 = std::clamp(i0, 0, n - 1);
            i1 = std::clamp(i1, i0 + 1, n);
            const int px = r.left() + x;
            if (banded) {
                float l = 0, m = 0, h = 0;
                for (int i = i0; i < i1; ++i) {
                    l = std::max(l, low[i]);
                    m = std::max(m, midB[i]);
                    h = std::max(h, high[i]);
                }
                // Low tallest in back, then mid, high in front.
                int lh = (int)(l * halfMax);
                int mh = (int)(m * halfMax);
                int hh = (int)(h * halfMax);
                if (lh > 0) {
                    p.setPen(lowC);
                    p.drawLine(px, mid - lh, px, mid + lh);
                }
                if (mh > 0) {
                    p.setPen(midC);
                    p.drawLine(px, mid - mh, px, mid + mh);
                }
                if (hh > 0) {
                    p.setPen(highC);
                    p.drawLine(px, mid - hh, px, mid + hh);
                }
            } else {
                float pk = 0;
                for (int i = i0; i < i1; ++i) pk = std::max(pk, peaks[i]);
                int ph = std::max(1, (int)(pk * halfMax));
                p.setPen(gray);
                p.drawLine(px, mid - ph, px, mid + ph);
            }
        }
    }

    auto xForSec = [&](double sec) {
        return r.left() + (int)std::lround((sec - leftSec) / secPerPx);
    };

    // Loop region: deck-accent shading between loopStartSec/loopEndSec —
    // ~25% alpha while active, dimmer when bounds are stored but inactive —
    // with brighter edge lines.
    {
        const double ls = d.loopStartSec.load();
        const double le = d.loopEndSec.load();
        const bool active = d.loopActive.load();
        lastLoopStart_[deck] = ls;
        lastLoopEnd_[deck] = le;
        lastLoopActive_[deck] = active;
        const double rightSec = leftSec + windowSec_;
        if (ls >= 0.0 && le > ls && ls < rightSec && le > leftSec) {
            const int x0 = std::max(r.left(), xForSec(ls));
            const int x1 = std::min(r.right(), xForSec(le));
            QColor fill = accent;
            fill.setAlpha(active ? 64 : 28);
            p.fillRect(QRect(x0, r.top(), std::max(1, x1 - x0), r.height()),
                       fill);
            QColor edge = accent.lighter(140);
            edge.setAlpha(active ? 230 : 110);
            p.setPen(QPen(edge, 1));
            if (ls >= leftSec) p.drawLine(x0, r.top(), x0, r.bottom());
            if (le <= rightSec) p.drawLine(x1, r.top(), x1, r.bottom());
        }
    }

    // Beatgrid: one tick per beat, stronger every 4 (downbeat).
    if (t->bpm > 0.0) {
        const double rightSec =
            std::min(leftSec + windowSec_, t->durationSec);
        const double firstVisibleBeat =
            std::ceil(t->beatAtSec(std::max(leftSec, 0.0)));
        for (double b = std::max(0.0, firstVisibleBeat);; b += 1.0) {
            const double sec = t->secAtBeat(b);
            if (sec > rightSec) break;
            const bool strong = std::fmod(b, 4.0) < 0.5;
            p.setPen(QColor(255, 255, 255, strong ? 70 : 28));
            const int x = xForSec(sec);
            p.drawLine(x, r.top(), x, r.bottom());
        }
    }

    // Cue point (Deck::cuePointSec) — small accent-colored notch marker.
    {
        const double cue = d.cuePointSec.load();
        if (cue >= 0.0 && cue <= t->durationSec && cue >= leftSec &&
            cue <= leftSec + windowSec_) {
            const int x = xForSec(cue);
            p.setPen(QPen(accent.lighter(130), 1));
            p.drawLine(x, r.top(), x, r.bottom());
            QPolygon tri;
            tri << QPoint(x - 4, r.top()) << QPoint(x + 4, r.top())
                << QPoint(x, r.top() + 6);
            p.setBrush(accent.lighter(130));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            p.setBrush(Qt::NoBrush);
        }
    }

    // Hot cue flags in slot colors.
    p.setFont(QFont(font().family(), 8, QFont::Bold));
    for (int i = 0; i < 8; ++i) {
        const double sec = t->hotCues[i];
        if (sec < 0.0 || sec < leftSec || sec > leftSec + windowSec_)
            continue;
        const int x = xForSec(sec);
        const QColor c = hotCueColor(i);
        p.setPen(c);
        p.drawLine(x, r.top(), x, r.bottom());
        QRect flag(x + 1, r.top() + 1, 11, 11);
        p.fillRect(flag, c);
        p.setPen(Qt::black);
        p.drawText(flag, Qt::AlignCenter, QString::number(i + 1));
    }

    // Transition entry marker: orange line + "T" tag.
    {
        const double sec = transitionEntrySec_[deck];
        if (sec >= 0.0 && sec >= leftSec && sec <= leftSec + windowSec_) {
            const int x = xForSec(sec);
            const QColor c = transitionEntryColor();
            p.setPen(QPen(c, 2));
            p.drawLine(x, r.top(), x, r.bottom());
            QRect tag(x + 2, r.bottom() - 12, 11, 11);
            p.fillRect(tag, c);
            p.setPen(Qt::black);
            p.drawText(tag, Qt::AlignCenter, QStringLiteral("T"));
        }
    }


    // Labeled cues belonging to the selected transition.  Alternate tag rows
    // to keep nearby moments readable without covering the waveform entirely.
    {
        const QColor c(0xf1, 0xc7, 0x5b);
        p.setFont(QFont(font().family(), 8, QFont::DemiBold));
        for (qsizetype i = 0; i < transitionCueSecs_[deck].size(); ++i) {
            const double sec = transitionCueSecs_[deck].at(i);
            if (sec < leftSec || sec > leftSec + windowSec_) continue;
            const int x = xForSec(sec);
            p.setPen(QPen(c, 1));
            p.drawLine(x, r.top(), x, r.bottom());
            QString label = i < transitionCueLabels_[deck].size()
                                ? transitionCueLabels_[deck].at(i)
                                : tr("Cue");
            QFontMetrics fm(p.font());
            const int labelW = std::min(150, fm.horizontalAdvance(label) + 10);
            const int y = r.top() + 15 + (int)(i % 2) * 16;
            QRect tag(std::clamp(x + 2, r.left(), r.right() - labelW), y,
                      labelW, 14);
            p.fillRect(tag, c);
            p.setPen(Qt::black);
            p.drawText(tag.adjusted(4, 0, -3, 0),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(label, Qt::ElideRight, labelW - 7));
        }
    }
}

} // namespace gvt
