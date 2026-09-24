// Two explicit rulers keep WHERE the sample comes from separate from WHEN
// notes play. Gesture previews are transient; release commits one undo command.
#include "TonePlayEditor.h"
#include "TransitionEditor.h"
#include <QAbstractScrollArea>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

namespace gvt {
class ToneSampleView final : public QWidget {
  public:
    TrackDataPtr track;
    TonePlayPattern pattern;
    std::function<void(double, double)> changed;
    double viewStart = 0, viewEnd = 32;
    int dragging = 0;
    explicit ToneSampleView(QWidget *parent) : QWidget(parent) {
        setObjectName("toneSampleWaveform");
        setMinimumHeight(110);
        setMaximumHeight(170);
        setMouseTracking(true);
        setToolTip("Drag across the outgoing waveform to choose a snippet. Drag either gold "
                   "boundary to trim. Use Zoom to slice for fine edits. No audio file is changed.");
    }
    void wholeSong() {
        if (!track || track->bpm <= 0)
            return;
        viewStart = track->canonicalBeatAtSec(0);
        viewEnd = track->canonicalBeatAtSec(track->durationSec);
        update();
    }
    void zoomToSlice() {
        const double margin = std::max(0.25, pattern.sourceEndBeat - pattern.sourceStartBeat);
        viewStart = pattern.sourceStartBeat - margin;
        viewEnd = pattern.sourceEndBeat + margin;
        update();
    }
    double x(double beat) const {
        return 10 + (beat - viewStart) / std::max(.001, viewEnd - viewStart) * (width() - 20);
    }
    double beat(double x) const {
        return viewStart + (x - 10) / std::max(1, width() - 20) * (viewEnd - viewStart);
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
        const double step = (viewEnd - viewStart) > 80 ? 16 : (viewEnd - viewStart) > 24 ? 4 : 1;
        for (double b = std::ceil(viewStart / step) * step; b <= viewEnd; b += step) {
            p.setPen(QColor(255, 255, 255, 45));
            p.drawLine(QPointF(x(b), 23), QPointF(x(b), height()));
            p.setPen(QColor("#aab4c0"));
            p.drawText(QPointF(x(b) + 3, 16), QString::number(b, 'f', 0));
        }
        const auto left = x(pattern.sourceStartBeat), right = x(pattern.sourceEndBeat);
        p.fillRect(QRectF(left, 24, right - left, height() - 24), QColor(242, 183, 83, 45));
        p.setPen(QPen(QColor("#f2b753"), 2));
        for (double edge : {left, right})
            p.drawLine(QPointF(edge, 24), QPointF(edge, height()));
        p.setPen(QColor("#f2b753"));
        p.drawText(12, height() - 8, "OUTGOING SONG BEATS — sample boundaries");
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton || !track)
            return;
        const double at = e->position().x();
        dragging = std::abs(at - x(pattern.sourceStartBeat)) < 9 ? 1
                   : std::abs(at - x(pattern.sourceEndBeat)) < 9 ? 2
                                                                 : 3;
        if (dragging == 3) {
            pattern.sourceStartBeat =
                std::clamp(beat(at), track->canonicalBeatAtSec(0),
                           track->canonicalBeatAtSec(track->durationSec) - .02);
            pattern.sourceEndBeat = std::min(pattern.sourceStartBeat + .25,
                                             track->canonicalBeatAtSec(track->durationSec));
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!dragging || !track)
            return;
        const double at = std::clamp(beat(e->position().x()), track->canonicalBeatAtSec(0),
                                     track->canonicalBeatAtSec(track->durationSec));
        const double maxLength = std::min(32.0, 8.0 * track->bpm / 60.0);
        if (dragging == 1)
            pattern.sourceStartBeat = std::clamp(
                at, std::max(track->canonicalBeatAtSec(0), pattern.sourceEndBeat - maxLength),
                pattern.sourceEndBeat - .01);
        else
            pattern.sourceEndBeat =
                std::clamp(at, pattern.sourceStartBeat + .01,
                           std::min(track->canonicalBeatAtSec(track->durationSec),
                                    pattern.sourceStartBeat + maxLength));
        update();
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (dragging && changed)
            changed(pattern.sourceStartBeat, pattern.sourceEndBeat);
        dragging = 0;
    }
};

