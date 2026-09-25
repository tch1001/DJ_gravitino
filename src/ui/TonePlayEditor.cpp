// Two explicit rulers keep WHERE the sample comes from separate from WHEN
// notes play. Gesture previews are transient; release commits one undo command.
#include "TonePlayEditor.h"
#include "TransitionEditor.h"
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace gvt {
class ToneSampleView final : public QWidget {
  public:
    TrackDataPtr track;
    TonePlayPattern pattern;
    bool editSustain = false;
    double &selectionStart() {
        return editSustain && pattern.sustain ? pattern.sustain->loopStartBeat : pattern.sourceStartBeat;
    }
    double &selectionEnd() {
        return editSustain && pattern.sustain ? pattern.sustain->loopEndBeat : pattern.sourceEndBeat;
    }
    double selectionStart() const {
        return editSustain && pattern.sustain ? pattern.sustain->loopStartBeat : pattern.sourceStartBeat;
    }
    double selectionEnd() const {
        return editSustain && pattern.sustain ? pattern.sustain->loopEndBeat : pattern.sourceEndBeat;
    }
    std::function<void(double, double)> changed;
    std::function<void()> viewChanged;
    double viewStart = 0, viewEnd = 32, selectionAnchor = 0;
    int dragging = 0;
    explicit ToneSampleView(QWidget *parent) : QWidget(parent) {
        setObjectName("toneSampleWaveform");
        setMinimumHeight(110);
        setMaximumHeight(170);
        setMouseTracking(true);
        setToolTip(
            "Drag across the outgoing waveform to select a slice. Drag START or END to trim. "
            "Command+wheel zooms at the pointer; wheel pans. Fit slice zooms to your selection. "
            "No audio file is changed.");
    }
    void setView(double first, double last) {
        if (!track || track->bpm <= 0 || track->durationSec <= 0)
            return;
        const double low = track->canonicalBeatAtSec(0);
        const double high = track->canonicalBeatAtSec(track->durationSec);
        const double span = std::clamp(last - first, std::min(.0625, high - low), high - low);
        viewStart = std::clamp(first, low, high - span);
        viewEnd = viewStart + span;
        update();
        if (viewChanged)
            viewChanged();
    }
    void zoom(double factor, double pixel) {
        const double anchor = beat(pixel);
        const double fraction = std::clamp((pixel - 10) / std::max(1.0, width() - 20.0), 0.0, 1.0);
        const double span = (viewEnd - viewStart) / factor;
        setView(anchor - fraction * span, anchor + (1 - fraction) * span);
    }
    void wholeSong() {
        if (!track || track->bpm <= 0)
            return;
        setView(track->canonicalBeatAtSec(0), track->canonicalBeatAtSec(track->durationSec));
    }
    void zoomToSlice() {
        const double margin =
            std::max(0.03125, (pattern.sourceEndBeat - pattern.sourceStartBeat) * .5);
        setView(pattern.sourceStartBeat - margin, pattern.sourceEndBeat + margin);
    }
    double x(double beat) const {
        return 10 + (beat - viewStart) / std::max(.001, viewEnd - viewStart) * (width() - 20);
    }
    double beat(double x) const {
        return viewStart + (x - 10) / std::max(1, width() - 20) * (viewEnd - viewStart);
    }
    QRectF handleRect(bool start) const {
        const double left = x(selectionStart()), right = x(selectionEnd());
        return {std::clamp(start ? left : right - 38, 10.0, std::max(10.0, width() - 48.0)),
                !start && right - left < 80 ? 44.0 : 24.0, 38, 18};
    }
    bool onHandle(bool start, QPointF at) const {
        const double edge = x(start ? selectionStart() : selectionEnd());
        return edge >= 10 && edge <= width() - 10 && handleRect(start).contains(at);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#14191e"));
        if (!track) {
            p.setPen(Qt::gray);
            p.drawText(rect(), Qt::AlignCenter, "Load the outgoing song to select a snippet");
            return;
        }
        const int center = height() / 2;
        for (int col = 10; col < width() - 10; ++col) {
            const double sec = track->secAtCanonicalBeat(beat(col));
            const double sec2 = track->secAtCanonicalBeat(beat(col + 1));
            if (sec2 < 0 || sec >= track->durationSec)
                continue;
            const int a = std::max(0, int(sec * kSampleRate / 512)),
                      b = std::max(a + 1, int(sec2 * kSampleRate / 512));
            const auto peak = [&](const std::vector<float> &bins) {
                float v = 0;
                for (int i = a; i < b && i < int(bins.size()); ++i)
                    v = std::max(v, bins[i]);
                return v;
            };
            const float lo = peak(track->overviewLow), mid = peak(track->overviewMid),
                        hi = peak(track->overviewHigh);
            const float total = std::max({lo, mid, hi, .01f});
            p.setPen(QColor(int(90 + 165 * lo / total), int(75 + 165 * mid / total),
                            int(90 + 165 * hi / total)));
            float amplitude = peak(track->overviewPeaks);
            if ((sec2 - sec) * kSampleRate < 512 && !track->pcm.empty()) {
                // At close zoom, show actual samples rather than a flat
                // overview bin, so tiny source trims remain meaningful.
                amplitude = 0;
                const size_t first = size_t(std::max(0.0, sec) * kSampleRate);
                const size_t last =
                    std::min(track->pcm.size() / 2,
                             std::max(first + 1, size_t(std::max(0.0, sec2) * kSampleRate)));
                for (size_t i = first; i < last; ++i)
                    amplitude = std::max(
                        {amplitude, std::abs(track->pcm[2 * i]), std::abs(track->pcm[2 * i + 1])});
            }
            const double amp = std::min(1.0f, amplitude) * height() * .36;
            p.drawLine(QPointF(col, center - amp), QPointF(col, center + amp));
        }
        const double targetStep = (viewEnd - viewStart) * 55 / std::max(1, width() - 20);
        const double step = std::exp2(std::ceil(std::log2(std::max(1.0 / 64, targetStep))));
        const double gridOffset = track ? track->canonicalBeatOffset : 0.0;
        for (double b = std::ceil((viewStart - gridOffset) / step) * step + gridOffset;
             b <= viewEnd; b += step) {
            p.setPen(QColor(255, 255, 255, 45));
            p.drawLine(QPointF(x(b), 23), QPointF(x(b), height()));
            p.setPen(QColor("#aab4c0"));
            p.drawText(QPointF(x(b) + 3, 16), QString::number(b, 'f',
                step < 1 ? 3 : std::abs(b - std::round(b)) < 1e-7 ? 0 : 2));
        }
        const auto left = x(pattern.sourceStartBeat), right = x(pattern.sourceEndBeat);
        p.fillRect(QRectF(left, 24, right - left, height() - 24), QColor(242, 183, 83, 45));
        p.setPen(QPen(QColor("#f2b753"), 2));
        for (double edge : {left, right})
            p.drawLine(QPointF(edge, 24), QPointF(edge, height()));
        if (pattern.sustain && pattern.sustain->enabled) {
            const double a = x(pattern.sustain->loopStartBeat), b = x(pattern.sustain->loopEndBeat);
            p.fillRect(QRectF(a, 44, b - a, height() - 44), QColor(190, 140, 255, 65));
            p.setPen(QPen(QColor("#bf8cff"), 2));
            p.drawLine(QPointF(a, 44), QPointF(a, height()));
            p.drawLine(QPointF(b, 44), QPointF(b, height()));
        }
        for (bool start : {true, false}) {
            const double edge = x(start ? selectionStart() : selectionEnd());
            if (edge < 10 || edge > width() - 10)
                continue;
            const auto cap = handleRect(start);
            p.fillRect(cap, QColor(editSustain ? "#bf8cff" : "#f2b753"));
            p.setPen(QColor("#14191e"));
            p.drawText(cap, Qt::AlignCenter, editSustain ? (start ? "HOLD IN" : "OUT") : (start ? "START" : "END"));
        }
        p.setPen(QColor("#f2b753"));
        p.drawText(12, height() - 8, editSustain ? "DRAG THE STEADY VOWEL — purple sustain region" : "OUTGOING SONG BEATS — sample boundaries");
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton || !track)
            return;
        const double at = e->position().x();
        dragging = onHandle(true, e->position())                   ? 1
                   : onHandle(false, e->position())                ? 2
                   : std::abs(at - x(selectionStart())) < 9 ? 1
                   : std::abs(at - x(selectionEnd())) < 9   ? 2
                                                                   : 3;
        if (dragging == 3) {
            const double low = editSustain ? pattern.sourceStartBeat : track->canonicalBeatAtSec(0);
            const double high = editSustain ? pattern.sourceEndBeat : track->canonicalBeatAtSec(track->durationSec);
            const double minimum = std::min(.005, (high - low) / 2);
            selectionStart() = std::clamp(beat(at), low, high - minimum);
            selectionEnd() = std::min(selectionStart() + .25, high);
            selectionAnchor = selectionStart();
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!track)
            return;
        if (!dragging) {
            setCursor(onHandle(true, e->position()) || onHandle(false, e->position()) ||
                              std::min(std::abs(e->position().x() - x(selectionStart())),
                                       std::abs(e->position().x() - x(selectionEnd()))) < 9
                          ? Qt::SizeHorCursor
                          : Qt::CrossCursor);
            return;
        }
        const double low = editSustain ? pattern.sourceStartBeat : track->canonicalBeatAtSec(0);
        const double high = editSustain ? pattern.sourceEndBeat : track->canonicalBeatAtSec(track->durationSec);
        const double minimum = std::min(.005, (high - low) / 2);
        const double at = std::clamp(beat(e->position().x()), low, high);
        const double maxLength = std::min(32.0, 8.0 * track->bpm / 60.0);
        if (dragging == 3) {
            const double edge =
                std::clamp(at, selectionAnchor - maxLength, selectionAnchor + maxLength);
            selectionStart() = std::min(std::min(selectionAnchor, edge), high - minimum);
            selectionEnd() = std::max(std::max(selectionAnchor, edge), selectionStart() + minimum);
        } else if (dragging == 1)
            selectionStart() = std::clamp(at, std::max(low, selectionEnd() - maxLength), selectionEnd() - minimum);
        else
            selectionEnd() = std::clamp(at, selectionStart() + minimum,
                                       std::min(high, selectionStart() + maxLength));
        update();
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (dragging && changed)
            changed(selectionStart(), selectionEnd());
        dragging = 0;
    }
    void wheelEvent(QWheelEvent *e) override {
        const QPoint delta = e->pixelDelta().isNull() ? e->angleDelta() : e->pixelDelta();
        const double movement = delta.y() ? delta.y() : delta.x();
        if (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))
            zoom(std::exp(movement * .002), e->position().x());
        else {
            const double pixels = delta.x() ? delta.x() : delta.y();
            const double offset = -pixels * (viewEnd - viewStart) / std::max(1, width() - 20);
            setView(viewStart + offset, viewEnd + offset);
        }
        e->accept();
    }
};