class TonePianoRoll final : public QAbstractScrollArea {
  public:
    TonePlayPattern pattern;
    std::function<void(TonePlayPattern, const QString &)> edited;
    std::function<void(int)> audition;
    std::function<void()> selectionChanged;
    std::function<void(double)> cursor;
    int selected = -1, dragging = -1;
    bool resizeNote = false;
    bool centered = false;
    TonePlayNote original;
    QPointF press;
    double pixelsPerBeat = 48, snap = .25, playhead = 0, endBeat = 32;
    static constexpr int keys = 66, ruler = 26, row = 19, topPitch = 96, lowPitch = 24;
    explicit TonePianoRoll(QWidget *parent) : QAbstractScrollArea(parent) {
        setObjectName("tonePianoRoll");
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(220);
        setToolTip("Double-click to draw a note. Drag to move; drag its right edge to resize. "
                   "Delete removes the selected note. Click a piano key to hear it. Scroll "
                   "horizontally; Command+scroll zooms.");
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
        const double grid = pixelsPerBeat > 40 ? std::max(.125, snap) : 1.0;
        for (double b = std::floor(first / grid) * grid; b <= last; b += grid) {
            const int x = keys + b * pixelsPerBeat - horizontalScrollBar()->value();
            p.setPen(QColor(255, 255, 255, std::fmod(std::abs(b), 4.0) < .001 ? 70 : 22));
            p.drawLine(x, ruler, x, viewport()->height());
        }
        for (int i = 0; i < int(pattern.notes.size()); ++i) {
            const auto r = noteRect(pattern.notes[i]);
            p.setPen(
                QPen(i == selected ? QColor("#ffda94") : QColor("#83cbbd"), i == selected ? 2 : 1));
            p.setBrush(i == selected ? QColor("#b78238") : QColor("#3c887a"));
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
        p.restore();
        p.fillRect(0, 0, viewport()->width(), ruler, QColor("#151c22"));
        p.setPen(QColor("#b1c0ce"));
        p.drawText(5, 18, "NOTE");
        for (double b = std::ceil(first); b <= last; b += pixelsPerBeat < 28 ? 4 : 1) {
            const double bx = keys + b * pixelsPerBeat - horizontalScrollBar()->value();
            p.drawText(QPointF(bx + 3, 18), QString::number(b, 'f', 0));
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        setFocus();
        const auto at = e->position();
        if (at.y() < ruler) {
            if (cursor)
                cursor(beat(at.x()));
            return;
        }
        if (at.x() < keys) {
            if (e->button() == Qt::LeftButton && audition)
                audition(pitch(at.y()));
            return;
        }
        selected = hit(at);
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
            press = at;
            resizeNote = at.x() > noteRect(original).right() - 9;
        } else if (cursor)
            cursor(snapped(beat(at.x())));
        viewport()->update();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton || e->position().x() < keys ||
            e->position().y() < ruler || hit(e->position()) >= 0)
            return;
        const int n =
            std::clamp(pitch(e->position().y()), std::max(lowPitch, pattern.rootNote - 24),
                       std::min(topPitch, pattern.rootNote + 24));
        pattern.notes.push_back({snapped(beat(e->position().x())), std::max(.25, snap), n, .8});
        selected = int(pattern.notes.size()) - 1;
        if (edited)
            edited(pattern, "Draw tone note");
        if (selectionChanged)
            selectionChanged();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (dragging < 0)
            return;
        auto &n = pattern.notes[dragging];
        if (resizeNote)
            n.duration = std::clamp(
                snapped(original.duration + (e->position().x() - press.x()) / pixelsPerBeat),
                1.0 / 64, 64.0);
        else {
            n.beat = std::max(
                0.0, snapped(original.beat + (e->position().x() - press.x()) / pixelsPerBeat));
            n.pitch =
                std::clamp(original.pitch + int(std::round((press.y() - e->position().y()) / row)),
                           std::max(lowPitch, pattern.rootNote - 24),
                           std::min(topPitch, pattern.rootNote + 24));
        }
        viewport()->update();
        if (selectionChanged)
            selectionChanged();
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (dragging >= 0) {
            dragging = -1;
            if (edited)
                edited(pattern, "Move/resize tone note");
        }
    }
    void removeSelected() {
        if (selected < 0 || selected >= int(pattern.notes.size()))
            return;
        pattern.notes.erase(pattern.notes.begin() + selected);
        selected = std::min(selected, int(pattern.notes.size()) - 1);
        if (edited)
            edited(pattern, "Delete tone note");
        if (selectionChanged)
            selectionChanged();
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            removeSelected();
            e->accept();
        } else
            QAbstractScrollArea::keyPressEvent(e);
    }
    void wheelEvent(QWheelEvent *e) override {
        const auto delta = e->pixelDelta().isNull() ? e->angleDelta() : e->pixelDelta();
        if (e->modifiers() & (Qt::MetaModifier | Qt::ControlModifier)) {
            const double anchor = beat(e->position().x());
            pixelsPerBeat = std::clamp(pixelsPerBeat * std::exp(delta.y() * .0015), 12.0, 160.0);
            ranges();
            horizontalScrollBar()->setValue(int(keys + anchor * pixelsPerBeat - e->position().x()));
        } else if (e->modifiers() & Qt::ShiftModifier)
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        else
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() -
                                            (delta.x() ? delta.x() : delta.y()));
        viewport()->update();
        e->accept();
    }
};