class TonePianoRoll final : public QAbstractScrollArea {
  public:
    TonePlayPattern pattern;
    std::function<bool(TonePlayPattern, const QString &)> edited;
    std::function<void(int)> audition;
    std::function<void()> selectionChanged;
    std::function<void(double)> cursor;
    int selected = -1, dragging = -1;
    std::set<int> selection;
    bool selectingBars = false, selectingBox = false;
    double rangeStart = -1, rangeEnd = -1;
    QPointF marqueeStart, marqueeEnd;
    std::function<void(const QString &)> feedback;
    static constexpr const char *clipboardType = "application/x-gravitino-tone-notes";
    bool resizeNote = false;
    bool centered = false;
    TonePlayNote original;
    std::vector<TonePlayNote> originalNotes;
    QPointF press;
    double pixelsPerBeat = 48, snap = .25, playhead = 0, endBeat = 32, barBeats = 4;
    static constexpr int keys = 66, ruler = 26, row = 19, topPitch = 96, lowPitch = 24;
    explicit TonePianoRoll(QWidget *parent) : QAbstractScrollArea(parent) {
        setObjectName("tonePianoRoll");
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(220);
        setToolTip("Drag the beat ruler to select bars; drag empty space to select notes. "
                   "Shift-click adds/removes notes. Command+C copies; click a destination then "
                   "Command+V pastes. "
                   "Double-click draws a note at the Snap length. Drag its right edge to resize. "
                   "Wheel scrolls "
                   "vertically; Shift+wheel scrolls horizontally; Command+wheel zooms.");
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { viewport()->update(); });
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { viewport()->update(); });
        verticalScrollBar()->setValue((topPitch - 60) * row - 100);
    }
    double beat(double x) const {
        return std::max(0.0, (x - keys + horizontalScrollBar()->value()) / pixelsPerBeat);
    }
    double snapped(double b) const { return snap > 0 ? std::round(b / snap) * snap : b; }
    int pitch(double y) const {
        return std::clamp(topPitch - int((y - ruler + verticalScrollBar()->value()) / row),
                          lowPitch, topPitch);
    }
    QRectF noteRect(const TonePlayNote &n) const {
        return {keys + n.beat * pixelsPerBeat - horizontalScrollBar()->value(),
                double(ruler + (topPitch - n.pitch) * row - verticalScrollBar()->value() + 2),
                std::max(4.0, n.duration * pixelsPerBeat), row - 4.0};
    }
    int hit(QPointF at) const {
        for (int i = int(pattern.notes.size()) - 1; i >= 0; --i)
            if (noteRect(pattern.notes[i]).contains(at))
                return i;
        return -1;
    }
    void ranges() {
        horizontalScrollBar()->setRange(
            0,
            std::max(0, int(std::max(endBeat + 4, tonePlayEndBeat(pattern) + 4) * pixelsPerBeat) -
                            viewport()->width() + keys));
        verticalScrollBar()->setRange(
            0, std::max(0, (topPitch - lowPitch + 1) * row - viewport()->height() + ruler));
        verticalScrollBar()->setPageStep(viewport()->height() - ruler);
        horizontalScrollBar()->setPageStep(viewport()->width() - keys);
    }
    void centerRoot() {
        ranges();
        verticalScrollBar()->setValue((topPitch - pattern.rootNote) * row -
                                      viewport()->height() / 2);
    }
    void resizeEvent(QResizeEvent *e) override {
        QAbstractScrollArea::resizeEvent(e);
        ranges();
    }
    void showEvent(QShowEvent *e) override {
        QAbstractScrollArea::showEvent(e);
        if (!centered) {
            centerRoot();
            centered = true;
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(viewport());
        p.fillRect(viewport()->rect(), QColor("#20262b"));
        for (int note = lowPitch; note <= topPitch; ++note) {
            const int y = ruler + (topPitch - note) * row - verticalScrollBar()->value();
            if (y + row < ruler || y > viewport()->height())
                continue;
            const int semitone = note % 12;
            const bool black =
                semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10;
            p.fillRect(keys, y, viewport()->width() - keys, row,
                       QColor(black ? "#20262b" : "#293036"));
            p.fillRect(0, y, keys - 1, row - 1, QColor(black ? "#202329" : "#b8c1c7"));
            p.setPen(black ? QColor("#bbc7d1") : QColor("#293138"));
            p.drawText(QRect(3, y, keys - 8, row), Qt::AlignRight | Qt::AlignVCenter,
                       tonePlayPitchName(note));
            if (note == pattern.rootNote) {
                p.setPen(QColor("#f2b753"));
                p.drawLine(keys, y + row - 1, viewport()->width(), y + row - 1);
            }
        }
        p.save();
        p.setClipRect(QRect(keys, ruler, viewport()->width() - keys, viewport()->height() - ruler));
        const double first = beat(keys), last = beat(viewport()->width());
        const double grid = std::max(snap > 0 ? snap : 1.0 / 64,
                                     std::exp2(std::ceil(std::log2(8 / pixelsPerBeat))));
        for (double b = std::floor(first / grid) * grid; b <= last; b += grid) {
            const int x = keys + b * pixelsPerBeat - horizontalScrollBar()->value();
            p.setPen(QColor(255, 255, 255, std::fmod(std::abs(b), barBeats) < .001 ? 70 : 22));
            p.drawLine(x, ruler, x, viewport()->height());
        }
        if (rangeStart >= 0) {
            const double left = keys + rangeStart * pixelsPerBeat - horizontalScrollBar()->value();
            p.fillRect(QRectF(left, ruler, (rangeEnd - rangeStart) * pixelsPerBeat,
                              viewport()->height() - ruler),
                       QColor(105, 216, 255, 30));
        }
        if (pattern.sustain && pattern.sustain->enabled) {
            const auto &h = *pattern.sustain;
            const auto r = noteRect({h.beat, h.duration, h.pitch, h.gain});
            p.setPen(QPen(QColor("#bf8cff"), 1, Qt::DashLine));
            p.setBrush(QColor(140, 90, 210, 65));
            p.drawRoundedRect(r, 2, 2);
            p.drawText(r.adjusted(4, 0, -3, 0), Qt::AlignRight | Qt::AlignVCenter, "BACKGROUND HOLD");
        }
        for (int i = 0; i < int(pattern.notes.size()); ++i) {
            const auto r = noteRect(pattern.notes[i]);
            const bool chosen = selection.contains(i);
            p.setPen(QPen(chosen ? QColor("#ffda94") : QColor("#83cbbd"), chosen ? 2 : 1));
            p.setBrush(chosen ? QColor("#b78238") : QColor("#3c887a"));
            p.drawRoundedRect(r, 2, 2);
            if (r.width() > 36) {
                p.setPen(Qt::white);
                p.drawText(r.adjusted(4, 0, -3, 0), Qt::AlignVCenter,
                           tonePlayPitchName(pattern.notes[i].pitch));
            }
            p.setPen(QColor("#d5e5df"));
            p.drawLine(r.right() - 3, r.top() + 3, r.right() - 3, r.bottom() - 3);
        }
        p.setPen(QPen(QColor("#69d8ff"), 1.5));
        const double x = keys + playhead * pixelsPerBeat - horizontalScrollBar()->value();
        p.drawLine(QPointF(x, ruler), QPointF(x, viewport()->height()));
        if (selectingBox) {
            p.setPen(QPen(QColor("#69d8ff"), 1, Qt::DashLine));
            p.setBrush(QColor(105, 216, 255, 30));
            p.drawRect(QRectF(marqueeStart, marqueeEnd).normalized());
        }
        p.restore();
        p.fillRect(0, 0, viewport()->width(), ruler, QColor("#151c22"));
        p.setPen(QColor("#b1c0ce"));
        p.drawText(5, 18, "NOTE");
        if (rangeStart >= 0) {
            p.fillRect(QRectF(keys + rangeStart * pixelsPerBeat - horizontalScrollBar()->value(), 0,
                              (rangeEnd - rangeStart) * pixelsPerBeat, ruler),
                       QColor(105, 216, 255, 55));
        }
        for (double b = std::ceil(first); b <= last; b += pixelsPerBeat < 28 ? 4 : 1) {
            const double bx = keys + b * pixelsPerBeat - horizontalScrollBar()->value();
            p.drawText(QPointF(bx + 3, 18), QString::number(b, 'f', 0));
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        setFocus();
        const auto at = e->position();
        if (at.y() < ruler && at.x() >= keys && e->button() == Qt::LeftButton) {
            selectingBars = true;
            press = at;
            rangeStart = rangeEnd = -1;
            selection.clear();
            selected = -1;
            if (cursor)
                cursor(snapped(beat(at.x())));
            if (selectionChanged)
                selectionChanged();
            viewport()->update();
            return;
        }
        if (at.x() < keys) {
            if (e->button() == Qt::LeftButton && audition)
                audition(pitch(at.y()));
            return;
        }
        const int target = hit(at);
        rangeStart = rangeEnd = -1;
        if (e->modifiers() & Qt::ShiftModifier && target >= 0) {
            if (!selection.erase(target))
                selection.insert(target);
            selected = selection.empty() ? -1 : *selection.rbegin();
            if (selectionChanged)
                selectionChanged();
            viewport()->update();
            return;
        }
        if (!selection.contains(target))
            selection.clear();
        selected = target;
        if (selected >= 0)
            selection.insert(selected);
        if (selectionChanged)
            selectionChanged();
        if (e->button() == Qt::RightButton) {
            removeSelected();
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        if (selected >= 0) {
            dragging = selected;
            original = pattern.notes[selected];
            originalNotes = pattern.notes;
            press = at;
            resizeNote = at.x() > noteRect(original).right() -
                                      std::min(9.0, noteRect(original).width() * .35);
        } else {
            selectingBox = true;
            marqueeStart = marqueeEnd = at;
            if (cursor)
                cursor(snapped(beat(at.x())));
        }
        viewport()->update();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton || e->position().x() < keys ||
            e->position().y() < ruler || hit(e->position()) >= 0)
            return;
        const int n =
            std::clamp(pitch(e->position().y()), std::max(lowPitch, pattern.rootNote - 24),
                       std::min(topPitch, pattern.rootNote + 24));
        selectingBox = selectingBars = false;
        rangeStart = rangeEnd = -1;
        pattern.notes.push_back({snapped(beat(e->position().x())), snap > 0 ? snap : .25, n, .8});
        selected = int(pattern.notes.size()) - 1;
        selection = {selected};
        if (edited)
            edited(pattern, "Draw tone note");
        if (selectionChanged)
            selectionChanged();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (selectingBars) {
            if (std::abs(e->position().x() - press.x()) < 3)
                return;
            const double a = beat(press.x()), b = beat(e->position().x());
            rangeStart = std::floor(std::min(a, b) / barBeats) * barBeats;
            rangeEnd =
                std::max(rangeStart + barBeats, std::ceil(std::max(a, b) / barBeats) * barBeats);
            selection.clear();
            for (int i = 0; i < int(pattern.notes.size()); ++i)
                if (pattern.notes[i].beat >= rangeStart && pattern.notes[i].beat < rangeEnd)
                    selection.insert(i);
            selected = selection.empty() ? -1 : *selection.begin();
            viewport()->update();
            if (selectionChanged)
                selectionChanged();
            return;
        }
        if (selectingBox) {
            marqueeEnd = e->position();
            selection.clear();
            const auto box = QRectF(marqueeStart, marqueeEnd).normalized();
            for (int i = 0; i < int(pattern.notes.size()); ++i)
                if (box.intersects(noteRect(pattern.notes[i])))
                    selection.insert(i);
            selected = selection.empty() ? -1 : *selection.begin();
            viewport()->update();
            if (selectionChanged)
                selectionChanged();
            return;
        }
        if (dragging < 0)
            return;
        const double dx = (e->position().x() - press.x()) / pixelsPerBeat;
        double movement = snapped(original.beat + dx) - original.beat;
        double minimumBeat = original.beat;
        int lower = -96, upper = 96;
        for (const int i : selection) {
            minimumBeat = std::min(minimumBeat, originalNotes[i].beat);
            lower =
                std::max(lower, std::max(lowPitch, pattern.rootNote - 24) - originalNotes[i].pitch);
            upper =
                std::min(upper, std::min(topPitch, pattern.rootNote + 24) - originalNotes[i].pitch);
        }
        movement = std::max(movement, -minimumBeat);
        const int shift =
            std::clamp(int(std::round((press.y() - e->position().y()) / row)), lower, upper);
        for (const int i : selection) {
            auto &n = pattern.notes[i];
            if (resizeNote)
                n.duration = std::clamp(snapped(originalNotes[i].duration + dx), 1.0 / 64, 64.0);
            else {
                n.beat = originalNotes[i].beat + movement;
                n.pitch = originalNotes[i].pitch + shift;
            }
        }
        viewport()->update();
        if (selectionChanged)
            selectionChanged();
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        selectingBox = selectingBars = false;
        if (dragging >= 0) {
            dragging = -1;
            if (edited)
                edited(pattern, "Move/resize tone note");
        }
        viewport()->update();
    }
    void removeSelected() {
        if (selection.empty())
            return;
        for (auto i = selection.rbegin(); i != selection.rend(); ++i)
            pattern.notes.erase(pattern.notes.begin() + *i);
        selected = std::min(selected, int(pattern.notes.size()) - 1);
        selection.clear();
        if (selected >= 0)
            selection.insert(selected);
        rangeStart = rangeEnd = -1;
        if (edited)
            edited(pattern, "Delete selected tone notes");
        if (selectionChanged)
            selectionChanged();
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->matches(QKeySequence::Copy)) {
            copySelection();
            e->accept();
        } else if (e->matches(QKeySequence::Paste)) {
            pasteSelection();
            e->accept();
        } else if (e->matches(QKeySequence::SelectAll)) {
            selection.clear();
            for (int i = 0; i < int(pattern.notes.size()); ++i)
                selection.insert(i);
            selected = selection.empty() ? -1 : *selection.begin();
            rangeStart = rangeEnd = -1;
            viewport()->update();
            if (selectionChanged)
                selectionChanged();
            e->accept();
        } else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            removeSelected();
            e->accept();
        } else
            QAbstractScrollArea::keyPressEvent(e);
    }
    void wheelEvent(QWheelEvent *e) override {
        const auto delta = e->pixelDelta().isNull() ? e->angleDelta() : e->pixelDelta();
        if (e->modifiers() & (Qt::MetaModifier | Qt::ControlModifier)) {
            const double anchor = beat(e->position().x());
            pixelsPerBeat =
                std::clamp(pixelsPerBeat * std::exp((delta.y() ? delta.y() : delta.x()) * .0015),
                           12.0, 1024.0);
            ranges();
            horizontalScrollBar()->setValue(int(keys + anchor * pixelsPerBeat - e->position().x()));
        } else if (e->modifiers() & Qt::ShiftModifier)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() -
                                            (delta.y() ? delta.y() : delta.x()));
        else {
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        }
        viewport()->update();
        e->accept();
    }
    QJsonObject copiedNotes() const {
        if (selection.empty())
            return {};
        auto copy = pattern;
        copy.notes.clear();
        double first = rangeStart >= 0 ? rangeStart : 16384;
        double last = rangeStart >= 0 ? rangeEnd : 0;
        if (rangeStart < 0)
            for (const int i : selection) {
                first = std::min(first, pattern.notes[i].beat);
                last = std::max(last, pattern.notes[i].beat + pattern.notes[i].duration);
            }
        for (const int i : selection) {
            auto note = pattern.notes[i];
            note.beat -= first;
            copy.notes.push_back(note);
        }
        return {{"version", 1}, {"length", last - first}, {"pattern", serializeTonePlay(copy)}};
    }
    void copySelection() {
        const auto data = copiedNotes();
        if (data.isEmpty()) {
            if (feedback)
                feedback("Select notes or drag across the beat ruler to select bars first.");
            return;
        }
        auto *mime = new QMimeData;
        mime->setData(clipboardType, QJsonDocument(data).toJson(QJsonDocument::Compact));
        QApplication::clipboard()->setMimeData(mime);
        if (feedback)
            feedback(
                QString("Copied %1 notes across %2 beats. Click a destination, then Paste (⌘V).")
                    .arg(selection.size())
                    .arg(data.value("length").toDouble()));
    }
    void pasteData(const QJsonObject &data, double at) {
        const double length = data.value("length").toDouble();
        std::optional<TonePlayPattern> copied;
        QString error;
        if (data.value("version").toInt() != 1 || !std::isfinite(length) || length <= 0 ||
            length > 16448 || !parseTonePlay(data.value("pattern"), copied, &error) || !copied ||
            copied->notes.empty()) {
            if (feedback)
                feedback("Clipboard does not contain a valid tone-note selection.");
            return;
        }
        auto next = pattern;
        const int first = int(next.notes.size());
        for (auto note : copied->notes) {
            note.beat += at;
            next.notes.push_back(note);
        }
        if (!validateTonePlay(next, &error)) {
            if (feedback)
                feedback(error);
            return;
        }
        selection.clear();
        for (int i = first; i < int(next.notes.size()); ++i)
            selection.insert(i);
        selected = first;
        if (edited && edited(next, "Paste tone notes")) {
            rangeStart = at;
            rangeEnd = at + length;
            if (cursor)
                cursor(rangeEnd);
            if (feedback)
                feedback(QString("Pasted %1 notes. Paste again to repeat the next %2 beats.")
                             .arg(copied->notes.size())
                             .arg(length));
        }
        viewport()->update();
        if (selectionChanged)
            selectionChanged();
    }
    void pasteSelection() {
        if (!isEnabled())
            return;
        const auto *mime = QApplication::clipboard()->mimeData();
        const auto bytes = mime ? mime->data(clipboardType) : QByteArray{};
        if (bytes.size() > 1024 * 1024) {
            if (feedback)
                feedback("The copied selection is too large.");
            return;
        }
        pasteData(QJsonDocument::fromJson(bytes).object(), snapped(playhead));
    }
    void duplicateSelection() {
        if (!isEnabled())
            return;
        const auto data = copiedNotes();
        if (data.isEmpty())
            return;
        double first = rangeStart >= 0 ? rangeStart : 16384;
        if (rangeStart < 0)
            for (int i : selection)
                first = std::min(first, pattern.notes[i].beat);
        pasteData(data, first + data.value("length").toDouble());
    }
};