TonePlayEditor::TonePlayEditor(TransitionEditorDocument *doc, QWidget *parent)
    : QWidget(parent), document_(doc) {
    setObjectName("tonePlayEditor");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto *header = new QHBoxLayout;
    enabled_ = new QCheckBox(tr("Enable tone play"));
    enabled_->setObjectName("tonePlayEnabled");
    header->addWidget(enabled_);
    auto *preview = new QPushButton(tr("▶ Preview mix"));
    preview->setObjectName("tonePreviewMix");
    header->addWidget(preview);
    auto *samplePreview = new QPushButton(tr("▶ Hear snippet"));
    header->addWidget(samplePreview);
    header->addStretch();
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
         {std::pair<const char *, QDoubleSpinBox *>{"Source IN", start_}, {"OUT", end_}}) {
        sampleTools->addWidget(new QLabel(tr(pair.first)));
        sampleTools->addWidget(pair.second);
    }
    auto *zoom = new QPushButton(tr("Zoom to slice"));
    auto *whole = new QPushButton(tr("Whole song"));
    sampleTools->addWidget(zoom);
    sampleTools->addWidget(whole);
    layout->addLayout(sampleTools);
    sample_ = new ToneSampleView(this);
    layout->addWidget(sample_);
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
    auto *noteTools = new QHBoxLayout;
    noteTools->addWidget(new QLabel(tr("PIANO ROLL — transition beats")));
    snap_ = new QComboBox;
    for (double v : {1.0, .5, .25, .125, .0625})
        snap_->addItem(QString::number(v) + " beat", v);
    snap_->setCurrentIndex(2);
    noteTools->addWidget(new QLabel(tr("Snap")));
    noteTools->addWidget(snap_);
    auto *center = new QPushButton(tr("Center root"));
    auto *duplicate = new QPushButton(tr("Duplicate"));
    auto *remove = new QPushButton(tr("Delete note"));
    noteTools->addWidget(center);
    noteTools->addWidget(duplicate);
    noteTools->addWidget(remove);
    layout->addLayout(noteTools);
    roll_ = new TonePianoRoll(this);
    layout->addWidget(roll_, 1);
    auto *noteForm = new QHBoxLayout;
    noteBeat_ = spin("toneNoteBeat", 0, 16384, 6);
    noteDuration_ = spin("toneNoteDuration", 1.0 / 64, 64, 6);
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
    auto *hint =
        new QLabel(tr("Double-click draws • drag moves • right edge resizes • Delete removes • "
                      "piano keys audition • wheel scrolls, ⌘wheel zooms\nOne-shot sampler: root "
                      "preserves pitch; higher notes play faster/shorter. Note length gates audio, "
                      "not time-stretch. Changes are undoable; only Save writes a recipe."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    connect(preview, &QPushButton::clicked, this, &TonePlayEditor::previewRequested);
    connect(samplePreview, &QPushButton::clicked, this,
            [this] { emit auditionRequested(root_->currentData().toInt()); });
    connect(zoom, &QPushButton::clicked, this, [this] { sample_->zoomToSlice(); });
    connect(whole, &QPushButton::clicked, this, [this] { sample_->wholeSong(); });
    connect(center, &QPushButton::clicked, this, [this] { roll_->centerRoot(); });
    connect(remove, &QPushButton::clicked, this, [this] { roll_->removeSelected(); });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        if (roll_->selected < 0 || roll_->selected >= int(roll_->pattern.notes.size()))
            return;
        auto p = roll_->pattern;
        auto n = p.notes[roll_->selected];
        n.beat += n.duration;
        p.notes.push_back(n);
        roll_->selected = int(p.notes.size()) - 1;
        commit(p, tr("Duplicate tone note"));
    });
    connect(snap_, &QComboBox::currentIndexChanged, this, [this] {
        roll_->snap = snap_->currentData().toDouble();
        roll_->viewport()->update();
    });
    roll_->edited = [this](auto p, const auto &text) { commit(std::move(p), text); };
    roll_->audition = [this](int n) { emit auditionRequested(n); };
    roll_->selectionChanged = [this] { refreshNote(); };
    roll_->cursor = [this](double b) { emit cursorRequested(b); };
    sample_->changed = [this](double a, double b) {
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        p.sourceStartBeat = a;
        p.sourceEndBeat = b;
        commit(p, tr("Trim tone snippet"));
    };
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_)
            return;
        auto p = document_->file().tonePlay.value_or(TonePlayPattern{});
        p.enabled = on;
        if (!document_->file().tonePlay) {
            p.sourceStartBeat = track_ ? std::max(0.0, track_->canonicalBeatAtSec(0)) : 0;
            p.sourceEndBeat = p.sourceStartBeat + .5;
            p.notes.push_back({0, .5, 60, .8});
        }
        commit(p, tr("Enable/disable tone play"));
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
void TonePlayEditor::commit(TonePlayPattern p, const QString &description) {
    if (document_->file().tonePlay &&
        serializeTonePlay(*document_->file().tonePlay) == serializeTonePlay(p)) {
        refresh();
        return;
    }
    QString error;
    if (!validateTonePlay(p, &error)) {
        refresh();
        help_->setText(error);
        return;
    }
    if (track_ && (track_->secAtCanonicalBeat(p.sourceStartBeat) < 0 ||
                   track_->secAtCanonicalBeat(p.sourceEndBeat) > track_->durationSec ||
                   (p.sourceEndBeat - p.sourceStartBeat) * 60 / track_->bpm > 8.000001)) {
        refresh();
        help_->setText(tr("Select at most 8 seconds inside the outgoing audio."));
        return;
    }
    document_->mutate(description, [&](GvtFile &f) {
        f.tonePlay = p;
        if (f.requirements.isEmpty())
            f.requirements = {"timeline.v1", "temporary-cues.v1"};
        if (!f.requirements.contains("tone-play.v1"))
            f.requirements.append("tone-play.v1");
        if (p.enabled && f.endBeat && *f.endBeat < tonePlayEndBeat(p))
            f.endBeat = tonePlayEndBeat(p);
    });
    refresh();
}
void TonePlayEditor::setTrack(TrackDataPtr t) {
    if (track_ == t)
        return;
    track_ = std::move(t);
    sample_->track = track_;
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
    roll_->pixelsPerBeat = std::clamp(roll_->pixelsPerBeat * factor, 12.0, 160.0);
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
    sample_->pattern = p;
    sample_->update();
    roll_->dragging = -1;
    roll_->pattern = p;
    roll_->selected = std::min(roll_->selected, int(p.notes.size()) - 1);
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