TonePlayEditor::TonePlayEditor(TransitionEditorDocument *doc, QWidget *parent)
    : QWidget(parent), document_(doc) {
    setObjectName("tonePlayEditor");
    // Compact windows must scroll, not compress numeric fields over the roll.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("toneWorkspaceScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    layout->setContentsMargins(4, 4, 4, 4);
    auto *header = new QHBoxLayout;
    enabled_ = new QCheckBox(tr("Enable tone play"));
    enabled_->setObjectName("tonePlayEnabled");
    header->addWidget(enabled_);
    auto *preview = new QPushButton(tr("▶ Preview mix"));
    preview->setObjectName("tonePreviewMix");
    header->addWidget(preview);
    auto *samplePreview = new QPushButton(tr("▶ Preview slice"));
    samplePreview->setObjectName("tonePreviewSlice");
    samplePreview->setToolTip(tr("Hear just this slice at its original pitch. Preview mix plays "
                                 "your arranged notes with the transition."));
    header->addWidget(samplePreview);
    header->addStretch();
    sliceInfo_ = new QLabel;
    sliceInfo_->setObjectName("toneSliceInfo");
    header->addWidget(sliceInfo_);
    layout->addLayout(header);
    help_ = new QLabel(tr(
        "1  Select an outgoing snippet    2  Draw notes below    3  Preview the mix, then Save"));
    help_->setWordWrap(true);
    layout->addWidget(help_);
    auto *sampleTools = new QHBoxLayout;
    const auto spin = [&](const char *name, double lo, double hi, int decimals) {
        auto *s = new QDoubleSpinBox(this);
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setKeyboardTracking(false);
        s->setSingleStep(.01);
        return s;
    };
    start_ = spin("toneSourceStart", -1000000, 1000000, 6);
    end_ = spin("toneSourceEnd", -1000000, 1000000, 6);
    for (auto pair :
         {std::pair<const char *, QDoubleSpinBox *>{"Start beat", start_}, {"End beat", end_}}) {
        sampleTools->addWidget(new QLabel(tr(pair.first)));
        sampleTools->addWidget(pair.second);
    }
    auto *zoom = new QPushButton(tr("Fit slice"));
    zoom->setObjectName("toneFitSlice");
    auto *whole = new QPushButton(tr("Whole song"));
    whole->setObjectName("toneWholeSong");
    auto *zoomOut = new QPushButton(tr("−"));
    auto *zoomIn = new QPushButton(tr("+"));
    zoomOut->setObjectName("toneSliceZoomOut");
    zoomIn->setObjectName("toneSliceZoomIn");
    zoomOut->setToolTip(tr("Zoom out of the source waveform"));
    zoomIn->setToolTip(
        tr("Zoom into the source waveform; use Command+wheel to zoom at your pointer"));
    for (auto *b : {zoomOut, zoomIn})
        b->setFixedWidth(28);
    sampleTools->addWidget(zoomOut);
    sampleTools->addWidget(zoomIn);
    sampleTools->addWidget(zoom);
    sampleTools->addWidget(whole);
    layout->addLayout(sampleTools);
    sample_ = new ToneSampleView(this);
    layout->addWidget(sample_);
    sampleScroll_ = new QScrollBar(Qt::Horizontal, this);
    sampleScroll_->setObjectName("toneSliceScroll");
    sampleScroll_->setToolTip(tr("Pan through the outgoing song without changing the slice"));
    layout->addWidget(sampleScroll_);
    sample_->viewChanged = [this] {
        if (!track_)
            return;
        const double low = track_->canonicalBeatAtSec(0);
        const double full = track_->canonicalBeatAtSec(track_->durationSec) - low;
        const double span = sample_->viewEnd - sample_->viewStart;
        QSignalBlocker block(sampleScroll_);
        sampleScroll_->setRange(0, std::max(0, qRound((full - span) * 1000)));
        sampleScroll_->setPageStep(std::max(1, qRound(span * 1000)));
        sampleScroll_->setSingleStep(std::max(1, qRound(span * 100)));
        sampleScroll_->setValue(qRound((sample_->viewStart - low) * 1000));
        sample_->setProperty("viewStartBeat", sample_->viewStart);
        sample_->setProperty("viewEndBeat", sample_->viewEnd);
    };
    connect(sampleScroll_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!track_)
            return;
        const double first = track_->canonicalBeatAtSec(0) + value / 1000.0;
        sample_->setView(first, first + sample_->viewEnd - sample_->viewStart);
    });
    auto *instrument = new QHBoxLayout;
    root_ = new QComboBox;
    root_->setObjectName("toneRootNote");
    notePitch_ = new QComboBox;
    notePitch_->setObjectName("toneNotePitch");
    for (int n = 24; n <= 96; ++n) {
        root_->addItem(tonePlayPitchName(n), n);
        notePitch_->addItem(tonePlayPitchName(n), n);
    }
    gain_ = spin("toneGain", 0, 1, 2);
    replace_ = new QCheckBox(tr("Replace outgoing audio during phrase"));
    replace_->setObjectName("toneReplaceOutgoing");
    replace_->setToolTip(
        tr("Off by default: sampled notes layer over the original mix. On: only outgoing audio is "
           "ducked between the first and last note; incoming audio stays unchanged."));
    instrument->addWidget(new QLabel(tr("Sample root")));
    instrument->addWidget(root_);
    instrument->addWidget(new QLabel(tr("Level")));
    instrument->addWidget(gain_);
    instrument->addWidget(replace_);
    instrument->addStretch();
    layout->addLayout(instrument);
    auto *effectsGroup = new QGroupBox(tr("SHORT-NOTE TAILS · background hold stays dry"));
    auto *effectsLayout = new QGridLayout(effectsGroup);
    effectsEnabled_ = new QCheckBox(tr("Reverb / echo"));
    effectsEnabled_->setObjectName("toneEffectsEnabled");
    effectsEnabled_->setToolTip(tr("Adds quiet effect tails after each short hit without softening its direct attack. Preview slice/piano keys include these effects; Preview hold does not."));
    effectsLayout->addWidget(effectsEnabled_, 0, 0, 1, 2);
    effectsLayout->addWidget(new QLabel(tr("Independent sends; the dry tone remains unchanged")), 0, 2, 1, 6);
    echo_ = spin("toneEcho", 0, 1, 3);
    reverb_ = spin("toneReverb", 0, 1, 3);
    echoBeats_ = spin("toneEchoBeats", .25, 4, 3);
    tailBeats_ = spin("toneTailBeats", .25, 16, 3);
    holdDucking_ = spin("toneHoldDucking", 0, 1, 3);
    effectsLayout->addWidget(new QLabel(tr("Hold duck")), 2, 0);
    effectsLayout->addWidget(holdDucking_, 2, 1);
    effectsLayout->addWidget(new QLabel(tr("Lower the held voice briefly under each hit")), 2, 2, 1, 6);
    holdDucking_->setToolTip(tr("0 keeps the held voice unchanged; 1 fully ducks it under the short hits, with smooth attack/recovery. This reduces clashing pitches without retuning either layer."));
    int fxColumn = 0;
    for (auto pair : {std::pair<const char *, QDoubleSpinBox *>{"Echo", echo_},
                     {"Reverb", reverb_}, {"Echo beats", echoBeats_}, {"Tail beats", tailBeats_}}) {
        effectsLayout->addWidget(new QLabel(tr(pair.first)), 1, fxColumn++);
        effectsLayout->addWidget(pair.second, 1, fxColumn++);
    }
    tailBeats_->setToolTip(tr("Room for the final echoes after the last note. The return fades out at this boundary, and the transition end extends when needed."));
    layout->addWidget(effectsGroup);
    auto *holdGroup = new QGroupBox(tr("BACKGROUND HOLD · independent of the short notes"));
    holdGroup->setObjectName("toneSustainGroup");
    auto *holdLayout = new QGridLayout(holdGroup);
    sustainEnabled_ = new QCheckBox(tr("Sustain vowel underneath"));
    sustainEnabled_->setObjectName("toneSustainEnabled");
    editSustain_ = new QCheckBox(tr("Select vowel in waveform"));
    editSustain_->setObjectName("toneEditSustainRegion");
    editSustain_->setToolTip(tr("Drag the purple region inside your amber slice. Choose only the steady vowel, not the consonant. The short-note slice stays unchanged."));
    sustainPreview_ = new QPushButton(tr("▶ Preview hold"));
    sustainPreview_->setObjectName("tonePreviewSustain");
    sustainPreview_->setToolTip(tr("Hear only the held vowel for up to eight beats. Preview mix hears both layers together."));
    holdLayout->addWidget(sustainEnabled_, 0, 0, 1, 3);
    holdLayout->addWidget(editSustain_, 0, 3, 1, 3);
    holdLayout->addWidget(sustainPreview_, 0, 6, 1, 2);
    holdBeat_ = spin("toneHoldBeat", 0, 16384, 6);
    holdDuration_ = spin("toneHoldDuration", 1.0 / 64, 256, 6);
    holdGain_ = spin("toneHoldGain", 0, 1, 3);
    holdPitch_ = new QComboBox;
    holdPitch_->setObjectName("toneHoldPitch");
    for (int n = 24; n <= 96; ++n) holdPitch_->addItem(tonePlayPitchName(n), n);
    loopStart_ = spin("toneHoldLoopStart", -1000000, 1000000, 6);
    loopEnd_ = spin("toneHoldLoopEnd", -1000000, 1000000, 6);
    crossfade_ = spin("toneHoldCrossfade", 1, 100, 2);
    attack_ = spin("toneHoldAttack", 1, 2000, 2);
    release_ = spin("toneHoldRelease", 1, 2000, 2);
    const auto field = [&](int row, int col, const QString &name, QWidget *control) {
        holdLayout->addWidget(new QLabel(name), row, col);
        holdLayout->addWidget(control, row, col + 1);
    };
    field(1, 0, tr("At beat"), holdBeat_);
    field(1, 2, tr("Length"), holdDuration_);
    field(1, 4, tr("Pitch"), holdPitch_);
    field(1, 6, tr("Level"), holdGain_);
    field(2, 0, tr("Vowel IN"), loopStart_);
    field(2, 2, tr("OUT"), loopEnd_);
    field(2, 4, tr("Join ms"), crossfade_);
    field(3, 0, tr("Fade in ms"), attack_);
    field(3, 2, tr("Fade out ms"), release_);
    auto *holdHint = new QLabel(tr("Purple: held voice · Green: short hits"));
    holdLayout->addWidget(holdHint, 3, 4, 1, 4);
    holdBeat_->setToolTip(tr("Start on the transition timeline, in beats."));
    holdDuration_->setToolTip(tr("Total hold length in transition beats, including its fades."));
    holdGain_->setToolTip(tr("Background level relative to the instrument Level. Does not change the short-note velocities."));
    for (auto *s : {loopStart_, loopEnd_}) {
        s->setSingleStep(.001);
        s->setToolTip(tr("Canonical outgoing-song beat inside the source slice. This region repeats without retriggering the short notes."));
    }
    crossfade_->setToolTip(tr("Smooth the loop join. Clamped to half the vowel region when needed. Not the mixer crossfader."));
    layout->addWidget(holdGroup);
    auto *noteTools = new QHBoxLayout;
    noteTools->addWidget(new QLabel(tr("NOTES · transition beats")));
    snap_ = new QComboBox;
    snap_->setObjectName("toneNoteSnap");
    for (int denominator : {1, 2, 4, 8, 16, 32, 64})
        snap_->addItem(denominator == 1 ? tr("1 beat") : tr("1/%1 beat").arg(denominator),
                       1.0 / denominator);
    snap_->addItem(tr("Off"), 0.0);
    snap_->setCurrentIndex(2);
    noteTools->addWidget(new QLabel(tr("Snap")));
    noteTools->addWidget(snap_);
    auto *center = new QPushButton(tr("Root"));
    center->setToolTip(tr("Center the piano roll on the sample's root note"));
    auto *duplicate = new QPushButton(tr("Duplicate"));
    duplicate->setObjectName("toneDuplicateNotes");
    auto *remove = new QPushButton(tr("Delete"));
    auto *copy = new QPushButton(tr("Copy"));
    auto *paste = new QPushButton(tr("Paste"));
    copy->setObjectName("toneCopyNotes");
    paste->setObjectName("tonePasteNotes");
    copy->setToolTip(tr("Copy selected notes or bars (Command+C)"));
    paste->setToolTip(
        tr("Paste at the cyan cursor (Command+V); click the beat ruler to choose a destination"));
    for (auto *b : {center, copy, paste, duplicate, remove})
        b->setFocusPolicy(Qt::NoFocus);
    noteTools->addWidget(center);
    noteTools->addWidget(copy);
    noteTools->addWidget(paste);
    noteTools->addWidget(duplicate);
    noteTools->addWidget(remove);
    layout->addLayout(noteTools);
    roll_ = new TonePianoRoll(this);
    layout->addWidget(roll_, 1);
    auto *noteForm = new QHBoxLayout;
    noteBeat_ = spin("toneNoteBeat", 0, 16384, 6);
    noteDuration_ = spin("toneNoteDuration", 1.0 / 64, 64, 6);
    noteDuration_->setSingleStep(.25);
    noteDuration_->setToolTip(tr("Note gate in transition beats. Minimum 1/64 beat (0.015625). "
                                 "Finer Snap settings also draw shorter notes."));
    noteVelocity_ = spin("toneNoteVelocity", 0, 1, 2);
    noteForm->addWidget(new QLabel(tr("Selected note")));
    noteForm->addWidget(notePitch_);
    for (auto pair : {std::pair<const char *, QDoubleSpinBox *>{"At", noteBeat_},
                      {"Length", noteDuration_},
                      {"Velocity", noteVelocity_}}) {
        noteForm->addWidget(new QLabel(tr(pair.first)));
        noteForm->addWidget(pair.second);
    }
    layout->addLayout(noteForm);
    auto *hint = new QLabel(tr(
        "Drag across the beat ruler to select bars, or drag an empty area to box-select notes. "
        "Shift-click adds/removes notes; ⌘C copies; click a destination then ⌘V pastes.\n"
        "Double-click draws at the Snap length; drag the right edge to shorten. Wheel: vertical; "
        "Shift: horizontal; ⌘: zoom. Higher pitches play the slice faster. Only Save writes a "
        "recipe."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    connect(preview, &QPushButton::clicked, this, &TonePlayEditor::previewRequested);
    connect(samplePreview, &QPushButton::clicked, this,
            [this] { emit auditionRequested(root_->currentData().toInt()); });
    connect(sustainPreview_, &QPushButton::clicked, this, &TonePlayEditor::sustainAuditionRequested);
    connect(editSustain_, &QCheckBox::toggled, this, [this](bool on) {
        sample_->editSustain = on;
        sample_->update();
    });
    connect(zoom, &QPushButton::clicked, this, [this] { sample_->zoomToSlice(); });
    connect(whole, &QPushButton::clicked, this, [this] { sample_->wholeSong(); });
    connect(zoomOut, &QPushButton::clicked, this,
            [this] { sample_->zoom(1 / 1.6, sample_->width() * .5); });
    connect(zoomIn, &QPushButton::clicked, this,
            [this] { sample_->zoom(1.6, sample_->width() * .5); });
    connect(center, &QPushButton::clicked, this, [this] { roll_->centerRoot(); });
    connect(remove, &QPushButton::clicked, this, [this] { roll_->removeSelected(); });
    connect(duplicate, &QPushButton::clicked, this, [this] { roll_->duplicateSelection(); });
    connect(copy, &QPushButton::clicked, this, [this] { roll_->copySelection(); });
    connect(paste, &QPushButton::clicked, this, [this] { roll_->pasteSelection(); });
    connect(snap_, &QComboBox::currentIndexChanged, this, [this] {
        roll_->snap = snap_->currentData().toDouble();
        noteDuration_->setSingleStep(roll_->snap > 0 ? roll_->snap : 1.0 / 64);
        roll_->viewport()->update();
    });
    roll_->edited = [this](auto p, const auto &text) { return commit(std::move(p), text); };
    roll_->feedback = [this](const QString &text) { help_->setText(text); };
    roll_->audition = [this](int n) { emit auditionRequested(n); };
    roll_->selectionChanged = [this] { refreshNote(); };
    roll_->cursor = [this](double b) { emit cursorRequested(b); };
    sample_->changed = [this](double a, double b) {
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        if (sample_->editSustain && p.sustain) {
            p.sustain->loopStartBeat = a;
            p.sustain->loopEndBeat = b;
            commit(p, tr("Trim background vowel region"));
        } else {
            p.sourceStartBeat = a;
            p.sourceEndBeat = b;
            commit(p, tr("Trim tone snippet"));
        }
    };
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_)
            return;
        const bool isNew = !document_->file().tonePlay;
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        p.enabled = on;
        if (!document_->file().tonePlay) {
            p.sourceStartBeat = track_ ? std::max(0.0, track_->canonicalBeatAtSec(0)) : 0;
            p.sourceEndBeat = p.sourceStartBeat + .5;
            p.notes.push_back({0, .5, 60, .8});
        }
        commit(p, tr("Enable/disable tone play"));
        if (on && isNew)
            sample_->zoomToSlice();
        roll_->centerRoot();
    });
    const auto settings = [this] {
        if (refreshing_)
            return;
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        p.sourceStartBeat = start_->value();
        p.sourceEndBeat = end_->value();
        p.rootNote = root_->currentData().toInt();
        p.gain = gain_->value();
        p.replaceOutgoing = replace_->isChecked();
        commit(p, tr("Edit tone instrument"));
    };
    for (auto *s : {start_, end_, gain_})
        connect(s, &QDoubleSpinBox::editingFinished, this, settings);
    connect(root_, &QComboBox::currentIndexChanged, this, settings);
    connect(replace_, &QCheckBox::toggled, this, settings);
    connect(effectsEnabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_) return;
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        if (!p.effects) p.effects = TonePlayEffects{};
        p.effects->enabled = on;
        commit(p, tr("Enable/disable short-note effects"));
    });
    const auto effectSettings = [this] {
        if (refreshing_ || !document_->file().tonePlay || !document_->file().tonePlay->effects) return;
        auto p = *document_->file().tonePlay;
        p.effects->echo = echo_->value(); p.effects->reverb = reverb_->value();
        p.effects->echoBeats = echoBeats_->value(); p.effects->tailBeats = tailBeats_->value();
        p.effects->sustainDucking = holdDucking_->value();
        commit(p, tr("Edit short-note effects"));
    };
    for (auto *s : {echo_, reverb_, echoBeats_, tailBeats_, holdDucking_})
        connect(s, &QDoubleSpinBox::editingFinished, this, effectSettings);
    connect(sustainEnabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_) return;
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        if (!p.sustain) {
            TonePlaySustain h;
            const double size = p.sourceEndBeat - p.sourceStartBeat;
            h.loopStartBeat = p.sourceStartBeat + size * .35;
            h.loopEndBeat = p.sourceStartBeat + size * .85;
            h.pitch = p.rootNote;
            h.duration = std::clamp(tonePlayEndBeat(p), 1.0, 256.0);
            p.sustain = h;
        }
        p.sustain->enabled = on;
        commit(p, tr("Enable/disable background hold"));
    });
    const auto holdSettings = [this] {
        if (refreshing_ || !document_->file().tonePlay || !document_->file().tonePlay->sustain) return;
        auto p = *document_->file().tonePlay;
        auto &h = *p.sustain;
        h.beat = holdBeat_->value(); h.duration = holdDuration_->value();
        h.pitch = holdPitch_->currentData().toInt(); h.gain = holdGain_->value();
        h.loopStartBeat = loopStart_->value(); h.loopEndBeat = loopEnd_->value();
        h.crossfadeMs = crossfade_->value(); h.attackMs = attack_->value(); h.releaseMs = release_->value();
        commit(p, tr("Edit background hold"));
    };
    for (auto *s : {holdBeat_, holdDuration_, holdGain_, loopStart_, loopEnd_, crossfade_, attack_, release_})
        connect(s, &QDoubleSpinBox::editingFinished, this, holdSettings);
    connect(holdPitch_, &QComboBox::currentIndexChanged, this, holdSettings);
    const auto noteEdit = [this] {
        if (refreshing_ || roll_->selected < 0 ||
            roll_->selected >= int(roll_->pattern.notes.size()))
            return;
        auto p = roll_->pattern;
        auto &n = p.notes[roll_->selected];
        n.beat = noteBeat_->value();
        n.duration = noteDuration_->value();
        n.velocity = noteVelocity_->value();
        n.pitch = notePitch_->currentData().toInt();
        commit(p, tr("Edit tone note"));
    };
    for (auto *s : {noteBeat_, noteDuration_, noteVelocity_})
        connect(s, &QDoubleSpinBox::editingFinished, this, noteEdit);
    connect(notePitch_, &QComboBox::currentIndexChanged, this, noteEdit);
    refresh();
}
bool TonePlayEditor::commit(TonePlayPattern p, const QString &description) {
    if (document_->file().tonePlay &&
        serializeTonePlay(*document_->file().tonePlay) == serializeTonePlay(p)) {
        refresh();
        return true;
    }
    QString error;
    if (!validateTonePlay(p, &error)) {
        refresh();
        help_->setText(error);
        return false;
    }
    if (track_ && (track_->secAtCanonicalBeat(p.sourceStartBeat) < 0 ||
                   track_->secAtCanonicalBeat(p.sourceEndBeat) > track_->durationSec ||
                   (p.sourceEndBeat - p.sourceStartBeat) * 60 / track_->bpm > 8.000001)) {
        refresh();
        help_->setText(tr("Select at most 8 seconds inside the outgoing audio."));
        return false;
    }
    document_->mutate(description, [&](GvtFile &f) {
        f.tonePlay = p;
        if (f.requirements.isEmpty())
            f.requirements = {"timeline.v1", "temporary-cues.v1"};
        if (!f.requirements.contains("tone-play.v1"))
            f.requirements.append("tone-play.v1");
        if (p.sustain && !f.requirements.contains("tone-play-sustain.v1"))
            f.requirements.append("tone-play-sustain.v1");
        if (p.effects && !f.requirements.contains("tone-play-effects.v1"))
            f.requirements.append("tone-play-effects.v1");
        if (p.enabled && f.endBeat && *f.endBeat < tonePlayEndBeat(p))
            f.endBeat = tonePlayEndBeat(p);
    });
    refresh();
    return true;
}
void TonePlayEditor::setTrack(TrackDataPtr t) {
    if (track_ == t)
        return;
    track_ = std::move(t);
    sample_->track = track_;
    sample_->pattern = document_->file().tonePlay.value_or(TonePlayPattern{});
    if (document_->file().tonePlay)
        sample_->zoomToSlice();
    else
        sample_->wholeSong();
    refresh();
}
void TonePlayEditor::setPlayhead(double b) {
    roll_->playhead = b;
    const double x =
        TonePianoRoll::keys + b * roll_->pixelsPerBeat - roll_->horizontalScrollBar()->value();
    if (x < TonePianoRoll::keys || x > roll_->viewport()->width() - 20)
        roll_->horizontalScrollBar()->setValue(std::max(0, int(b * roll_->pixelsPerBeat) - 80));
    roll_->viewport()->update();
}
void TonePlayEditor::zoom(double factor) {
    const double center = roll_->viewport()->width() * .5;
    const double beat = roll_->beat(center);
    roll_->pixelsPerBeat = std::clamp(roll_->pixelsPerBeat * factor, 12.0, 1024.0);
    roll_->ranges();
    roll_->horizontalScrollBar()->setValue(
        int(TonePianoRoll::keys + beat * roll_->pixelsPerBeat - center));
    roll_->viewport()->update();
}
void TonePlayEditor::refresh() {
    refreshing_ = true;
    const auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
    enabled_->setChecked(document_->file().tonePlay && p.enabled);
    start_->setValue(p.sourceStartBeat);
    end_->setValue(p.sourceEndBeat);
    root_->setCurrentIndex(root_->findData(p.rootNote));
    gain_->setValue(p.gain);
    replace_->setChecked(p.replaceOutgoing);
    const auto fx = p.effects.value_or(TonePlayEffects{});
    effectsEnabled_->setChecked(p.effects && fx.enabled);
    effectsEnabled_->setEnabled(enabled_->isChecked());
    echo_->setValue(fx.echo); reverb_->setValue(fx.reverb);
    echoBeats_->setValue(fx.echoBeats); tailBeats_->setValue(fx.tailBeats);
    holdDucking_->setValue(fx.sustainDucking);
    for (QWidget *w : {echo_, reverb_, echoBeats_, tailBeats_, holdDucking_})
        w->setEnabled(enabled_->isChecked() && effectsEnabled_->isChecked());
    const auto h = p.sustain.value_or(TonePlaySustain{});
    sustainEnabled_->setChecked(p.sustain && h.enabled);
    sustainEnabled_->setEnabled(p.enabled && document_->file().tonePlay.has_value());
    holdBeat_->setValue(h.beat); holdDuration_->setValue(h.duration);
    holdGain_->setValue(h.gain); holdPitch_->setCurrentIndex(holdPitch_->findData(h.pitch));
    loopStart_->setValue(h.loopStartBeat); loopEnd_->setValue(h.loopEndBeat);
    crossfade_->setValue(h.crossfadeMs); attack_->setValue(h.attackMs); release_->setValue(h.releaseMs);
    const bool holdOn = enabled_->isChecked() && sustainEnabled_->isChecked();
    if (!holdOn) editSustain_->setChecked(false);
    for (QWidget *w : std::initializer_list<QWidget *>{editSustain_, sustainPreview_, holdBeat_, holdDuration_,
            holdPitch_, holdGain_, loopStart_, loopEnd_, crossfade_, attack_, release_})
        w->setEnabled(holdOn);
    sample_->pattern = p;
    sample_->update();
    const double length = p.sourceEndBeat - p.sourceStartBeat;
    sliceInfo_->setText(
        track_ && track_->bpm > 0
            ? tr("%1 s · %2 beats").arg(length * 60 / track_->bpm, 0, 'f', 3).arg(length, 0, 'g', 5)
            : tr("No source audio"));
    roll_->dragging = -1;
    roll_->pattern = p;
    const auto meter = document_->file().from.meter.split('/');
    roll_->barBeats = meter.size() == 2 && meter[0].toDouble() > 0 && meter[1].toDouble() > 0
                          ? meter[0].toDouble() * 4 / meter[1].toDouble()
                          : 4;
    roll_->selected = std::min(roll_->selected, int(p.notes.size()) - 1);
    std::erase_if(roll_->selection, [&](int i) { return i < 0 || i >= int(p.notes.size()); });
    roll_->endBeat = document_->effectiveEndBeat();
    roll_->ranges();
    roll_->viewport()->update();
    for (QWidget *w : std::initializer_list<QWidget *>{sample_, roll_, start_, end_, root_, gain_,
                                                       replace_, snap_})
        w->setEnabled(enabled_->isChecked());
    help_->setText(tr(
        "1  Select an outgoing snippet    2  Draw notes below    3  Preview the mix, then Save"));
    refreshing_ = false;
    refreshNote();
}
void TonePlayEditor::refreshNote() {
    refreshing_ = true;
    const bool valid = enabled_->isChecked() && roll_->selected >= 0 &&
                       roll_->selected < int(roll_->pattern.notes.size());
    for (QWidget *w :
         std::initializer_list<QWidget *>{noteBeat_, noteDuration_, notePitch_, noteVelocity_})
        w->setEnabled(valid);
    if (valid) {
        const auto &n = roll_->pattern.notes[roll_->selected];
        noteBeat_->setValue(n.beat);
        noteDuration_->setValue(n.duration);
        noteVelocity_->setValue(n.velocity);
        notePitch_->setCurrentIndex(notePitch_->findData(n.pitch));
    }
    refreshing_ = false;
}
} // namespace gvt
