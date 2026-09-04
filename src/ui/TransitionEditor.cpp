// Full transition authoring window. The GUI edits a typed working copy and
// auditions it through a private two-deck graph routed to the live MASTER.
#include "TransitionEditor.h"

#include "Theme.h"
#include "../audio/AudioEngine.h"
#include "../audio/MasterRecorder.h"
#include "../analysis/StemSeparator.h"
#include "../library/TrackLibrary.h"
#include "../performance/PerformancePads.h"
#include "../transitions/PlayerMath.h"
#include "../transitions/TransitionEngine.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineF>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace gvt {
namespace {

constexpr int kTimelineLeft = 104;
constexpr int kRulerHeight = 28;
constexpr int kWaveformHeight = 92;
constexpr int kActionHeight = 54;
constexpr int kLaneHeight = 74;
constexpr int kPreviewFrames = 256;
constexpr std::size_t kPreviewRingFrames = 32768;

QString roleText(Role role)
{
    switch (role) {
    case Role::FromDeck: return QObject::tr("Outgoing");
    case Role::ToDeck: return QObject::tr("Incoming");
    case Role::Mixer: return QObject::tr("Mixer");
    }
    return {};
}

QString controlText(ControlId control)
{
    QString text = QString::fromLatin1(controlName(control));
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QStringList words = text.split(QLatin1Char(' '));
    QStringList title;
    for (QString word : words) {
        if (!word.isEmpty()) word[0] = word[0].toUpper();
        title.append(word);
    }
    return title.join(QLatin1Char(' '));
}

bool editableTimelineControl(ControlId control)
{
    switch (control) {
    case ControlId::Load:
    case ControlId::Trim:
    case ControlId::HeadphoneCue:
    case ControlId::MasterCue:
    case ControlId::HeadphoneMix:
    case ControlId::Jog:
    case ControlId::BrowseSelect:
    case ControlId::BrowseNavigate:
    case ControlId::PlatterScratch:
    case ControlId::PlatterTouch:
    case ControlId::PerformancePadMode:
    case ControlId::PerformancePad1:
    case ControlId::PerformancePad2:
    case ControlId::PerformancePad3:
    case ControlId::PerformancePad4:
    case ControlId::PerformancePad5:
    case ControlId::PerformancePad6:
    case ControlId::PerformancePad7:
    case ControlId::PerformancePad8:
    case ControlId::TempoRange:
    case ControlId::Count:
        return false;
    default:
        return true;
    }
}

bool isStemControl(ControlId control)
{
    return control == ControlId::StemVocals ||
           control == ControlId::StemMelody ||
           control == ControlId::StemBass ||
           control == ControlId::StemDrums;
}

bool initialUsesStems(const GvtInitialState& state)
{
    constexpr double kUnityTolerance = 0.000001;
    return std::fabs(state.stemVocals - 1.0) > kUnityTolerance ||
           std::fabs(state.stemMelody - 1.0) > kUnityTolerance ||
           std::fabs(state.stemBass - 1.0) > kUnityTolerance ||
           std::fabs(state.stemDrums - 1.0) > kUnityTolerance;
}

std::pair<double, double> controlRange(ControlId control)
{
    switch (control) {
    case ControlId::Tempo: return {0.5, 1.5};
    case ControlId::FxBeats: return {0.25, 4.0};
    case ControlId::LoopAuto: return {0.03125, 64.0};
    case ControlId::BeatJump: return {-32.0, 32.0};
    case ControlId::FxType: return {0.0, 2.0};
    default: return {0.0, 1.0};
    }
}

std::pair<double, double> controlEditRange(ControlId control)
{
    switch (control) {
    case ControlId::Tempo: return {0.01, 4.0};
    case ControlId::FxBeats: return {0.25, 4.0};
    case ControlId::LoopAuto: return {0.03125, 64.0};
    case ControlId::BeatJump: return {-1024.0, 1024.0};
    case ControlId::FxType: return {0.0, 2.0};
    default: return {0.0, 1.0};
    }
}

double defaultControlValue(ControlId control)
{
    switch (control) {
    case ControlId::Tempo: return 1.0;
    case ControlId::EqLow:
    case ControlId::EqMid:
    case ControlId::EqHigh:
    case ControlId::Filter:
    case ControlId::FxWet: return 0.5;
    case ControlId::FxBeats: return 0.5;
    case ControlId::Fader:
    case ControlId::StemVocals:
    case ControlId::StemMelody:
    case ControlId::StemBass:
    case ControlId::StemDrums: return 1.0;
    default: return controlIsTrigger(control) ? 1.0 : 0.0;
    }
}

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

double latestBeat(const GvtFile& file)
{
    double result = 0.0;
    for (const GvtEvent& event : file.events)
        result = std::max(result, event.beat);
    for (const GvtCue& cue : file.cues)
        result = std::max(result, cue.beat);
    return result;
}

bool sameEndpointProfile(const GvtTrackRef& a, const GvtTrackRef& b)
{
    if (a.title != b.title || a.artist != b.artist ||
        a.artists != b.artists || a.versionName != b.versionName ||
        a.isrc != b.isrc ||
        a.musicBrainzRecording != b.musicBrainzRecording ||
        a.providerIds != b.providerIds || a.bpm != b.bpm ||
        a.durationSec != b.durationSec ||
        a.durationBeats != b.durationBeats || a.meter != b.meter ||
        a.referenceDownbeatSec != b.referenceDownbeatSec ||
        a.fingerprint != b.fingerprint || a.notes != b.notes ||
        a.extraYaml != b.extraYaml ||
        a.identityExtraYaml != b.identityExtraYaml ||
        a.identifiersExtraYaml != b.identifiersExtraYaml ||
        a.providersExtraYaml != b.providersExtraYaml ||
        a.assumptionsExtraYaml != b.assumptionsExtraYaml ||
        a.fingerprints.size() != b.fingerprints.size())
        return false;
    for (std::size_t index = 0; index < a.fingerprints.size(); ++index) {
        const TransitionFingerprint& left = a.fingerprints[index];
        const TransitionFingerprint& right = b.fingerprints[index];
        if (left.algorithm != right.algorithm || left.value != right.value ||
            left.extraYaml != right.extraYaml)
            return false;
    }
    return true;
}

class TransitionSnapshotCommand final : public QUndoCommand {
public:
    TransitionSnapshotCommand(TransitionEditorDocument* document,
                              GvtFile before, GvtFile after,
                              const QString& description)
        : QUndoCommand(description), document_(document),
          before_(std::move(before)), after_(std::move(after)) {}

    void undo() override { document_->setFromCommand(before_); }
    void redo() override { document_->setFromCommand(after_); }

private:
    TransitionEditorDocument* document_;
    GvtFile before_;
    GvtFile after_;
};

GvtInitialState defaultInitial(bool outgoing, double tempoRatio)
{
    GvtInitialState state;
    state.captured = true;
    state.playing = outgoing;
    state.positionBeat = 0.0;
    state.cueBeat = 0.0;
    state.tempoRatio = tempoRatio;
    state.fader = 1.0;
    state.eqLow = state.eqMid = state.eqHigh = 0.5;
    state.filter = 0.5;
    state.quantizeCaptured = true;
    state.quantize = true;
    state.fxType = 0;
    state.fxOn = false;
    state.fxWet = state.fxBeats = 0.5;
    state.stemVocals = state.stemMelody = state.stemBass = state.stemDrums = 1.0;
    return state;
}

} // namespace

TransitionEditorDocument::TransitionEditorDocument(QObject* parent)
    : QObject(parent), undo_(new QUndoStack(this))
{
    connect(undo_, &QUndoStack::cleanChanged, this,
            [this](bool clean) { emit dirtyChanged(!clean); });
}

bool TransitionEditorDocument::isDirty() const { return !undo_->isClean(); }

void TransitionEditorDocument::reset(const GvtFile& file)
{
    undo_->clear();
    file_ = file;
    undo_->setClean();
    emit changed();
    emit dirtyChanged(false);
}

void TransitionEditorDocument::apply(const GvtFile& file,
                                     const QString& description)
{
    undo_->push(new TransitionSnapshotCommand(this, file_, file, description));
}

void TransitionEditorDocument::mutate(
    const QString& description,
    const std::function<void(GvtFile&)>& mutation)
{
    GvtFile after = file_;
    mutation(after);
    apply(after, description);
}

void TransitionEditorDocument::markSaved() { undo_->setClean(); }

double TransitionEditorDocument::effectiveEndBeat() const
{
    return file_.endBeat.value_or(latestBeat(file_) + 1.0);
}

QStringList TransitionEditorDocument::validationErrors() const
{
    QStringList errors;
    if (file_.name.trimmed().isEmpty()) errors.append(tr("Transition needs a name"));
    if (file_.from.title.trimmed().isEmpty()) errors.append(tr("Choose an outgoing track"));
    if (file_.to.title.trimmed().isEmpty()) errors.append(tr("Choose an incoming track"));
    if (!std::isfinite(file_.masterBpm) || file_.masterBpm < 20.0 ||
        file_.masterBpm > 400.0)
        errors.append(tr("Master BPM must be between 20 and 400"));
    if (file_.events.empty()) errors.append(tr("Add at least one timeline action"));
    if (file_.endBeat.has_value() &&
        (!std::isfinite(*file_.endBeat) || *file_.endBeat < latestBeat(file_)))
        errors.append(tr("End beat must be after every action and label"));
    if (!file_.unsupportedRequirements.isEmpty())
        errors.append(tr("Unsupported requirements: %1")
                          .arg(file_.unsupportedRequirements.join(", ")));
    for (const Role role : {Role::FromDeck, Role::ToDeck}) {
        int count = 0;
        for (const TransitionHotCue& cue : file_.transitionCues)
            if (cue.role == role) ++count;
        for (const TransitionSavedLoop& loop : file_.transitionLoops)
            if (loop.role == role) ++count;
        if (count > 8)
            errors.append(tr("%1 has more than eight temporary cues/loops")
                              .arg(roleText(role)));
    }
    // The format reader is the single source of truth for reference,
    // control-range, role, and capability invariants. Validating an editor
    // snapshot through the deterministic serializer keeps GUI-only changes
    // from creating a document that the normal repository cannot reopen.
    GvtFile validated;
    QString schemaError;
    if (!transitionParse(transitionSerialize(file_), validated, &schemaError,
                         nullptr) &&
        !schemaError.isEmpty())
        errors.append(tr("Format validation: %1").arg(schemaError));
    errors.removeDuplicates();
    return errors;
}

void TransitionEditorDocument::setFromCommand(const GvtFile& file)
{
    file_ = file;
    emit changed();
}

TransitionTimelineView::TransitionTimelineView(
    TransitionEditorDocument* document, QWidget* parent)
    : QWidget(parent), document_(document)
{
    setObjectName(QStringLiteral("transitionEditorTimeline"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(document_, &TransitionEditorDocument::changed, this,
            [this] { updateCanvasSize(); update(); });
    updateCanvasSize();
}

void TransitionTimelineView::setTracks(TrackDataPtr outgoing,
                                       TrackDataPtr incoming)
{
    outgoing_ = std::move(outgoing);
    incoming_ = std::move(incoming);
    update();
}

void TransitionTimelineView::setPixelsPerBeat(double value)
{
    pixelsPerBeat_ = std::clamp(value, 8.0, 160.0);
    updateCanvasSize();
    update();
}

void TransitionTimelineView::setPlayheadBeat(double beat)
{
    if (!std::isfinite(beat)) return;
    playheadBeat_ = std::max(0.0, beat);
    update();
}

void TransitionTimelineView::setSelectedEvent(int index)
{
    selectedEvent_ = index;
    update();
}

std::vector<TransitionTimelineView::Lane> TransitionTimelineView::lanes() const
{
    std::vector<Lane> result;
    std::set<std::pair<int, int>> seen;
    for (const GvtEvent& event : document_->file().events) {
        if (controlIsTrigger(event.control)) continue;
        const auto key = std::make_pair(static_cast<int>(event.role),
                                        static_cast<int>(event.control));
        if (seen.insert(key).second) result.push_back({event.role, event.control});
    }
    if (result.empty()) result.push_back({Role::Mixer, ControlId::Crossfader});
    std::stable_sort(result.begin(), result.end(), [](const Lane& a, const Lane& b) {
        if (a.role != b.role) return static_cast<int>(a.role) < static_cast<int>(b.role);
        return static_cast<int>(a.control) < static_cast<int>(b.control);
    });
    return result;
}

void TransitionTimelineView::updateCanvasSize()
{
    const double end = std::max(32.0, document_->effectiveEndBeat() + 4.0);
    const int h = kRulerHeight + 2 * kWaveformHeight + kActionHeight +
                  static_cast<int>(lanes().size()) * kLaneHeight;
    setMinimumSize(std::max(1100, kTimelineLeft +
                           static_cast<int>(std::ceil(end * pixelsPerBeat_)) + 40),
                   std::max(420, h));
}

QRect TransitionTimelineView::waveformRect(int roleIndex) const
{
    return QRect(kTimelineLeft, kRulerHeight + roleIndex * kWaveformHeight,
                 width() - kTimelineLeft, kWaveformHeight);
}

QRect TransitionTimelineView::actionRect() const
{
    return QRect(kTimelineLeft, kRulerHeight + 2 * kWaveformHeight,
                 width() - kTimelineLeft, kActionHeight);
}

QRect TransitionTimelineView::automationRect(int laneIndex) const
{
    return QRect(kTimelineLeft,
                 kRulerHeight + 2 * kWaveformHeight + kActionHeight +
                     laneIndex * kLaneHeight,
                 width() - kTimelineLeft, kLaneHeight);
}

double TransitionTimelineView::incomingLaunchBeat() const
{
    for (const GvtEvent& event : document_->file().events) {
        if (event.role == Role::ToDeck && event.control == ControlId::Play &&
            event.value >= 0.5)
            return event.beat;
    }
    return 0.0;
}

double TransitionTimelineView::beatAtX(
    double x, Qt::KeyboardModifiers modifiers) const
{
    double beat = std::max(0.0, (x - kTimelineLeft) / pixelsPerBeat_);
    if (snapBeats_ > 0.0 && !(modifiers & Qt::AltModifier))
        beat = std::round(beat / snapBeats_) * snapBeats_;
    return beat;
}

double TransitionTimelineView::normalizedValue(ControlId control,
                                                double value) const
{
    const auto [minimum, maximum] = controlRange(control);
    return std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
}

double TransitionTimelineView::valueAtY(ControlId control, double y,
                                        int laneTop) const
{
    const auto [minimum, maximum] = controlRange(control);
    const double normalized = std::clamp(
        1.0 - (y - laneTop - 8.0) / (kLaneHeight - 16.0), 0.0, 1.0);
    double value = minimum + normalized * (maximum - minimum);
    if (control == ControlId::FxType) value = std::round(value);
    return value;
}

int TransitionTimelineView::laneAtY(double y) const
{
    const int top = kRulerHeight + 2 * kWaveformHeight + kActionHeight;
    if (y < top) return -1;
    const int lane = static_cast<int>((y - top) / kLaneHeight);
    return lane >= 0 && lane < static_cast<int>(lanes().size()) ? lane : -1;
}

int TransitionTimelineView::eventAt(const QPointF& point) const
{
    const auto laneList = lanes();
    const GvtFile& file = document_->file();
    int best = -1;
    double distance = 11.0;
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
        QPointF position;
        if (controlIsTrigger(event.control)) {
            position = QPointF(kTimelineLeft + event.beat * pixelsPerBeat_,
                               actionRect().center().y());
        } else {
            int laneIndex = -1;
            for (int lane = 0; lane < static_cast<int>(laneList.size()); ++lane)
                if (laneList[static_cast<std::size_t>(lane)].role == event.role &&
                    laneList[static_cast<std::size_t>(lane)].control == event.control)
                    laneIndex = lane;
            if (laneIndex < 0) continue;
            const QRect rect = automationRect(laneIndex);
            position = QPointF(kTimelineLeft + event.beat * pixelsPerBeat_,
                               rect.bottom() - 8.0 -
                                   normalizedValue(event.control, event.value) *
                                       (rect.height() - 16.0));
        }
        const double candidate = QLineF(position, point).length();
        if (candidate < distance) { distance = candidate; best = i; }
    }
    return best;
}

void TransitionTimelineView::drawWaveform(QPainter& painter,
                                          const QRect& rect,
                                          const TrackDataPtr& track,
                                          Role role)
{
    painter.fillRect(rect, role == Role::FromDeck ? QColor(19, 25, 31)
                                                   : QColor(29, 19, 27));
    if (!track || track->durationSec <= 0.0) {
        painter.setPen(themeDimText());
        painter.drawText(rect, Qt::AlignCenter, tr("Audio asset not resolved"));
        return;
    }

    const GvtFile& file = document_->file();
    const double anchor = role == Role::FromDeck ? file.anchorFromBeat
                                                  : file.anchorToBeat;
    const double launchBeat = role == Role::ToDeck ? incomingLaunchBeat() : 0.0;
    const auto& peaks = track->overviewPeaks;
    if (!peaks.empty()) {
        painter.setPen(role == Role::FromDeck ? QColor(77, 197, 226)
                                               : QColor(232, 93, 117));
        const int mid = rect.center().y();
        for (int x = rect.left(); x < rect.right(); ++x) {
            const double transitionBeat = (x - kTimelineLeft) / pixelsPerBeat_;
            const double trackBeat = anchor + transitionBeat - launchBeat;
            const double sec = transitionSecAtBeat(file, *track, trackBeat);
            if (sec < 0.0 || sec >= track->durationSec) continue;
            const int bin = std::clamp(
                static_cast<int>(sec / track->durationSec * peaks.size()),
                0, static_cast<int>(peaks.size()) - 1);
            const int height = static_cast<int>(
                std::clamp(peaks[static_cast<std::size_t>(bin)], 0.0f, 1.0f) *
                (rect.height() / 2 - 6));
            painter.drawLine(x, mid - height, x, mid + height);
        }
    }
    if (role == Role::ToDeck && launchBeat > 0.0) {
        const int launchX = kTimelineLeft + static_cast<int>(launchBeat * pixelsPerBeat_);
        painter.fillRect(QRect(rect.left(), rect.top(),
                               std::max(0, launchX - rect.left()), rect.height()),
                         QColor(0, 0, 0, 105));
    }

    const auto drawCue = [&](double trackBeat, const QString& label,
                             const QColor& color) {
        const double transitionBeat = trackBeat - anchor + launchBeat;
        const int x = kTimelineLeft + static_cast<int>(transitionBeat * pixelsPerBeat_);
        if (!rect.contains(x, rect.center().y())) return;
        painter.setPen(QPen(color, 2));
        painter.drawLine(x, rect.top(), x, rect.bottom());
        painter.drawText(QRect(x + 3, rect.top() + 3, 90, 18), label);
    };
    for (int index = 0; index < static_cast<int>(file.transitionCues.size()); ++index) {
        const TransitionHotCue& cue =
            file.transitionCues[static_cast<std::size_t>(index)];
        if (cue.role != role) continue;
        const double beat = dragDefinition_ == DragDefinition::Cue &&
                                    dragDefinitionIndex_ == index
                                ? dragDefinitionPreviewBeat_ : cue.trackBeat;
        drawCue(beat, cue.label.isEmpty() ? cue.id : cue.label,
                cue.color.isEmpty() ? QColor(85, 185, 223) : QColor(cue.color));
    }
    for (int index = 0; index < static_cast<int>(file.transitionLoops.size()); ++index) {
        const TransitionSavedLoop& loop =
            file.transitionLoops[static_cast<std::size_t>(index)];
        if (loop.role != role) continue;
        const double startTrackBeat =
            dragDefinition_ == DragDefinition::LoopStart &&
                    dragDefinitionIndex_ == index
                ? dragDefinitionPreviewBeat_ : loop.startTrackBeat;
        const double endTrackBeat =
            dragDefinition_ == DragDefinition::LoopEnd &&
                    dragDefinitionIndex_ == index
                ? dragDefinitionPreviewBeat_ : loop.endTrackBeat;
        const double startBeat = startTrackBeat - anchor + launchBeat;
        const double endBeat = endTrackBeat - anchor + launchBeat;
        QRectF region(kTimelineLeft + startBeat * pixelsPerBeat_, rect.top(),
                      (endBeat - startBeat) * pixelsPerBeat_, rect.height());
        painter.fillRect(region.intersected(rect), QColor(232, 161, 58, 45));
        drawCue(startTrackBeat,
                loop.label.isEmpty() ? loop.id : loop.label,
                QColor(232, 161, 58));
    }
}

void TransitionTimelineView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(18, 20, 24));
    const GvtFile& file = document_->file();
    const auto laneList = lanes();

    painter.fillRect(QRect(0, 0, width(), kRulerHeight), QColor(31, 35, 42));
    const double maximumBeat = (width() - kTimelineLeft) / pixelsPerBeat_;
    for (double beat = 0.0; beat <= maximumBeat; beat += 1.0) {
        const int x = kTimelineLeft + static_cast<int>(beat * pixelsPerBeat_);
        const bool bar = static_cast<int>(std::llround(beat)) % 4 == 0;
        painter.setPen(QPen(bar ? QColor(105, 112, 126) : QColor(57, 62, 74),
                            bar ? 1.5 : 1.0));
        painter.drawLine(x, kRulerHeight, x, height());
        if (bar) {
            painter.setPen(QColor(194, 199, 210));
            painter.drawText(QRect(x + 4, 3, 80, 20),
                             tr("%1 | %2").arg(static_cast<int>(beat / 4) + 1)
                                             .arg(static_cast<int>(beat) + 1));
        }
    }

    painter.setPen(QColor(85, 185, 223));
    painter.drawText(QRect(6, kRulerHeight, kTimelineLeft - 10, kWaveformHeight),
                     Qt::AlignVCenter, tr("OUTGOING"));
    painter.setPen(QColor(232, 93, 117));
    painter.drawText(QRect(6, kRulerHeight + kWaveformHeight,
                           kTimelineLeft - 10, kWaveformHeight),
                     Qt::AlignVCenter, tr("INCOMING"));
    drawWaveform(painter, waveformRect(0), outgoing_, Role::FromDeck);
    drawWaveform(painter, waveformRect(1), incoming_, Role::ToDeck);

    painter.fillRect(actionRect(), QColor(25, 28, 34));
    painter.setPen(themeDimText());
    painter.drawText(QRect(6, actionRect().top(), kTimelineLeft - 10,
                           actionRect().height()), Qt::AlignVCenter,
                     tr("ACTIONS"));
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
        if (!controlIsTrigger(event.control)) continue;
        const int x = kTimelineLeft + static_cast<int>(event.beat * pixelsPerBeat_);
        QRect card(x - 5, actionRect().top() + 8, 11, actionRect().height() - 16);
        const QColor color = event.role == Role::FromDeck ? QColor(85, 185, 223)
                             : event.role == Role::ToDeck ? QColor(232, 93, 117)
                                                          : QColor(232, 161, 58);
        painter.setBrush(color);
        painter.setPen(i == selectedEvent_ ? QPen(Qt::white, 2) : Qt::NoPen);
        painter.drawRoundedRect(card, 3, 3);
        painter.setPen(themeText());
        painter.drawText(QRect(x + 8, actionRect().top() + 4, 115,
                               actionRect().height() - 8),
                         Qt::AlignVCenter,
                         controlText(event.control));
    }

    for (int laneIndex = 0; laneIndex < static_cast<int>(laneList.size()); ++laneIndex) {
        const Lane lane = laneList[static_cast<std::size_t>(laneIndex)];
        const QRect laneRect = automationRect(laneIndex);
        painter.fillRect(laneRect, laneIndex % 2 ? QColor(25, 28, 34)
                                                 : QColor(22, 25, 30));
        painter.setPen(themeDimText());
        painter.drawText(QRect(6, laneRect.top(), kTimelineLeft - 10,
                               laneRect.height()), Qt::AlignVCenter,
                         roleText(lane.role).left(3).toUpper() + QLatin1Char(' ') +
                             controlText(lane.control));
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
            const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
            if (event.role == lane.role && event.control == lane.control)
                indices.push_back(i);
        }
        QPointF previous;
        bool havePrevious = false;
        for (int eventIndex : indices) {
            GvtEvent event = file.events[static_cast<std::size_t>(eventIndex)];
            if (eventIndex == dragEvent_) event = dragPreview_;
            const QPointF point(
                kTimelineLeft + event.beat * pixelsPerBeat_,
                laneRect.bottom() - 8.0 - normalizedValue(event.control, event.value) *
                    (laneRect.height() - 16.0));
            if (havePrevious) {
                painter.setPen(QPen(event.curve == Curve::SCurve
                                        ? QColor(176, 112, 232)
                                        : QColor(82, 193, 221), 2));
                if (event.curve == Curve::Step) {
                    painter.drawLine(previous, QPointF(point.x(), previous.y()));
                    painter.drawLine(QPointF(point.x(), previous.y()), point);
                } else {
                    painter.drawLine(previous, point);
                }
            }
            painter.setBrush(eventIndex == selectedEvent_ ? Qt::white
                                                           : QColor(85, 200, 232));
            painter.setPen(QPen(QColor(18, 20, 24), 1));
            painter.drawEllipse(point, eventIndex == selectedEvent_ ? 6 : 4,
                                eventIndex == selectedEvent_ ? 6 : 4);
            previous = point;
            havePrevious = true;
        }
    }

    for (int index = 0; index < static_cast<int>(file.cues.size()); ++index) {
        const GvtCue& cue = file.cues[static_cast<std::size_t>(index)];
        const double beat = dragDefinition_ == DragDefinition::Label &&
                                    dragDefinitionIndex_ == index
                                ? dragDefinitionPreviewBeat_ : cue.beat;
        const int x = kTimelineLeft + static_cast<int>(beat * pixelsPerBeat_);
        painter.setPen(QPen(QColor(246, 208, 94), 1.5));
        painter.drawLine(x, kRulerHeight, x, height());
        painter.drawText(QRect(x + 3, kRulerHeight + 2, 150, 18), cue.label);
    }

    const double shownEnd = dragEnd_ ? dragPreviewEnd_ : document_->effectiveEndBeat();
    const int endX = kTimelineLeft + static_cast<int>(shownEnd * pixelsPerBeat_);
    painter.setPen(QPen(QColor(244, 152, 66), 3));
    painter.drawLine(endX, 0, endX, height());
    painter.drawText(QRect(endX - 40, 2, 36, 20), Qt::AlignRight, tr("END"));

    const int playheadX = kTimelineLeft + static_cast<int>(playheadBeat_ * pixelsPerBeat_);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(playheadX, 0, playheadX, height());
}

void TransitionTimelineView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    const double endX = kTimelineLeft + document_->effectiveEndBeat() * pixelsPerBeat_;
    if (std::fabs(event->position().x() - endX) < 8.0) {
        dragEnd_ = true;
        dragStart_ = event->position();
        dragOriginalEnd_ = document_->effectiveEndBeat();
        dragPreviewEnd_ = dragOriginalEnd_;
        setCursor(Qt::SizeHorCursor);
        event->accept();
        return;
    }
    if (event->position().y() < kRulerHeight) {
        for (int index = 0;
             index < static_cast<int>(document_->file().cues.size()); ++index) {
            const double x = kTimelineLeft +
                document_->file().cues[static_cast<std::size_t>(index)].beat *
                    pixelsPerBeat_;
            if (std::fabs(event->position().x() - x) >= 8.0) continue;
            dragDefinition_ = DragDefinition::Label;
            dragDefinitionIndex_ = index;
            dragDefinitionOriginalBeat_ =
                document_->file().cues[static_cast<std::size_t>(index)].beat;
            dragDefinitionPreviewBeat_ = dragDefinitionOriginalBeat_;
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
    }
    for (int roleIndex = 0; roleIndex < 2; ++roleIndex) {
        if (!waveformRect(roleIndex).contains(event->position().toPoint())) continue;
        const Role role = roleIndex == 0 ? Role::FromDeck : Role::ToDeck;
        const double anchor = role == Role::FromDeck
                                  ? document_->file().anchorFromBeat
                                  : document_->file().anchorToBeat;
        const double launch = role == Role::ToDeck ? incomingLaunchBeat() : 0.0;
        const auto markerX = [&](double trackBeat) {
            return kTimelineLeft + (trackBeat - anchor + launch) * pixelsPerBeat_;
        };
        for (int index = 0;
             index < static_cast<int>(document_->file().transitionCues.size());
             ++index) {
            const TransitionHotCue& cue = document_->file().transitionCues[
                static_cast<std::size_t>(index)];
            if (cue.role != role ||
                std::fabs(event->position().x() - markerX(cue.trackBeat)) >= 8.0)
                continue;
            dragDefinition_ = DragDefinition::Cue;
            dragDefinitionIndex_ = index;
            dragDefinitionRole_ = role;
            dragDefinitionOriginalBeat_ = cue.trackBeat;
            dragDefinitionPreviewBeat_ = cue.trackBeat;
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
        for (int index = 0;
             index < static_cast<int>(document_->file().transitionLoops.size());
             ++index) {
            const TransitionSavedLoop& loop = document_->file().transitionLoops[
                static_cast<std::size_t>(index)];
            if (loop.role != role) continue;
            const bool atStart = std::fabs(
                event->position().x() - markerX(loop.startTrackBeat)) < 8.0;
            const bool atEnd = std::fabs(
                event->position().x() - markerX(loop.endTrackBeat)) < 8.0;
            if (!atStart && !atEnd) continue;
            dragDefinition_ = atStart ? DragDefinition::LoopStart
                                      : DragDefinition::LoopEnd;
            dragDefinitionIndex_ = index;
            dragDefinitionRole_ = role;
            dragDefinitionOriginalBeat_ = atStart ? loop.startTrackBeat
                                                  : loop.endTrackBeat;
            dragDefinitionPreviewBeat_ = dragDefinitionOriginalBeat_;
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
    }
    const int index = eventAt(event->position());
    if (index >= 0) {
        selectedEvent_ = index;
        dragEvent_ = index;
        dragStart_ = event->position();
        dragOriginal_ = document_->file().events[static_cast<std::size_t>(index)];
        dragPreview_ = dragOriginal_;
        emit eventSelected(index);
        setCursor(Qt::ClosedHandCursor);
        update();
        event->accept();
        return;
    }
    selectedEvent_ = -1;
    emit eventSelected(-1);
    setPlayheadBeat(beatAtX(event->position().x(), event->modifiers()));
    emit playheadChanged(playheadBeat_);
    event->accept();
}

void TransitionTimelineView::mouseMoveEvent(QMouseEvent* event)
{
    if (dragDefinition_ != DragDefinition::None) {
        const double transitionBeat = beatAtX(event->position().x(),
                                              event->modifiers());
        if (dragDefinition_ == DragDefinition::Label) {
            dragDefinitionPreviewBeat_ = transitionBeat;
        } else {
            const double anchor = dragDefinitionRole_ == Role::FromDeck
                                      ? document_->file().anchorFromBeat
                                      : document_->file().anchorToBeat;
            const double launch = dragDefinitionRole_ == Role::ToDeck
                                      ? incomingLaunchBeat() : 0.0;
            dragDefinitionPreviewBeat_ = anchor + transitionBeat - launch;
            if (dragDefinitionIndex_ >= 0 &&
                dragDefinitionIndex_ < static_cast<int>(
                    document_->file().transitionLoops.size())) {
                const TransitionSavedLoop& loop =
                    document_->file().transitionLoops[
                        static_cast<std::size_t>(dragDefinitionIndex_)];
                const double minimumGap = snapBeats_ > 0.0
                                              ? std::min(snapBeats_, 0.25)
                                              : 0.001;
                if (dragDefinition_ == DragDefinition::LoopStart)
                    dragDefinitionPreviewBeat_ = std::min(
                        dragDefinitionPreviewBeat_, loop.endTrackBeat - minimumGap);
                else if (dragDefinition_ == DragDefinition::LoopEnd)
                    dragDefinitionPreviewBeat_ = std::max(
                        dragDefinitionPreviewBeat_, loop.startTrackBeat + minimumGap);
            }
        }
        update();
        event->accept();
        return;
    }
    if (dragEnd_) {
        dragPreviewEnd_ = std::max(latestBeat(document_->file()),
                                   beatAtX(event->position().x(), event->modifiers()));
        update();
        event->accept();
        return;
    }
    if (dragEvent_ >= 0) {
        dragPreview_ = dragOriginal_;
        dragPreview_.beat = beatAtX(event->position().x(), event->modifiers());
        if (!controlIsTrigger(dragPreview_.control)) {
            const auto laneList = lanes();
            for (int lane = 0; lane < static_cast<int>(laneList.size()); ++lane) {
                if (laneList[static_cast<std::size_t>(lane)].role == dragPreview_.role &&
                    laneList[static_cast<std::size_t>(lane)].control == dragPreview_.control) {
                    dragPreview_.value = valueAtY(dragPreview_.control,
                                                   event->position().y(),
                                                   automationRect(lane).top());
                    break;
                }
            }
        }
        update();
        event->accept();
        return;
    }
    const double endX = kTimelineLeft + document_->effectiveEndBeat() * pixelsPerBeat_;
    setCursor(std::fabs(event->position().x() - endX) < 8.0
                  ? Qt::SizeHorCursor
                  : eventAt(event->position()) >= 0 ? Qt::OpenHandCursor
                                                    : Qt::ArrowCursor);
}

void TransitionTimelineView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(event);
    if (dragDefinition_ != DragDefinition::None) {
        const DragDefinition kind = dragDefinition_;
        const int index = dragDefinitionIndex_;
        const double beat = dragDefinitionPreviewBeat_;
        document_->mutate(tr("Move cue or loop marker"),
                          [kind, index, beat](GvtFile& file) {
            if (kind == DragDefinition::Cue && index >= 0 &&
                index < static_cast<int>(file.transitionCues.size()))
                file.transitionCues[static_cast<std::size_t>(index)].trackBeat = beat;
            else if (kind == DragDefinition::LoopStart && index >= 0 &&
                     index < static_cast<int>(file.transitionLoops.size()))
                file.transitionLoops[static_cast<std::size_t>(index)].startTrackBeat = beat;
            else if (kind == DragDefinition::LoopEnd && index >= 0 &&
                     index < static_cast<int>(file.transitionLoops.size()))
                file.transitionLoops[static_cast<std::size_t>(index)].endTrackBeat = beat;
            else if (kind == DragDefinition::Label && index >= 0 &&
                     index < static_cast<int>(file.cues.size())) {
                file.cues[static_cast<std::size_t>(index)].beat = beat;
                std::stable_sort(file.cues.begin(), file.cues.end(),
                                 [](const GvtCue& a, const GvtCue& b) {
                    return a.beat < b.beat;
                });
            }
        });
        dragDefinition_ = DragDefinition::None;
        dragDefinitionIndex_ = -1;
    } else if (dragEnd_) {
        const double value = dragPreviewEnd_;
        document_->mutate(tr("Move transition end"), [value](GvtFile& file) {
            file.endBeat = value;
            if (!file.requirements.contains(QStringLiteral("timeline-end.v1")))
                file.requirements.append(QStringLiteral("timeline-end.v1"));
        });
        dragEnd_ = false;
    } else if (dragEvent_ >= 0) {
        const int index = dragEvent_;
        const GvtEvent changed = dragPreview_;
        document_->mutate(tr("Move timeline point"), [index, changed](GvtFile& file) {
            if (index < 0 || index >= static_cast<int>(file.events.size())) return;
            file.events[static_cast<std::size_t>(index)] = changed;
            std::stable_sort(file.events.begin(), file.events.end(),
                             [](const GvtEvent& a, const GvtEvent& b) {
                                 return a.beat < b.beat;
                             });
        });
        dragEvent_ = -1;
        // Sorting can move the edited point to a different vector index.
        // Clear selection rather than leaving the inspector attached to a
        // different event that happened to inherit the old index.
        selectedEvent_ = -1;
        emit eventSelected(-1);
    }
    setCursor(Qt::ArrowCursor);
    event->accept();
}

void TransitionTimelineView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int laneIndex = laneAtY(event->position().y());
    if (laneIndex < 0) return QWidget::mouseDoubleClickEvent(event);
    const auto laneList = lanes();
    const Lane lane = laneList[static_cast<std::size_t>(laneIndex)];
    GvtEvent added;
    added.role = lane.role;
    added.control = lane.control;
    added.beat = beatAtX(event->position().x(), event->modifiers());
    added.value = valueAtY(lane.control, event->position().y(),
                           automationRect(laneIndex).top());
    added.curve = Curve::Linear;
    document_->mutate(tr("Add automation point"), [added](GvtFile& file) {
        file.events.push_back(added);
        std::stable_sort(file.events.begin(), file.events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    });
    event->accept();
}

struct TransitionEditorWindow::Preview final : AudioPreviewSource {
    ControlBus bus;
    AudioEngine engine {&bus};
    GvtFile file;
    TrackDataPtr outgoing;
    TrackDataPtr incoming;
    std::vector<ScheduledEvent> schedule;
    std::vector<bool> fired;
    std::array<float, kPreviewRingFrames * 2U> ring {};
    std::atomic<std::uint64_t> writeFrame {0};
    std::atomic<std::uint64_t> readFrame {0};
    double beat = 0.0;
    double endBeat = 0.0;
    bool active = false;
    bool leased = false;
    ControlId incomingPreviewControl = ControlId::Count;

    static double engineValue(AudioEngine& engine, Role role, ControlId control)
    {
        if (role == Role::Mixer)
            return control == ControlId::Crossfader ? engine.crossfader.load() : 0.0;
        const Deck& deck = engine.deck(role == Role::FromDeck ? 0 : 1);
        switch (control) {
        case ControlId::Tempo: return deck.tempoRatio.load();
        case ControlId::Fader: return deck.fader.load();
        case ControlId::EqLow: return deck.eqLow.load();
        case ControlId::EqMid: return deck.eqMid.load();
        case ControlId::EqHigh: return deck.eqHigh.load();
        case ControlId::Filter: return deck.filter.load();
        case ControlId::FxWet: return deck.fxWet.load();
        case ControlId::FxBeats: return deck.fxBeats.load();
        case ControlId::FxType: return deck.fxType.load();
        case ControlId::FxOn: return deck.fxOn.load() ? 1.0 : 0.0;
        case ControlId::Quantize:
            return deck.quantizeHotCues.load() ? 1.0 : 0.0;
        case ControlId::StemVocals: return deck.stemVocals.load();
        case ControlId::StemMelody: return deck.stemMelody.load();
        case ControlId::StemBass: return deck.stemBass.load();
        case ControlId::StemDrums: return deck.stemDrums.load();
        default: return 0.0;
        }
    }

    void clearRing()
    {
        readFrame.store(0, std::memory_order_relaxed);
        writeFrame.store(0, std::memory_order_relaxed);
    }

    void read(float* output, int frames) noexcept override
    {
        const std::uint64_t read = readFrame.load(std::memory_order_relaxed);
        const std::uint64_t write = writeFrame.load(std::memory_order_acquire);
        const int available = static_cast<int>(std::min<std::uint64_t>(
            write - read, static_cast<std::uint64_t>(frames)));
        for (int frame = 0; frame < available; ++frame) {
            const std::size_t ringFrame = static_cast<std::size_t>(
                (read + static_cast<std::uint64_t>(frame)) % kPreviewRingFrames);
            output[static_cast<std::size_t>(frame) * 2U] = ring[ringFrame * 2U];
            output[static_cast<std::size_t>(frame) * 2U + 1U] = ring[ringFrame * 2U + 1U];
        }
        std::fill(output + static_cast<std::size_t>(available) * 2U,
                  output + static_cast<std::size_t>(frames) * 2U, 0.0f);
        readFrame.store(read + static_cast<std::uint64_t>(available),
                        std::memory_order_release);
    }

    void applyInitial(const GvtInitialState& state, int deckIndex)
    {
        Deck& deck = engine.deck(deckIndex);
        deck.tempoRatio.store(state.tempoRatio);
        deck.fader.store(state.fader);
        deck.eqLow.store(state.eqLow);
        deck.eqMid.store(state.eqMid);
        deck.eqHigh.store(state.eqHigh);
        deck.filter.store(state.filter);
        deck.quantizeHotCues.store(state.quantize);
        deck.fxType.store(state.fxType);
        deck.fxOn.store(state.fxOn);
        deck.fxWet.store(state.fxWet);
        deck.fxBeats.store(state.fxBeats);
        deck.stemVocals.store(state.stemVocals);
        deck.stemMelody.store(state.stemMelody);
        deck.stemBass.store(state.stemBass);
        deck.stemDrums.store(state.stemDrums);
        const TrackDataPtr track = deck.track();
        if (track) {
            deck.seekSec(transitionSecAtBeat(file, *track, state.positionBeat));
            deck.cuePointSec.store(transitionSecAtBeat(file, *track, state.cueBeat));
            deck.loopStartSec.store(transitionSecAtBeat(file, *track,
                                                        state.loopStartBeat));
            deck.loopEndSec.store(transitionSecAtBeat(file, *track,
                                                      state.loopEndBeat));
            deck.loopActive.store(state.loopActive &&
                                  state.loopEndBeat > state.loopStartBeat);
        }
        if (state.playing) deck.play(); else deck.stop();
    }

    void prepareSlots(Role role, int deckIndex)
    {
        std::array<double, 8> starts {-1,-1,-1,-1,-1,-1,-1,-1};
        std::array<double, 8> ends {-1,-1,-1,-1,-1,-1,-1,-1};
        const TrackDataPtr track = engine.deck(deckIndex).track();
        const auto performanceSlots = transitionPerformanceSlots(file, role);
        for (int index = 0; index < 8 && track; ++index) {
            const auto& slot = performanceSlots[static_cast<std::size_t>(index)];
            if (slot.cue)
                starts[static_cast<std::size_t>(index)] = transitionSecAtBeat(
                    file, *track, slot.cue->trackBeat);
            else if (slot.loop) {
                starts[static_cast<std::size_t>(index)] = transitionSecAtBeat(
                    file, *track, slot.loop->startTrackBeat);
                ends[static_cast<std::size_t>(index)] = transitionSecAtBeat(
                    file, *track, slot.loop->endTrackBeat);
            }
        }
        engine.deck(deckIndex).setTransitionPerformanceSlots(starts, ends);
    }

    void reset(const GvtFile& source, TrackDataPtr out, TrackDataPtr in,
               StemSetPtr outStems, StemSetPtr inStems)
    {
        file = source;
        outgoing = std::move(out);
        incoming = std::move(in);
        engine.deck(0).loadTrack(outgoing);
        engine.deck(1).loadTrack(incoming);
        if (outStems) engine.deck(0).attachStems(std::move(outStems));
        if (inStems) engine.deck(1).attachStems(std::move(inStems));
        prepareSlots(Role::FromDeck, 0);
        prepareSlots(Role::ToDeck, 1);
        engine.crossfader.store(file.initialMixerCaptured
                                   ? static_cast<float>(file.initialCrossfader)
                                   : 0.0f);
        if (file.initialComplete) {
            GvtInitialState fromState = file.initialFrom;
            GvtInitialState toState = file.initialTo;
            fromState.tempoRatio = transitionReplayTempoRatio(
                fromState, file.from, outgoing, file.masterBpm);
            toState.tempoRatio = transitionReplayTempoRatio(
                toState, file.to, incoming);
            applyInitial(fromState, 0);
            applyInitial(toState, 1);
        } else {
            engine.deck(0).seekSec(transitionSecAtBeat(file, *outgoing,
                                                       file.anchorFromBeat));
            engine.deck(1).seekSec(transitionSecAtBeat(file, *incoming,
                                                       file.anchorToBeat));
            if (file.initialFrom.captured) {
                Deck& deck = engine.deck(0);
                deck.tempoRatio.store(transitionReplayTempoRatio(
                    file.initialFrom, file.from, outgoing, file.masterBpm));
                deck.fader.store(file.initialFrom.fader);
                deck.eqLow.store(file.initialFrom.eqLow);
                deck.eqMid.store(file.initialFrom.eqMid);
                deck.eqHigh.store(file.initialFrom.eqHigh);
                deck.filter.store(file.initialFrom.filter);
            }
            engine.deck(0).play();
            engine.deck(1).stop();
        }

        std::vector<GvtEvent> events = file.events;
        for (GvtEvent& event : events) {
            if (event.control == ControlId::Tempo) {
                event.value = event.role == Role::FromDeck
                    ? transitionReplayTempoEvent(event.value, file.from,
                                                 outgoing)
                    : transitionReplayTempoEvent(event.value, file.to,
                                                 incoming);
            }
            if (event.role == Role::Mixer ||
                (event.cueId.isEmpty() && event.loopId.isEmpty())) continue;
            const auto performanceSlots = transitionPerformanceSlots(file, event.role);
            for (int index = 0; index < 8; ++index) {
                const auto& slot = performanceSlots[static_cast<std::size_t>(index)];
                if ((!event.cueId.isEmpty() && slot.cue &&
                     slot.cue->id == event.cueId) ||
                    (!event.loopId.isEmpty() && slot.loop &&
                     slot.loop->id == event.loopId)) {
                    event.control = static_cast<ControlId>(
                        static_cast<int>(ControlId::TransitionCue1) + index);
                    break;
                }
            }
        }
        schedule = buildSchedule(events, [this](Role role, ControlId control) {
            return engineValue(engine, role, control);
        });
        fired.assign(schedule.size(), false);
        beat = 0.0;
        endBeat = file.endBeat.value_or(latestBeat(file) + 1.0);
        incomingPreviewControl = ControlId::Count;
        clearRing();
    }

    void dispatch(Role role, ControlId control, double value)
    {
        const int deck = role == Role::FromDeck ? 0
                       : role == Role::ToDeck ? 1 : kNoDeck;
        if (role == Role::ToDeck && control == ControlId::Play && value >= 0.5) {
            Deck& incomingDeck = engine.deck(1);
            if (incomingPreviewControl == ControlId::Count ||
                !incomingDeck.previewActive())
                incomingDeck.seekSec(transitionSecAtBeat(
                    file, *incoming, file.anchorToBeat));
        }
        bus.dispatch({deck, control, value}, Origin::Replay);
        const bool previewControl = control == ControlId::Cue ||
            (control >= ControlId::TransitionCue1 &&
             control <= ControlId::TransitionCue8) ||
            (control >= ControlId::SavedLoop1 && control <= ControlId::SavedLoop8);
        if (role == Role::ToDeck && previewControl) {
            if (value >= 0.5 && engine.deck(1).previewActive())
                incomingPreviewControl = control;
            else if (value < 0.5 && incomingPreviewControl == control)
                incomingPreviewControl = ControlId::Count;
        }
    }

    void renderBlock(float* output, bool enqueue)
    {
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            ScheduledEvent& scheduled = schedule[index];
            if (!fired[index] && beat >= scheduled.e.beat) {
                dispatch(scheduled.e.role, scheduled.e.control,
                         scheduled.e.value);
                fired[index] = true;
            } else if (!fired[index] && scheduledIsGlide(scheduled) &&
                       beat > scheduled.startBeat) {
                dispatch(scheduled.e.role, scheduled.e.control,
                         glideValueAt(scheduled, beat));
            }
        }
        engine.renderOffline(output, kPreviewFrames);
        const double bpm = engine.deck(0).effectiveBpm() > 0.0
                               ? engine.deck(0).effectiveBpm() : file.masterBpm;
        beat += static_cast<double>(kPreviewFrames) /
                static_cast<double>(kSampleRate) * bpm / 60.0;
        if (!enqueue) return;
        const std::uint64_t write = writeFrame.load(std::memory_order_relaxed);
        for (int frame = 0; frame < kPreviewFrames; ++frame) {
            const std::size_t ringFrame = static_cast<std::size_t>(
                (write + static_cast<std::uint64_t>(frame)) % kPreviewRingFrames);
            ring[ringFrame * 2U] = output[static_cast<std::size_t>(frame) * 2U];
            ring[ringFrame * 2U + 1U] = output[static_cast<std::size_t>(frame) * 2U + 1U];
        }
        writeFrame.store(write + kPreviewFrames, std::memory_order_release);
    }

    void primeTo(double wantedBeat)
    {
        std::array<float, static_cast<std::size_t>(kPreviewFrames) * 2U> scratch {};
        while (beat + 1e-9 < wantedBeat && beat < endBeat)
            renderBlock(scratch.data(), false);
        clearRing();
    }

    void produce()
    {
        if (!active) return;
        std::array<float, static_cast<std::size_t>(kPreviewFrames) * 2U> scratch {};
        const std::uint64_t read = readFrame.load(std::memory_order_acquire);
        while (writeFrame.load(std::memory_order_relaxed) - read < 1536 &&
               beat < endBeat)
            renderBlock(scratch.data(), true);
        if (beat >= endBeat &&
            writeFrame.load(std::memory_order_acquire) ==
                readFrame.load(std::memory_order_acquire))
            active = false;
    }
};

TransitionEditorWindow::TransitionEditorWindow(
    AudioEngine* liveEngine, TrackLibrary* library, TransitionStore* store,
    TransitionRecorder* recorder, TransitionPlayer* player,
    MasterRecorder* masterRecorder, StemSeparator* stemSeparator,
    QWidget* parent)
    : QMainWindow(parent), liveEngine_(liveEngine), library_(library),
      store_(store), recorder_(recorder), player_(player),
      masterRecorder_(masterRecorder), stemSeparator_(stemSeparator),
      document_(new TransitionEditorDocument(this)),
      preview_(std::make_unique<Preview>())
{
    setObjectName(QStringLiteral("transitionEditorWindow"));
    setWindowFlag(Qt::Window, true);
    setWindowTitle(tr("Transition Editor — Gravitino"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(1100, 700);
    resize(1450, 900);
    const QByteArray geometry = QSettings().value(
        QStringLiteral("transitionEditor/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    buildUi();

    connect(document_, &TransitionEditorDocument::changed, this, [this] {
        if (preview_ && preview_->active) stopPreview();
        scheduleDraftSave();
        refreshUi();
    });
    connect(document_, &TransitionEditorDocument::dirtyChanged, this,
            [this](bool) { refreshUi(); });

    if (stemSeparator_) {
        connect(stemSeparator_, &StemSeparator::progress, this,
                [this](const QString& fingerprint, const QString& stage) {
            if ((outgoing_ && outgoing_->fingerprint == fingerprint) ||
                (incoming_ && incoming_->fingerprint == fingerprint)) {
                prepareStemsButton_->setText(tr("STEMS: %1").arg(stage));
                prepareStemsButton_->setEnabled(false);
            }
        });
        connect(stemSeparator_, &StemSeparator::stemsReady, this,
                [this](const QString& fingerprint, StemSetPtr stems) {
            if (outgoing_ && outgoing_->fingerprint == fingerprint)
                outgoingStems_ = stems;
            if (incoming_ && incoming_->fingerprint == fingerprint)
                incomingStems_ = stems;
            refreshUi();
            statusBar()->showMessage(tr("Editor stems are ready for preview"),
                                     4000);
        });
        connect(stemSeparator_, &StemSeparator::stemsFailed, this,
                [this](const QString& fingerprint, const QString& error) {
            if ((!outgoing_ || outgoing_->fingerprint != fingerprint) &&
                (!incoming_ || incoming_->fingerprint != fingerprint))
                return;
            refreshUi();
            statusBar()->showMessage(tr("Could not prepare editor stems: %1")
                                         .arg(error), 8000);
        });
    }

    draftTimer_ = new QTimer(this);
    draftTimer_->setSingleShot(true);
    draftTimer_->setInterval(1000);
    connect(draftTimer_, &QTimer::timeout, this,
            &TransitionEditorWindow::writeDraft);
    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(5);
    previewTimer_->setTimerType(Qt::PreciseTimer);
    connect(previewTimer_, &QTimer::timeout, this,
            &TransitionEditorWindow::updatePreviewTick);
}

TransitionEditorWindow::~TransitionEditorWindow()
{
    stopPreview();
}

void TransitionEditorWindow::buildUi()
{
    auto* toolbar = addToolBar(tr("Editor"));
    toolbar->setObjectName(QStringLiteral("transitionEditorToolbar"));
    toolbar->setMovable(false);
    saveAction_ = toolbar->addAction(tr("Save"), this,
                                     &TransitionEditorWindow::save);
    saveAction_->setShortcut(QKeySequence::Save);
    saveAsAction_ = toolbar->addAction(tr("Save As…"), this,
                                       &TransitionEditorWindow::saveAs);
    saveAsAction_->setShortcut(QKeySequence::SaveAs);
    toolbar->addSeparator();
    QAction* undo = document_->undoStack()->createUndoAction(this, tr("Undo"));
    QAction* redo = document_->undoStack()->createRedoAction(this, tr("Redo"));
    undo->setShortcut(QKeySequence::Undo);
    redo->setShortcut(QKeySequence::Redo);
    toolbar->addAction(undo);
    toolbar->addAction(redo);
    toolbar->addSeparator();

    snapCombo_ = new QComboBox(toolbar);
    snapCombo_->setObjectName(QStringLiteral("transitionEditorSnap"));
    const std::array<std::pair<const char*, double>, 7> snaps {{
        {"1 Bar", 4.0}, {"1 Beat", 1.0}, {"1/2", 0.5},
        {"1/4", 0.25}, {"1/8", 0.125}, {"1/16", 0.0625}, {"Off", 0.0}}};
    for (const auto& [name, value] : snaps)
        snapCombo_->addItem(tr(name), value);
    snapCombo_->setCurrentIndex(3);
    toolbar->addWidget(new QLabel(tr(" Snap "), toolbar));
    toolbar->addWidget(snapCombo_);
    connect(snapCombo_, &QComboBox::currentIndexChanged, this, [this] {
        timeline_->setSnapBeats(snapCombo_->currentData().toDouble());
    });

    auto* zoomOut = new QPushButton(QStringLiteral("−"), toolbar);
    auto* zoomIn = new QPushButton(QStringLiteral("+"), toolbar);
    zoomOut->setFixedWidth(28);
    zoomIn->setFixedWidth(28);
    toolbar->addWidget(new QLabel(tr("  Zoom "), toolbar));
    toolbar->addWidget(zoomOut);
    toolbar->addWidget(zoomIn);
    connect(zoomOut, &QPushButton::clicked, this, [this] {
        timeline_->setPixelsPerBeat(timeline_->pixelsPerBeat() / 1.25);
    });
    connect(zoomIn, &QPushButton::clicked, this, [this] {
        timeline_->setPixelsPerBeat(timeline_->pixelsPerBeat() * 1.25);
    });

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(5);

    auto* validationRow = new QHBoxLayout;
    validationLabel_ = new QLabel(central);
    validationLabel_->setObjectName(QStringLiteral("transitionEditorValidation"));
    validationLabel_->setWordWrap(true);
    validationRow->addWidget(validationLabel_, 1);
    prepareStemsButton_ = new QPushButton(tr("PREPARE STEMS"), central);
    prepareStemsButton_->setObjectName(
        QStringLiteral("transitionEditorPrepareStems"));
    prepareStemsButton_->hide();
    validationRow->addWidget(prepareStemsButton_);
    root->addLayout(validationRow);
    connect(prepareStemsButton_, &QPushButton::clicked, this,
            &TransitionEditorWindow::prepareRequiredStems);

    auto* mainSplit = new QSplitter(Qt::Horizontal, central);
    timeline_ = new TransitionTimelineView(document_);
    timelineScroll_ = new QScrollArea(mainSplit);
    timelineScroll_->setWidget(timeline_);
    timelineScroll_->setWidgetResizable(false);
    timelineScroll_->setFrameShape(QFrame::NoFrame);
    mainSplit->addWidget(timelineScroll_);

    inspectorTabs_ = new QTabWidget(mainSplit);
    inspectorTabs_->setObjectName(QStringLiteral("transitionEditorInspector"));
    inspectorTabs_->setMinimumWidth(360);
    inspectorTabs_->setMaximumWidth(520);
    mainSplit->addWidget(inspectorTabs_);
    mainSplit->setStretchFactor(0, 4);
    mainSplit->setStretchFactor(1, 1);
    root->addWidget(mainSplit, 1);

    connect(timeline_, &TransitionTimelineView::eventSelected, this,
            &TransitionEditorWindow::selectEvent);
    connect(timeline_, &TransitionTimelineView::playheadChanged, this,
            [this](double beat) {
                if (preview_ && preview_->active) stopPreview();
                playheadLabel_->setText(tr("Beat %1").arg(beat, 0, 'f', 3));
                if (selectedEvent_ < 0) eventBeatSpin_->setValue(beat);
            });

    // Transition metadata and endpoint assumptions.
    auto* details = new QWidget(inspectorTabs_);
    auto* detailsLayout = new QVBoxLayout(details);
    auto* form = new QFormLayout;
    nameEdit_ = new QLineEdit(details);
    nameEdit_->setObjectName(QStringLiteral("transitionEditorName"));
    authorEdit_ = new QLineEdit(details);
    descriptionEdit_ = new QPlainTextEdit(details);
    descriptionEdit_->setMaximumHeight(82);
    masterBpmSpin_ = new QDoubleSpinBox(details);
    masterBpmSpin_->setRange(20.0, 400.0);
    masterBpmSpin_->setDecimals(6);
    endBeatSpin_ = new QDoubleSpinBox(details);
    endBeatSpin_->setRange(0.0, 1000000.0);
    endBeatSpin_->setDecimals(6);
    outgoingAnchorSpin_ = new QDoubleSpinBox(details);
    incomingAnchorSpin_ = new QDoubleSpinBox(details);
    for (QDoubleSpinBox* spin : {outgoingAnchorSpin_, incomingAnchorSpin_}) {
        spin->setRange(-1000000.0, 1000000.0);
        spin->setDecimals(6);
    }
    form->addRow(tr("Name"), nameEdit_);
    form->addRow(tr("Author"), authorEdit_);
    form->addRow(tr("Description"), descriptionEdit_);
    form->addRow(tr("Master BPM"), masterBpmSpin_);
    form->addRow(tr("End beat"), endBeatSpin_);
    form->addRow(tr("Outgoing anchor"), outgoingAnchorSpin_);
    form->addRow(tr("Incoming anchor"), incomingAnchorSpin_);
    detailsLayout->addLayout(form);

    auto* outgoingBox = new QGroupBox(tr("Outgoing endpoint"), details);
    auto* outgoingLayout = new QHBoxLayout(outgoingBox);
    outgoingTrackLabel_ = new QLabel(outgoingBox);
    outgoingTrackLabel_->setWordWrap(true);
    auto* changeOutgoing = new QPushButton(tr("Change…"), outgoingBox);
    outgoingLayout->addWidget(outgoingTrackLabel_, 1);
    outgoingLayout->addWidget(changeOutgoing);
    detailsLayout->addWidget(outgoingBox);
    auto* incomingBox = new QGroupBox(tr("Incoming endpoint"), details);
    auto* incomingLayout = new QHBoxLayout(incomingBox);
    incomingTrackLabel_ = new QLabel(incomingBox);
    incomingTrackLabel_->setWordWrap(true);
    auto* changeIncoming = new QPushButton(tr("Change…"), incomingBox);
    incomingLayout->addWidget(incomingTrackLabel_, 1);
    incomingLayout->addWidget(changeIncoming);
    detailsLayout->addWidget(incomingBox);
    auto* applyDetails = new QPushButton(tr("Apply transition details"), details);
    applyDetails->setObjectName(QStringLiteral("transitionEditorApplyDetails"));
    detailsLayout->addWidget(applyDetails);
    detailsLayout->addStretch();
    inspectorTabs_->addTab(details, tr("Transition"));

    connect(changeOutgoing, &QPushButton::clicked, this,
            [this] { setEndpoint(true); });
    connect(changeIncoming, &QPushButton::clicked, this,
            [this] { setEndpoint(false); });
    connect(applyDetails, &QPushButton::clicked, this, [this] {
        const QString name = nameEdit_->text();
        const QString author = authorEdit_->text();
        const QString description = descriptionEdit_->toPlainText();
        const double bpm = masterBpmSpin_->value();
        const double end = endBeatSpin_->value();
        const double fromAnchor = outgoingAnchorSpin_->value();
        const double toAnchor = incomingAnchorSpin_->value();
        document_->mutate(tr("Edit transition details"),
                          [=](GvtFile& file) {
            file.name = name.trimmed();
            file.author = author.trimmed();
            file.description = description;
            file.masterBpm = bpm;
            file.endBeat = std::max(end, latestBeat(file));
            file.anchorFromBeat = fromAnchor;
            file.anchorToBeat = toAnchor;
            if (!file.requirements.contains(QStringLiteral("timeline-end.v1")))
                file.requirements.append(QStringLiteral("timeline-end.v1"));
        });
    });

    // Exact event table and selected-event inspector.
    auto* eventsPage = new QWidget(inspectorTabs_);
    auto* eventsLayout = new QVBoxLayout(eventsPage);
    eventTable_ = new QTableWidget(eventsPage);
    eventTable_->setObjectName(QStringLiteral("transitionEditorEvents"));
    eventTable_->setColumnCount(4);
    eventTable_->setHorizontalHeaderLabels(
        {tr("Beat"), tr("Target"), tr("Control"), tr("Value")});
    eventTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    eventTable_->horizontalHeader()->setStretchLastSection(true);
    eventTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    eventTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    eventsLayout->addWidget(eventTable_, 1);
    connect(eventTable_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { selectEvent(row); });

    auto* eventForm = new QFormLayout;
    roleCombo_ = new QComboBox(eventsPage);
    roleCombo_->addItem(tr("Outgoing"), static_cast<int>(Role::FromDeck));
    roleCombo_->addItem(tr("Incoming"), static_cast<int>(Role::ToDeck));
    roleCombo_->addItem(tr("Mixer"), static_cast<int>(Role::Mixer));
    controlCombo_ = new QComboBox(eventsPage);
    for (int value = 0; value < static_cast<int>(ControlId::Count); ++value) {
        const ControlId control = static_cast<ControlId>(value);
        if (editableTimelineControl(control))
            controlCombo_->addItem(controlText(control), value);
    }
    eventBeatSpin_ = new QDoubleSpinBox(eventsPage);
    eventBeatSpin_->setRange(-1000000.0, 1000000.0);
    eventBeatSpin_->setDecimals(6);
    eventValueSpin_ = new QDoubleSpinBox(eventsPage);
    eventValueSpin_->setRange(-1024.0, 1024.0);
    eventValueSpin_->setDecimals(6);
    curveCombo_ = new QComboBox(eventsPage);
    curveCombo_->addItem(tr("Step"), static_cast<int>(Curve::Step));
    curveCombo_->addItem(tr("Linear"), static_cast<int>(Curve::Linear));
    curveCombo_->addItem(tr("S-Curve"), static_cast<int>(Curve::SCurve));
    gestureControlCombo_ = new QComboBox(eventsPage);
    gestureControlCombo_->addItem(tr("None"), static_cast<int>(ControlId::Count));
    for (int value = 0; value < static_cast<int>(ControlId::Count); ++value) {
        const ControlId control = static_cast<ControlId>(value);
        gestureControlCombo_->addItem(controlText(control), value);
    }
    gesturePadModeCombo_ = new QComboBox(eventsPage);
    gesturePadModeCombo_->addItem(tr("None"), -1);
    for (int value = 0; value < static_cast<int>(PerformancePadMode::Count);
         ++value) {
        const auto mode = static_cast<PerformancePadMode>(value);
        gesturePadModeCombo_->addItem(
            QString::fromLatin1(performancePadModeLabel(mode)), value);
    }
    eventReferenceEdit_ = new QLineEdit(eventsPage);
    eventReferenceEdit_->setPlaceholderText(tr("Optional semantic cue/loop ID"));
    eventForm->addRow(tr("Target"), roleCombo_);
    eventForm->addRow(tr("Control"), controlCombo_);
    eventForm->addRow(tr("Beat"), eventBeatSpin_);
    eventForm->addRow(tr("Value"), eventValueSpin_);
    eventForm->addRow(tr("Curve"), curveCombo_);
    eventForm->addRow(tr("Cue/loop ID"), eventReferenceEdit_);
    eventForm->addRow(tr("Input gesture"), gestureControlCombo_);
    eventForm->addRow(tr("Pad mode"), gesturePadModeCombo_);
    eventsLayout->addLayout(eventForm);
    auto* eventButtons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), eventsPage);
    auto* duplicate = new QPushButton(tr("Duplicate"), eventsPage);
    deleteEventButton_ = new QPushButton(tr("Delete"), eventsPage);
    applyEventButton_ = new QPushButton(tr("Apply"), eventsPage);
    eventButtons->addWidget(add);
    eventButtons->addWidget(duplicate);
    eventButtons->addWidget(deleteEventButton_);
    eventButtons->addStretch();
    eventButtons->addWidget(applyEventButton_);
    eventsLayout->addLayout(eventButtons);
    inspectorTabs_->addTab(eventsPage, tr("Events"));
    connect(add, &QPushButton::clicked, this, &TransitionEditorWindow::addEvent);
    connect(duplicate, &QPushButton::clicked, this,
            &TransitionEditorWindow::duplicateSelectedEvent);
    connect(deleteEventButton_, &QPushButton::clicked, this,
            &TransitionEditorWindow::deleteSelectedEvent);
    connect(applyEventButton_, &QPushButton::clicked, this,
            &TransitionEditorWindow::applyEventInspector);
    connect(controlCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (refreshing_ || selectedEvent_ >= 0) return;
        const ControlId control = static_cast<ControlId>(
            controlCombo_->currentData().toInt());
        const auto [minimum, maximum] = controlEditRange(control);
        eventValueSpin_->setRange(minimum, maximum);
        eventValueSpin_->setValue(defaultControlValue(control));
        curveCombo_->setCurrentIndex(
            curveCombo_->findData(static_cast<int>(Curve::Step)));
        curveCombo_->setEnabled(!controlIsTrigger(control));
    });
    connect(roleCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (refreshing_ || selectedEvent_ >= 0) return;
        const Role role = static_cast<Role>(roleCombo_->currentData().toInt());
        const ControlId control = static_cast<ControlId>(
            controlCombo_->currentData().toInt());
        if (role == Role::Mixer && control != ControlId::Crossfader)
            controlCombo_->setCurrentIndex(
                controlCombo_->findData(static_cast<int>(ControlId::Crossfader)));
    });

    // Semantic cues, loops and timeline labels.
    auto* performancePage = new QWidget(inspectorTabs_);
    auto* performanceLayout = new QVBoxLayout(performancePage);
    performanceTable_ = new QTableWidget(performancePage);
    performanceTable_->setObjectName(QStringLiteral("transitionEditorPerformanceDefinitions"));
    performanceTable_->setColumnCount(11);
    performanceTable_->setHorizontalHeaderLabels(
        {tr("Kind"), tr("Endpoint"), tr("ID"), tr("Start"), tr("End"),
         tr("Label"), tr("Purpose"), tr("Color"), tr("Pairing"),
         tr("Pad"), tr("Key")});
    performanceTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    performanceTable_->horizontalHeader()->setStretchLastSection(true);
    performanceLayout->addWidget(performanceTable_, 1);
    auto* definitionButtons = new QHBoxLayout;
    auto* addCueButton = new QPushButton(tr("+ Cue"), performancePage);
    auto* addLoopButton = new QPushButton(tr("+ Loop"), performancePage);
    auto* addLabelButton = new QPushButton(tr("+ Label"), performancePage);
    auto* deleteDefinition = new QPushButton(tr("Delete"), performancePage);
    auto* applyDefinitions = new QPushButton(tr("Apply table"), performancePage);
    definitionButtons->addWidget(addCueButton);
    definitionButtons->addWidget(addLoopButton);
    definitionButtons->addWidget(addLabelButton);
    definitionButtons->addWidget(deleteDefinition);
    definitionButtons->addStretch();
    definitionButtons->addWidget(applyDefinitions);
    performanceLayout->addLayout(definitionButtons);
    inspectorTabs_->addTab(performancePage, tr("Cues / Loops"));
    connect(addCueButton, &QPushButton::clicked, this,
            &TransitionEditorWindow::addCue);
    connect(addLoopButton, &QPushButton::clicked, this,
            &TransitionEditorWindow::addLoop);
    connect(addLabelButton, &QPushButton::clicked, this, [this] {
        GvtCue cue;
        cue.beat = timeline_->playheadBeat();
        cue.label = tr("New label");
        document_->mutate(tr("Add transition label"), [cue](GvtFile& file) {
            file.cues.push_back(cue);
            std::stable_sort(file.cues.begin(), file.cues.end(),
                             [](const GvtCue& a, const GvtCue& b) {
                                 return a.beat < b.beat;
                             });
        });
    });
    connect(deleteDefinition, &QPushButton::clicked, this,
            &TransitionEditorWindow::deletePerformanceDefinition);
    connect(applyDefinitions, &QPushButton::clicked, this,
            &TransitionEditorWindow::applyPerformanceDefinition);

    // Initial state matrix.
    auto* initialPage = new QWidget(inspectorTabs_);
    auto* initialLayout = new QVBoxLayout(initialPage);
    auto* initialHelp = new QLabel(
        tr("Double-click values to edit the state restored at transition beat zero."),
        initialPage);
    initialHelp->setWordWrap(true);
    initialLayout->addWidget(initialHelp);
    initialTable_ = new QTableWidget(initialPage);
    initialTable_->setObjectName(QStringLiteral("transitionEditorInitialState"));
    initialTable_->setColumnCount(3);
    initialTable_->setHorizontalHeaderLabels(
        {tr("Control"), tr("Outgoing"), tr("Incoming")});
    initialTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    initialLayout->addWidget(initialTable_, 1);
    inspectorTabs_->addTab(initialPage, tr("Initial State"));
    connect(initialTable_, &QTableWidget::cellChanged, this,
            &TransitionEditorWindow::applyInitialCell);

    // Raw source remains available as an explicit advanced operation.
    auto* yamlPage = new QWidget(inspectorTabs_);
    auto* yamlLayout = new QVBoxLayout(yamlPage);
    auto* yamlHelp = new QLabel(
        tr("Advanced source editing. Apply parses the complete safe YAML document "
           "and records the replacement as one undo step."), yamlPage);
    yamlHelp->setWordWrap(true);
    yamlLayout->addWidget(yamlHelp);
    yamlEdit_ = new QPlainTextEdit(yamlPage);
    yamlEdit_->setObjectName(QStringLiteral("transitionEditorYaml"));
    yamlEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    yamlEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    yamlLayout->addWidget(yamlEdit_, 1);
    auto* applyYamlButton = new QPushButton(tr("Apply source"), yamlPage);
    yamlLayout->addWidget(applyYamlButton);
    connect(applyYamlButton, &QPushButton::clicked, this,
            &TransitionEditorWindow::applyYaml);
    inspectorTabs_->addTab(yamlPage, tr("YAML"));
    connect(inspectorTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (inspectorTabs_->widget(index) == yamlEdit_->parentWidget())
            updateYamlFromModel();
    });

    // Mouse/keyboard performance strip. Sliders create points at the playhead
    // and become realtime write controls while preview is running.
    auto* transport = new QWidget(central);
    transport->setObjectName(QStringLiteral("transitionEditorTransport"));
    transport->setProperty("panel", true);
    auto* transportLayout = new QVBoxLayout(transport);
    transportLayout->setContentsMargins(6, 4, 6, 4);
    auto* transportTop = new QHBoxLayout;
    playButton_ = new QPushButton(tr("▶ PREVIEW FROM CURSOR"), transport);
    playButton_->setObjectName(QStringLiteral("transitionEditorPlay"));
    stopButton_ = new QPushButton(tr("■ STOP"), transport);
    writeAutomationCheck_ = new QCheckBox(tr("WRITE AUTOMATION"), transport);
    playheadLabel_ = new QLabel(tr("Beat 0.000"), transport);
    transportTop->addWidget(playButton_);
    transportTop->addWidget(stopButton_);
    transportTop->addWidget(writeAutomationCheck_);
    transportTop->addStretch();
    transportTop->addWidget(playheadLabel_);
    transportLayout->addLayout(transportTop);
    connect(playButton_, &QPushButton::clicked, this,
            &TransitionEditorWindow::startOrPausePreview);
    connect(stopButton_, &QPushButton::clicked, this,
            &TransitionEditorWindow::stopPreview);

    auto* controls = new QHBoxLayout;
    const auto addTransportButton = [&](const QString& label, Role role,
                                        ControlId control, double value = 1.0) {
        auto* button = new QPushButton(label, transport);
        button->setMaximumWidth(72);
        connect(button, &QPushButton::clicked, this,
                [this, role, control, value] {
                    recordControlValue(role, control, value);
                });
        controls->addWidget(button);
    };
    addTransportButton(tr("OUT PLAY"), Role::FromDeck, ControlId::Play);
    addTransportButton(tr("OUT STOP"), Role::FromDeck, ControlId::Stop);
    addTransportButton(tr("IN PLAY"), Role::ToDeck, ControlId::Play);
    addTransportButton(tr("IN STOP"), Role::ToDeck, ControlId::Stop);

    const auto addControlSlider = [&](const QString& label, Role role,
                                      ControlId control) {
        auto* host = new QWidget(transport);
        auto* layout = new QVBoxLayout(host);
        layout->setContentsMargins(2, 0, 2, 0);
        auto* title = new QLabel(label, host);
        title->setAlignment(Qt::AlignCenter);
        auto* slider = new QSlider(Qt::Horizontal, host);
        slider->setRange(0, 1000);
        const auto [minimum, maximum] = controlEditRange(control);
        slider->setValue(static_cast<int>(
            (defaultControlValue(control) - minimum) /
            (maximum - minimum) * 1000.0));
        layout->addWidget(title);
        layout->addWidget(slider);
        host->setMinimumWidth(78);
        controls->addWidget(host, 1);
        const auto valueFor = [slider, minimum, maximum] {
            return minimum + slider->value() / 1000.0 * (maximum - minimum);
        };
        connect(slider, &QSlider::valueChanged, this,
                [this, role, control, valueFor](int) {
                    if (writeAutomationCheck_->isChecked() && preview_->active)
                        recordControlValue(role, control, valueFor());
                });
        connect(slider, &QSlider::sliderReleased, this,
                [this, role, control, valueFor] {
                    if (!writeAutomationCheck_->isChecked() || !preview_->active)
                        recordControlValue(role, control, valueFor());
                });
    };
    addControlSlider(tr("OUT FADER"), Role::FromDeck, ControlId::Fader);
    addControlSlider(tr("OUT LOW"), Role::FromDeck, ControlId::EqLow);
    addControlSlider(tr("OUT FILTER"), Role::FromDeck, ControlId::Filter);
    addControlSlider(tr("CROSSFADER"), Role::Mixer, ControlId::Crossfader);
    addControlSlider(tr("IN FILTER"), Role::ToDeck, ControlId::Filter);
    addControlSlider(tr("IN LOW"), Role::ToDeck, ControlId::EqLow);
    addControlSlider(tr("IN FADER"), Role::ToDeck, ControlId::Fader);
    const auto addAssignableSlider = [&](const QString& label, Role role) {
        auto* host = new QWidget(transport);
        auto* layout = new QVBoxLayout(host);
        layout->setContentsMargins(2, 0, 2, 0);
        auto* title = new QLabel(label, host);
        title->setAlignment(Qt::AlignCenter);
        auto* selector = new QComboBox(host);
        for (const ControlId control : {
                 ControlId::Tempo, ControlId::EqHigh, ControlId::EqMid,
                 ControlId::FxWet, ControlId::FxBeats,
                 ControlId::StemVocals, ControlId::StemMelody,
                 ControlId::StemBass, ControlId::StemDrums})
            selector->addItem(controlText(control), static_cast<int>(control));
        auto* slider = new QSlider(Qt::Horizontal, host);
        slider->setRange(0, 1000);
        const auto setDefault = [selector, slider] {
            const ControlId control = static_cast<ControlId>(
                selector->currentData().toInt());
            const auto [minimum, maximum] = controlRange(control);
            slider->setValue(static_cast<int>(std::clamp(
                (defaultControlValue(control) - minimum) /
                    (maximum - minimum), 0.0, 1.0) * 1000.0));
        };
        const auto selectedValue = [selector, slider] {
            const ControlId control = static_cast<ControlId>(
                selector->currentData().toInt());
            const auto [minimum, maximum] = controlRange(control);
            return std::make_pair(
                control,
                minimum + slider->value() / 1000.0 * (maximum - minimum));
        };
        layout->addWidget(title);
        layout->addWidget(selector);
        layout->addWidget(slider);
        host->setMinimumWidth(105);
        controls->addWidget(host, 1);
        setDefault();
        connect(selector, &QComboBox::currentIndexChanged, this, setDefault);
        connect(slider, &QSlider::valueChanged, this,
                [this, role, selectedValue](int) {
            if (writeAutomationCheck_->isChecked() && preview_->active) {
                const auto [control, value] = selectedValue();
                recordControlValue(role, control, value);
            }
        });
        connect(slider, &QSlider::sliderReleased, this,
                [this, role, selectedValue] {
            if (!writeAutomationCheck_->isChecked() || !preview_->active) {
                const auto [control, value] = selectedValue();
                recordControlValue(role, control, value);
            }
        });
    };
    addAssignableSlider(tr("OUT OTHER"), Role::FromDeck);
    addAssignableSlider(tr("IN OTHER"), Role::ToDeck);
    transportLayout->addLayout(controls);
    root->addWidget(transport);
    setCentralWidget(central);

    statusBar()->showMessage(
        tr("Double-click an automation lane to add a point; Option-drag bypasses snapping."));
}

GvtTrackRef TransitionEditorWindow::trackReference(const TrackData& track) const
{
    GvtTrackRef ref;
    ref.title = track.title;
    ref.artist = track.artist;
    if (!track.artist.isEmpty()) ref.artists.append(track.artist);
    ref.bpm = track.bpm;
    ref.durationSec = track.audibleDurationSec > 0.0
                          ? track.audibleDurationSec : track.durationSec;
    ref.durationBeats = ref.bpm > 0.0 ? ref.durationSec * ref.bpm / 60.0 : 0.0;
    ref.referenceDownbeatSec = track.firstBeatSec;
    ref.isrc = track.isrc;
    ref.musicBrainzRecording = track.musicBrainzRecording;
    ref.fingerprint = track.fingerprint;
    if (!track.structureFingerprint.isEmpty())
        ref.fingerprints.push_back(
            {QStringLiteral("gravitino-structure-2"),
             track.structureFingerprint, {}});
    if (!track.fingerprint.isEmpty())
        ref.fingerprints.push_back(
            {QStringLiteral("gvfp1"), track.fingerprint, {}});
    return ref;
}

TrackDataPtr TransitionEditorWindow::resolveTrack(const GvtFile& file,
                                                  bool outgoing) const
{
    if (!library_) return {};
    TrackDataPtr best;
    int bestQuality = static_cast<int>(MatchQuality::None);
    for (int row = 0; row < library_->trackCount(); ++row) {
        const TrackDataPtr track = library_->trackAt(row);
        if (!track) continue;
        const bool matches = store_
            ? store_->matchesEndpoint(file, outgoing, *track)
            : isReliableTrackMatch(matchTrack(outgoing ? file.from : file.to,
                                              *track));
        if (!matches) continue;
        const int quality = static_cast<int>(
            matchTrack(outgoing ? file.from : file.to, *track));
        if (!best || quality > bestQuality) {
            best = track;
            bestQuality = quality;
        }
    }
    return best;
}

bool TransitionEditorWindow::chooseEndpoints(TrackDataPtr& outgoing,
                                             TrackDataPtr& incoming)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New transition"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(
        tr("Choose two analyzed local tracks. Their canonical beat grids and "
           "fingerprints seed a portable transition edge."), &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* form = new QFormLayout;
    auto* outgoingCombo = new QComboBox(&dialog);
    auto* incomingCombo = new QComboBox(&dialog);
    for (QComboBox* combo : {outgoingCombo, incomingCombo}) {
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        combo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
        combo->completer()->setFilterMode(Qt::MatchContains);
    }
    for (int row = 0; library_ && row < library_->trackCount(); ++row) {
        const TrackDataPtr track = library_->trackAt(row);
        if (!track) continue;
        const QString text = track->artist.isEmpty()
            ? track->title
            : QStringLiteral("%1 — %2").arg(track->artist, track->title);
        outgoingCombo->addItem(text, row);
        incomingCombo->addItem(text, row);
    }
    if (outgoingCombo->count() < 2) {
        QMessageBox::information(
            this, tr("Tracks are still loading"),
            tr("At least two analyzed library tracks are needed to create a transition."));
        return false;
    }
    const auto selectLoaded = [this](QComboBox* combo, int deck) {
        if (!liveEngine_) return false;
        const TrackDataPtr loaded = liveEngine_->deck(deck).track();
        if (!loaded) return false;
        for (int index = 0; index < combo->count(); ++index) {
            const TrackDataPtr candidate = library_->trackAt(
                combo->itemData(index).toInt());
            if (candidate && candidate->filePath == loaded->filePath) {
                combo->setCurrentIndex(index);
                return true;
            }
        }
        return false;
    };
    const bool selectedOutgoing = selectLoaded(outgoingCombo, 0);
    const bool selectedIncoming = selectLoaded(incomingCombo, 1);
    if (!selectedOutgoing) outgoingCombo->setCurrentIndex(0);
    if (!selectedIncoming) incomingCombo->setCurrentIndex(1);
    if (outgoingCombo->currentData() == incomingCombo->currentData())
        incomingCombo->setCurrentIndex(outgoingCombo->currentIndex() == 0 ? 1 : 0);
    form->addRow(tr("Outgoing"), outgoingCombo);
    form->addRow(tr("Incoming"), incomingCombo);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return false;
    const int outgoingRow = outgoingCombo->currentData().toInt();
    const int incomingRow = incomingCombo->currentData().toInt();
    if (outgoingRow == incomingRow) {
        QMessageBox::warning(this, tr("Choose two tracks"),
                             tr("Outgoing and incoming must be different tracks."));
        return false;
    }
    outgoing = library_->trackAt(outgoingRow);
    incoming = library_->trackAt(incomingRow);
    return outgoing && incoming;
}

bool TransitionEditorWindow::createTransition()
{
    if (isVisible() && (document_->isDirty() || isNew_) &&
        !ensureCanDiscard())
        return false;
    if (maybeRecoverUnsavedDraft()) return true;
    TrackDataPtr outgoing;
    TrackDataPtr incoming;
    if (!chooseEndpoints(outgoing, incoming)) return false;
    GvtFile file;
    file.version = 1;
    file.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file.sourceFormat = TransitionSourceFormat::Unsaved;
    file.created = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    file.name = tr("%1 → %2").arg(outgoing->title, incoming->title);
    file.requirements = {QStringLiteral("timeline.v1"),
                         QStringLiteral("temporary-cues.v1"),
                         QStringLiteral("temporary-loops.v1"),
                         QStringLiteral("timeline-end.v1")};
    file.from = trackReference(*outgoing);
    file.to = trackReference(*incoming);
    file.masterBpm = outgoing->bpm;
    file.anchorFromBeat = 0.0;
    file.anchorToBeat = 0.0;
    file.endBeat = 32.0;
    file.initialComplete = true;
    file.initialMixerCaptured = true;
    file.initialCrossfader = 0.0;
    file.initialFrom = defaultInitial(true, 1.0);
    const double incomingRatio = incoming->bpm > 0.0
        ? file.masterBpm / incoming->bpm : 1.0;
    file.initialTo = defaultInitial(false, incomingRatio);

    // A basic crossfade template is useful but remains opt-in so a blank
    // document never silently claims an artistic decision.
    if (QMessageBox::question(
            this, tr("Start with a basic blend?"),
            tr("Add IN PLAY at beat 0 and a 32-beat S-curve crossfade?\n\n"
               "Choose No for a completely blank timeline."),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes) {
        file.events.push_back(
            {0.0, Role::ToDeck, ControlId::Play, 1.0, Curve::Step});
        file.events.push_back(
            {0.0, Role::Mixer, ControlId::Crossfader, 0.0, Curve::Step});
        file.events.push_back(
            {32.0, Role::Mixer, ControlId::Crossfader, 1.0, Curve::SCurve});
    }
    outgoing_ = outgoing;
    incoming_ = incoming;
    outgoingStems_.reset();
    incomingStems_.reset();
    setWorkingFile(file, true);
    return true;
}

void TransitionEditorWindow::openTransition(const GvtFile& original)
{
    if (isVisible() && (document_->isDirty() || isNew_) &&
        !ensureCanDiscard())
        return;
    GvtFile file = original;
    sourcePath_ = original.filePath;
    sourceHash_ = fileHash(sourcePath_);
    const bool restoredDraft = maybeResolveDraft(file);
    const bool draftRequiresCopy = restoredDraft &&
        (file.id != original.id ||
         !sameEndpointProfile(file.from, original.from) ||
         !sameEndpointProfile(file.to, original.to));
    outgoing_ = resolveTrack(file, true);
    incoming_ = resolveTrack(file, false);
    outgoingStems_.reset();
    incomingStems_.reset();
    // Reset to the saved document first so restoring a draft is a real undoable
    // edit. Resetting directly to `file` would create a no-op undo command and
    // make Undo incapable of returning to the on-disk version.
    setWorkingFile(original, false);
    originalName_ = original.name;
    requiresSaveAs_ = draftRequiresCopy;
    if (restoredDraft) {
        document_->apply(file, tr("Restore autosaved draft"));
        timeline_->setTracks(outgoing_, incoming_);
    }
}

void TransitionEditorWindow::setWorkingFile(const GvtFile& file, bool isNew)
{
    stopPreview();
    isNew_ = isNew;
    requiresSaveAs_ = false;
    if (isNew) {
        sourcePath_.clear();
        sourceHash_.clear();
    }
    selectedEvent_ = -1;
    originalName_ = file.name;
    document_->reset(file);
    timeline_->setTracks(outgoing_, incoming_);
    timeline_->setPlayheadBeat(0.0);
    refreshUi();
    show();
    raise();
    activateWindow();
    if (isNew_) scheduleDraftSave();
}

void TransitionEditorWindow::refreshUi()
{
    if (refreshing_) return;
    refreshing_ = true;
    const GvtFile& file = document_->file();
    nameEdit_->setText(file.name);
    authorEdit_->setText(file.author);
    descriptionEdit_->setPlainText(file.description);
    masterBpmSpin_->setValue(file.masterBpm > 0.0 ? file.masterBpm : 120.0);
    endBeatSpin_->setValue(document_->effectiveEndBeat());
    outgoingAnchorSpin_->setValue(file.anchorFromBeat);
    incomingAnchorSpin_->setValue(file.anchorToBeat);
    const auto endpointText = [](const GvtTrackRef& ref,
                                 const TrackDataPtr& resolved) {
        const QString identity = ref.artist.isEmpty()
            ? ref.title : QStringLiteral("%1 — %2").arg(ref.artist, ref.title);
        return resolved
            ? QObject::tr("%1\n%2 BPM • %3")
                  .arg(identity)
                  .arg(resolved->bpm, 0, 'f', 3)
                  .arg(QFileInfo(resolved->filePath).fileName())
            : QObject::tr("%1\nAudio asset unresolved").arg(identity);
    };
    outgoingTrackLabel_->setText(endpointText(file.from, outgoing_));
    incomingTrackLabel_->setText(endpointText(file.to, incoming_));
    rebuildEventTable();
    rebuildPerformanceDefinitions();
    rebuildInitialStateTable();
    updateEventInspector();
    updateValidation();
    timeline_->setSelectedEvent(selectedEvent_);
    if (inspectorTabs_->currentWidget() == yamlEdit_->parentWidget())
        updateYamlFromModel();
    saveAction_->setEnabled(isNew_ || document_->isDirty());
    setWindowTitle(tr("%1%2 — Transition Editor")
                       .arg(document_->isDirty() || isNew_ ? QStringLiteral("● ")
                                                           : QString(),
                            file.name.isEmpty() ? tr("Untitled") : file.name));
    refreshing_ = false;
}

void TransitionEditorWindow::updateValidation()
{
    QStringList errors = document_->validationErrors();
    if (!outgoing_ || !incoming_)
        errors.append(tr("Preview needs compatible local audio for both endpoints"));
    const bool needsStems = endpointNeedsStems(Role::FromDeck) ||
                            endpointNeedsStems(Role::ToDeck);
    prepareStemsButton_->setVisible(needsStems && !requiredStemsReady());
    prepareStemsButton_->setEnabled(stemSeparator_ && outgoing_ && incoming_);
    prepareStemsButton_->setText(stemSeparator_ ? tr("PREPARE STEMS")
                                                : tr("STEMS UNAVAILABLE"));
    if (needsStems && !requiredStemsReady())
        errors.append(stemSeparator_
            ? tr("Prepare the required stems before preview")
            : tr("Stem automation cannot be previewed because the separator is unavailable"));
    if (errors.isEmpty()) {
        validationLabel_->setText(tr("✓ Transition is valid and preview-ready"));
        validationLabel_->setStyleSheet(
            QStringLiteral("background:#173c31; color:#8ee6be; padding:5px;"));
    } else {
        validationLabel_->setText(tr("⚠ %1").arg(errors.join(QStringLiteral("  •  "))));
        validationLabel_->setStyleSheet(
            QStringLiteral("background:#4a3320; color:#ffd090; padding:5px;"));
    }
}

bool TransitionEditorWindow::endpointNeedsStems(Role role) const
{
    if (role != Role::FromDeck && role != Role::ToDeck) return false;
    const GvtFile& file = document_->file();
    if (file.initialComplete &&
        initialUsesStems(role == Role::FromDeck ? file.initialFrom
                                               : file.initialTo))
        return true;
    return std::any_of(file.events.begin(), file.events.end(),
                       [role](const GvtEvent& event) {
        return event.role == role && isStemControl(event.control);
    });
}

bool TransitionEditorWindow::requiredStemsReady() const
{
    const bool outgoingReady = !endpointNeedsStems(Role::FromDeck) ||
                               static_cast<bool>(outgoingStems_);
    const bool incomingReady = !endpointNeedsStems(Role::ToDeck) ||
                               static_cast<bool>(incomingStems_);
    return outgoingReady && incomingReady;
}

void TransitionEditorWindow::prepareRequiredStems()
{
    if (!stemSeparator_) {
        QMessageBox::information(this, tr("Stem separation unavailable"),
            tr("This build cannot prepare the separated audio required by the transition."));
        return;
    }
    bool requested = false;
    if (endpointNeedsStems(Role::FromDeck) && outgoing_ && !outgoingStems_) {
        stemSeparator_->requestStems(outgoing_);
        requested = true;
    }
    if (endpointNeedsStems(Role::ToDeck) && incoming_ && !incomingStems_) {
        stemSeparator_->requestStems(incoming_);
        requested = true;
    }
    if (requested) {
        prepareStemsButton_->setEnabled(false);
        prepareStemsButton_->setText(tr("STEMS: QUEUED…"));
        statusBar()->showMessage(
            tr("Preparing separated audio; preview stays blocked until it is ready."));
    }
}

void TransitionEditorWindow::rebuildEventTable()
{
    eventTable_->setRowCount(static_cast<int>(document_->file().events.size()));
    for (int row = 0; row < eventTable_->rowCount(); ++row) {
        const GvtEvent& event = document_->file().events[static_cast<std::size_t>(row)];
        const QString reference = !event.cueId.isEmpty() ? event.cueId : event.loopId;
        const QString control = controlText(event.control) +
            (reference.isEmpty() ? QString() : QStringLiteral(" • ") + reference);
        const QStringList values {QString::number(event.beat, 'f', 3),
                                  roleText(event.role), control,
                                  QString::number(event.value, 'f', 3)};
        for (int column = 0; column < 4; ++column)
            eventTable_->setItem(row, column,
                                 new QTableWidgetItem(values.at(column)));
    }
    if (selectedEvent_ >= eventTable_->rowCount()) selectedEvent_ = -1;
    if (selectedEvent_ >= 0)
        eventTable_->selectRow(selectedEvent_);
}

void TransitionEditorWindow::selectEvent(int index)
{
    if (index < 0 || index >= static_cast<int>(document_->file().events.size()))
        index = -1;
    selectedEvent_ = index;
    timeline_->setSelectedEvent(index);
    if (index >= 0 && eventTable_->currentRow() != index)
        eventTable_->selectRow(index);
    updateEventInspector();
}

void TransitionEditorWindow::updateEventInspector()
{
    const bool selected = selectedEvent_ >= 0 &&
        selectedEvent_ < static_cast<int>(document_->file().events.size());
    roleCombo_->setEnabled(true);
    controlCombo_->setEnabled(true);
    eventBeatSpin_->setEnabled(true);
    eventValueSpin_->setEnabled(true);
    eventReferenceEdit_->setEnabled(true);
    gestureControlCombo_->setEnabled(true);
    gesturePadModeCombo_->setEnabled(true);
    applyEventButton_->setEnabled(selected);
    deleteEventButton_->setEnabled(selected);
    if (!selected) {
        eventBeatSpin_->setValue(timeline_->playheadBeat());
        const ControlId control = static_cast<ControlId>(
            controlCombo_->currentData().toInt());
        const auto [minimum, maximum] = controlEditRange(control);
        eventValueSpin_->setRange(minimum, maximum);
        eventValueSpin_->setValue(defaultControlValue(control));
        curveCombo_->setCurrentIndex(
            curveCombo_->findData(static_cast<int>(Curve::Step)));
        curveCombo_->setEnabled(!controlIsTrigger(control));
        eventReferenceEdit_->clear();
        gestureControlCombo_->setCurrentIndex(0);
        gesturePadModeCombo_->setCurrentIndex(0);
        return;
    }
    const GvtEvent& event =
        document_->file().events[static_cast<std::size_t>(selectedEvent_)];
    roleCombo_->setCurrentIndex(roleCombo_->findData(static_cast<int>(event.role)));
    controlCombo_->setCurrentIndex(
        controlCombo_->findData(static_cast<int>(event.control)));
    eventBeatSpin_->setValue(event.beat);
    const auto [minimum, maximum] = controlEditRange(event.control);
    eventValueSpin_->setRange(minimum, maximum);
    eventValueSpin_->setValue(event.value);
    curveCombo_->setCurrentIndex(curveCombo_->findData(static_cast<int>(event.curve)));
    eventReferenceEdit_->setText(!event.cueId.isEmpty() ? event.cueId
                                                         : event.loopId);
    gestureControlCombo_->setCurrentIndex(gestureControlCombo_->findData(
        static_cast<int>(event.gestureControl)));
    gesturePadModeCombo_->setCurrentIndex(gesturePadModeCombo_->findData(
        event.gesturePadMode));
    curveCombo_->setEnabled(!controlIsTrigger(event.control));
}

void TransitionEditorWindow::addEvent()
{
    GvtEvent event;
    event.role = static_cast<Role>(roleCombo_->currentData().toInt());
    event.control = static_cast<ControlId>(controlCombo_->currentData().toInt());
    if ((event.role == Role::Mixer) !=
        (event.control == ControlId::Crossfader)) {
        QMessageBox::warning(this, tr("Invalid target"),
            tr("Mixer actions can only control the crossfader; deck actions need an endpoint."));
        return;
    }
    event.beat = eventBeatSpin_->value();
    event.value = eventValueSpin_->value();
    event.curve = controlIsTrigger(event.control)
        ? Curve::Step
        : static_cast<Curve>(curveCombo_->currentData().toInt());
    event.gestureControl = static_cast<ControlId>(
        gestureControlCombo_->currentData().toInt());
    event.gesturePadMode = gesturePadModeCombo_->currentData().toInt();
    const QString reference = eventReferenceEdit_->text().trimmed();
    for (const TransitionHotCue& cue : document_->file().transitionCues) {
        if (cue.id == reference && cue.role == event.role) {
            event.cueId = reference;
            event.control = ControlId::TransitionCue1;
        }
    }
    for (const TransitionSavedLoop& loop : document_->file().transitionLoops) {
        if (loop.id == reference && loop.role == event.role) {
            event.loopId = reference;
            event.control = ControlId::TransitionCue1;
        }
    }
    if (!reference.isEmpty() && event.cueId.isEmpty() && event.loopId.isEmpty()) {
        QMessageBox::warning(this, tr("Unknown semantic ID"),
            tr("No cue or loop with ID “%1” belongs to this endpoint.")
                .arg(reference));
        return;
    }
    const bool requiresReference =
        (event.control >= ControlId::HotCue1 &&
         event.control <= ControlId::HotCue8) ||
        (event.control >= ControlId::SavedLoop1 &&
         event.control <= ControlId::SavedLoop8) ||
        (event.control >= ControlId::TransitionCue1 &&
         event.control <= ControlId::TransitionCue8);
    if (requiresReference && reference.isEmpty()) {
        QMessageBox::information(this, tr("Choose a transition cue or loop"),
            tr("Create a transition-owned cue or loop first, then enter its semantic ID for this action."));
        return;
    }
    document_->mutate(tr("Add timeline event"), [event](GvtFile& file) {
        file.events.push_back(event);
        std::stable_sort(file.events.begin(), file.events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    });
    int nearest = 0;
    double distance = std::numeric_limits<double>::max();
    for (int index = 0; index < static_cast<int>(document_->file().events.size()); ++index) {
        const double candidate = std::fabs(
            document_->file().events[static_cast<std::size_t>(index)].beat - event.beat);
        if (candidate < distance) { distance = candidate; nearest = index; }
    }
    selectEvent(nearest);
}

void TransitionEditorWindow::deleteSelectedEvent()
{
    const int index = selectedEvent_;
    if (index < 0) return;
    document_->mutate(tr("Delete timeline event"), [index](GvtFile& file) {
        if (index >= 0 && index < static_cast<int>(file.events.size()))
            file.events.erase(file.events.begin() + index);
    });
    selectEvent(-1);
}

void TransitionEditorWindow::duplicateSelectedEvent()
{
    if (selectedEvent_ < 0) return;
    GvtEvent event = document_->file().events[static_cast<std::size_t>(selectedEvent_)];
    event.beat += snapCombo_->currentData().toDouble() > 0.0
                      ? snapCombo_->currentData().toDouble() : 0.25;
    document_->mutate(tr("Duplicate timeline event"), [event](GvtFile& file) {
        file.events.push_back(event);
        std::stable_sort(file.events.begin(), file.events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    });
}

void TransitionEditorWindow::applyEventInspector()
{
    const int index = selectedEvent_;
    if (index < 0) return;
    GvtEvent event = document_->file().events[static_cast<std::size_t>(index)];
    event.role = static_cast<Role>(roleCombo_->currentData().toInt());
    event.control = static_cast<ControlId>(controlCombo_->currentData().toInt());
    if ((event.role == Role::Mixer) !=
        (event.control == ControlId::Crossfader)) {
        QMessageBox::warning(this, tr("Invalid target"),
            tr("Mixer events can only edit the crossfader; deck controls need an endpoint."));
        return;
    }
    event.beat = eventBeatSpin_->value();
    event.value = eventValueSpin_->value();
    event.curve = controlIsTrigger(event.control)
        ? Curve::Step
        : static_cast<Curve>(curveCombo_->currentData().toInt());
    event.gestureControl = static_cast<ControlId>(
        gestureControlCombo_->currentData().toInt());
    event.gesturePadMode = gesturePadModeCombo_->currentData().toInt();
    const QString reference = eventReferenceEdit_->text().trimmed();
    event.cueId.clear();
    event.loopId.clear();
    for (const TransitionHotCue& cue : document_->file().transitionCues) {
        if (cue.id != reference) continue;
        if (cue.role != event.role) {
            QMessageBox::warning(this, tr("Cue belongs to another endpoint"),
                tr("Cue “%1” belongs to %2. Change the event target or choose another cue.")
                    .arg(reference, roleText(cue.role)));
            return;
        }
        event.cueId = reference;
        event.control = ControlId::TransitionCue1;
    }
    for (const TransitionSavedLoop& loop : document_->file().transitionLoops) {
        if (loop.id != reference) continue;
        if (loop.role != event.role) {
            QMessageBox::warning(this, tr("Loop belongs to another endpoint"),
                tr("Loop “%1” belongs to %2. Change the event target or choose another loop.")
                    .arg(reference, roleText(loop.role)));
            return;
        }
        event.loopId = reference;
        event.control = ControlId::TransitionCue1;
    }
    if (!reference.isEmpty() && event.cueId.isEmpty() && event.loopId.isEmpty()) {
        QMessageBox::warning(this, tr("Unknown semantic ID"),
                             tr("No transition cue or loop has ID “%1”.").arg(reference));
        return;
    }
    const bool requiresReference =
        (event.control >= ControlId::HotCue1 &&
         event.control <= ControlId::HotCue8) ||
        (event.control >= ControlId::SavedLoop1 &&
         event.control <= ControlId::SavedLoop8) ||
        (event.control >= ControlId::TransitionCue1 &&
         event.control <= ControlId::TransitionCue8);
    if (requiresReference && reference.isEmpty()) {
        QMessageBox::information(this, tr("Choose a transition cue or loop"),
            tr("This action must reference a transition-owned cue or loop ID."));
        return;
    }
    document_->mutate(tr("Edit timeline event"), [index, event](GvtFile& file) {
        if (index < 0 || index >= static_cast<int>(file.events.size())) return;
        file.events[static_cast<std::size_t>(index)] = event;
        std::stable_sort(file.events.begin(), file.events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    });
    selectEvent(-1);
}

void TransitionEditorWindow::rebuildPerformanceDefinitions()
{
    performanceTable_->setRowCount(0);
    const auto addRow = [this](const QStringList& values,
                               const QString& kind, int index) {
        const int row = performanceTable_->rowCount();
        performanceTable_->insertRow(row);
        for (int column = 0; column < performanceTable_->columnCount(); ++column) {
            auto* item = new QTableWidgetItem(values.value(column));
            if (column == 0) {
                item->setData(Qt::UserRole, kind);
                item->setData(Qt::UserRole + 1, index);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            if (kind == QLatin1String("label") &&
                (column == 1 || column == 2 || column == 4 || column >= 6))
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            performanceTable_->setItem(row, column, item);
        }
    };
    const GvtFile& file = document_->file();
    for (int index = 0; index < static_cast<int>(file.transitionCues.size()); ++index) {
        const TransitionHotCue& cue = file.transitionCues[static_cast<std::size_t>(index)];
        addRow({tr("Cue"), roleText(cue.role), cue.id,
                QString::number(cue.trackBeat, 'f', 6), QString(), cue.label,
                cue.purpose, cue.color, cue.pairingGroup,
                cue.preferredPad >= 0 ? QString::number(cue.preferredPad + 1)
                                      : QString(),
                cue.preferredKey},
               QStringLiteral("cue"), index);
    }
    for (int index = 0; index < static_cast<int>(file.transitionLoops.size()); ++index) {
        const TransitionSavedLoop& loop =
            file.transitionLoops[static_cast<std::size_t>(index)];
        addRow({tr("Loop"), roleText(loop.role), loop.id,
                QString::number(loop.startTrackBeat, 'f', 6),
                QString::number(loop.endTrackBeat, 'f', 6), loop.label,
                loop.purpose, loop.color, loop.pairingGroup,
                loop.preferredPad >= 0 ? QString::number(loop.preferredPad + 1)
                                       : QString(),
                loop.preferredKey},
               QStringLiteral("loop"), index);
    }
    for (int index = 0; index < static_cast<int>(file.cues.size()); ++index) {
        const GvtCue& cue = file.cues[static_cast<std::size_t>(index)];
        addRow({tr("Label"), QString(), QString(),
                QString::number(cue.beat, 'f', 6), QString(), cue.label,
                QString(), QString(), QString(), QString(), QString()},
               QStringLiteral("label"), index);
    }
}

void TransitionEditorWindow::addCue()
{
    bool accepted = false;
    const QString side = QInputDialog::getItem(
        this, tr("Add transition cue"), tr("Endpoint"),
        {tr("Outgoing"), tr("Incoming")}, 1, false, &accepted);
    if (!accepted) return;
    const Role role = side == tr("Outgoing") ? Role::FromDeck : Role::ToDeck;
    const QString prefix = role == Role::FromDeck ? QStringLiteral("outgoing-cue")
                                                   : QStringLiteral("incoming-cue");
    QString id = prefix;
    std::set<QString> used;
    for (const TransitionHotCue& cue : document_->file().transitionCues) used.insert(cue.id);
    for (const TransitionSavedLoop& loop : document_->file().transitionLoops) used.insert(loop.id);
    for (int suffix = 2; used.contains(id); ++suffix)
        id = prefix + QLatin1Char('-') + QString::number(suffix);
    TransitionHotCue cue;
    cue.id = id;
    cue.role = role;
    cue.trackBeat = (role == Role::FromDeck ? document_->file().anchorFromBeat
                                            : document_->file().anchorToBeat) +
                    timeline_->playheadBeat();
    cue.label = tr("New cue");
    cue.purpose = QStringLiteral("start-track");
    cue.color = role == Role::FromDeck ? QStringLiteral("#55b9df")
                                       : QStringLiteral("#e85d75");
    document_->mutate(tr("Add transition cue"), [cue](GvtFile& file) {
        file.transitionCues.push_back(cue);
        if (!file.requirements.contains(QStringLiteral("temporary-cues.v1")))
            file.requirements.append(QStringLiteral("temporary-cues.v1"));
    });
}

void TransitionEditorWindow::addLoop()
{
    bool accepted = false;
    const QString side = QInputDialog::getItem(
        this, tr("Add transition loop"), tr("Endpoint"),
        {tr("Outgoing"), tr("Incoming")}, 1, false, &accepted);
    if (!accepted) return;
    const Role role = side == tr("Outgoing") ? Role::FromDeck : Role::ToDeck;
    const QString prefix = role == Role::FromDeck ? QStringLiteral("outgoing-loop")
                                                   : QStringLiteral("incoming-loop");
    QString id = prefix;
    std::set<QString> used;
    for (const TransitionHotCue& cue : document_->file().transitionCues) used.insert(cue.id);
    for (const TransitionSavedLoop& loop : document_->file().transitionLoops) used.insert(loop.id);
    for (int suffix = 2; used.contains(id); ++suffix)
        id = prefix + QLatin1Char('-') + QString::number(suffix);
    TransitionSavedLoop loop;
    loop.id = id;
    loop.role = role;
    loop.startTrackBeat = (role == Role::FromDeck ? document_->file().anchorFromBeat
                                                  : document_->file().anchorToBeat) +
                          timeline_->playheadBeat();
    loop.endTrackBeat = loop.startTrackBeat + 4.0;
    loop.label = tr("New loop");
    loop.purpose = QStringLiteral("saved-loop");
    loop.color = QStringLiteral("#e8a13a");
    document_->mutate(tr("Add transition loop"), [loop](GvtFile& file) {
        file.transitionLoops.push_back(loop);
        if (!file.requirements.contains(QStringLiteral("temporary-loops.v1")))
            file.requirements.append(QStringLiteral("temporary-loops.v1"));
    });
}

void TransitionEditorWindow::deletePerformanceDefinition()
{
    const int row = performanceTable_->currentRow();
    if (row < 0 || !performanceTable_->item(row, 0)) return;
    const QString kind = performanceTable_->item(row, 0)->data(Qt::UserRole).toString();
    const int index = performanceTable_->item(row, 0)->data(Qt::UserRole + 1).toInt();
    document_->mutate(tr("Delete performance definition"),
                      [kind, index](GvtFile& file) {
        if (kind == QLatin1String("cue") && index >= 0 &&
            index < static_cast<int>(file.transitionCues.size())) {
            const QString id = file.transitionCues[static_cast<std::size_t>(index)].id;
            file.transitionCues.erase(file.transitionCues.begin() + index);
            file.events.erase(std::remove_if(file.events.begin(), file.events.end(),
                [&id](const GvtEvent& event) { return event.cueId == id; }),
                file.events.end());
        } else if (kind == QLatin1String("loop") && index >= 0 &&
                   index < static_cast<int>(file.transitionLoops.size())) {
            const QString id = file.transitionLoops[static_cast<std::size_t>(index)].id;
            file.transitionLoops.erase(file.transitionLoops.begin() + index);
            file.events.erase(std::remove_if(file.events.begin(), file.events.end(),
                [&id](const GvtEvent& event) { return event.loopId == id; }),
                file.events.end());
        } else if (kind == QLatin1String("label") && index >= 0 &&
                   index < static_cast<int>(file.cues.size())) {
            file.cues.erase(file.cues.begin() + index);
        }
    });
}

void TransitionEditorWindow::applyPerformanceDefinition()
{
    GvtFile after = document_->file();
    std::vector<TransitionHotCue> cues;
    std::vector<TransitionSavedLoop> loops;
    std::vector<GvtCue> labels;
    std::set<QString> ids;
    std::map<QString, std::pair<QString, Role>> cueChanges;
    std::map<QString, std::pair<QString, Role>> loopChanges;
    const auto roleFrom = [](const QString& value) {
        return value.compare(QObject::tr("Incoming"), Qt::CaseInsensitive) == 0
            ? Role::ToDeck : Role::FromDeck;
    };
    for (int row = 0; row < performanceTable_->rowCount(); ++row) {
        const auto text = [this, row](int column) {
            QTableWidgetItem* item = performanceTable_->item(row, column);
            return item ? item->text().trimmed() : QString();
        };
        QTableWidgetItem* kindItem = performanceTable_->item(row, 0);
        const QString kind = kindItem
            ? kindItem->data(Qt::UserRole).toString() : QString();
        const int originalIndex = kindItem
            ? kindItem->data(Qt::UserRole + 1).toInt() : -1;
        if (kind == QLatin1String("label")) {
            bool ok = false;
            const double beat = text(3).toDouble(&ok);
            if (!ok || text(5).isEmpty()) {
                QMessageBox::warning(this, tr("Invalid label"),
                    tr("Every label needs a numeric beat and non-empty text."));
                return;
            }
            GvtCue label;
            if (originalIndex >= 0 &&
                originalIndex < static_cast<int>(after.cues.size()))
                label = after.cues[static_cast<std::size_t>(originalIndex)];
            label.beat = beat;
            label.label = text(5);
            labels.push_back(std::move(label));
            continue;
        }
        if (kind != QLatin1String("cue") && kind != QLatin1String("loop")) {
            QMessageBox::warning(this, tr("Invalid definition"),
                                 tr("Unknown cue/loop row type."));
            return;
        }
        const bool incomingRole = text(1).compare(
            tr("Incoming"), Qt::CaseInsensitive) == 0;
        const bool outgoingRole = text(1).compare(
            tr("Outgoing"), Qt::CaseInsensitive) == 0;
        if (!incomingRole && !outgoingRole) {
            QMessageBox::warning(this, tr("Invalid endpoint"),
                tr("Cue and loop endpoints must be Outgoing or Incoming."));
            return;
        }
        const QString id = text(2);
        if (id.isEmpty() || ids.contains(id)) {
            QMessageBox::warning(this, tr("Invalid semantic ID"),
                tr("Cue and loop IDs must be non-empty and unique."));
            return;
        }
        ids.insert(id);
        bool startOk = false;
        const double start = text(3).toDouble(&startOk);
        if (!startOk) {
            QMessageBox::warning(this, tr("Invalid beat"),
                                 tr("Cue and loop beats must be numeric."));
            return;
        }
        bool padOk = false;
        const int pad = text(9).toInt(&padOk);
        const QString key = text(10);
        if (!text(9).isEmpty() && (!padOk || pad < 1 || pad > 8)) {
            QMessageBox::warning(this, tr("Invalid preferred pad"),
                tr("Preferred pad must be blank or a number from 1 through 8."));
            return;
        }
        if (kind == QLatin1String("cue")) {
            TransitionHotCue cue;
            if (originalIndex >= 0 &&
                originalIndex < static_cast<int>(after.transitionCues.size())) {
                cue = after.transitionCues[static_cast<std::size_t>(originalIndex)];
                cueChanges.emplace(cue.id,
                                   std::make_pair(id, roleFrom(text(1))));
            }
            cue.id = id;
            cue.role = roleFrom(text(1));
            cue.trackBeat = start;
            cue.label = text(5);
            cue.purpose = text(6);
            cue.color = text(7);
            cue.pairingGroup = text(8);
            cue.preferredPad = padOk && pad >= 1 && pad <= 8 ? pad - 1 : -1;
            cue.preferredKey = key;
            cues.push_back(cue);
        } else {
            bool endOk = false;
            const double end = text(4).toDouble(&endOk);
            if (!endOk || end <= start) {
                QMessageBox::warning(this, tr("Invalid loop"),
                    tr("Each loop needs an end beat after its start beat."));
                return;
            }
            TransitionSavedLoop loop;
            if (originalIndex >= 0 &&
                originalIndex < static_cast<int>(after.transitionLoops.size())) {
                loop = after.transitionLoops[static_cast<std::size_t>(originalIndex)];
                loopChanges.emplace(loop.id,
                                    std::make_pair(id, roleFrom(text(1))));
            }
            loop.id = id;
            loop.role = roleFrom(text(1));
            loop.startTrackBeat = start;
            loop.endTrackBeat = end;
            loop.label = text(5);
            loop.purpose = text(6);
            loop.color = text(7);
            loop.pairingGroup = text(8);
            loop.preferredPad = padOk && pad >= 1 && pad <= 8 ? pad - 1 : -1;
            loop.preferredKey = key;
            loops.push_back(loop);
        }
    }
    after.transitionCues = std::move(cues);
    after.transitionLoops = std::move(loops);
    after.cues = std::move(labels);
    for (GvtEvent& event : after.events) {
        if (!event.cueId.isEmpty()) {
            const auto change = cueChanges.find(event.cueId);
            if (change != cueChanges.end()) {
                event.cueId = change->second.first;
                event.role = change->second.second;
                event.control = ControlId::TransitionCue1;
            }
        } else if (!event.loopId.isEmpty()) {
            const auto change = loopChanges.find(event.loopId);
            if (change != loopChanges.end()) {
                event.loopId = change->second.first;
                event.role = change->second.second;
                event.control = ControlId::TransitionCue1;
            }
        }
    }
    std::stable_sort(after.cues.begin(), after.cues.end(),
                     [](const GvtCue& a, const GvtCue& b) { return a.beat < b.beat; });
    document_->apply(after, tr("Edit cues, loops and labels"));
}

void TransitionEditorWindow::rebuildInitialStateTable()
{
    struct Row { const char* key; const char* label; };
    static constexpr Row rows[] = {
        {"playing", "Playing (0/1)"}, {"position", "Position beat"},
        {"cue", "Cue beat"}, {"tempo", "Tempo ratio"},
        {"fader", "Channel fader"}, {"eq_low", "EQ low"},
        {"eq_mid", "EQ mid"}, {"eq_high", "EQ high"},
        {"filter", "Filter"}, {"quantize", "Quantize (0/1)"},
        {"loop_active", "Loop active (0/1)"},
        {"loop_start", "Loop start beat"}, {"loop_end", "Loop end beat"},
        {"fx_type", "FX type (0..2)"}, {"fx_on", "FX on (0/1)"},
        {"fx_wet", "FX wet"}, {"fx_beats", "FX beats"},
        {"stem_vocals", "Stem vocals"}, {"stem_melody", "Stem melody"},
        {"stem_bass", "Stem bass"}, {"stem_drums", "Stem drums"},
        {"crossfader", "Mixer crossfader"},
    };
    initialTable_->setRowCount(static_cast<int>(std::size(rows)));
    const auto value = [](const GvtInitialState& state, const QString& key) {
        if (key == QLatin1String("playing")) return state.playing ? 1.0 : 0.0;
        if (key == QLatin1String("position")) return state.positionBeat;
        if (key == QLatin1String("cue")) return state.cueBeat;
        if (key == QLatin1String("tempo")) return state.tempoRatio;
        if (key == QLatin1String("fader")) return state.fader;
        if (key == QLatin1String("eq_low")) return state.eqLow;
        if (key == QLatin1String("eq_mid")) return state.eqMid;
        if (key == QLatin1String("eq_high")) return state.eqHigh;
        if (key == QLatin1String("filter")) return state.filter;
        if (key == QLatin1String("quantize")) return state.quantize ? 1.0 : 0.0;
        if (key == QLatin1String("loop_active")) return state.loopActive ? 1.0 : 0.0;
        if (key == QLatin1String("loop_start")) return state.loopStartBeat;
        if (key == QLatin1String("loop_end")) return state.loopEndBeat;
        if (key == QLatin1String("fx_type")) return static_cast<double>(state.fxType);
        if (key == QLatin1String("fx_on")) return state.fxOn ? 1.0 : 0.0;
        if (key == QLatin1String("fx_wet")) return state.fxWet;
        if (key == QLatin1String("fx_beats")) return state.fxBeats;
        if (key == QLatin1String("stem_vocals")) return state.stemVocals;
        if (key == QLatin1String("stem_melody")) return state.stemMelody;
        if (key == QLatin1String("stem_bass")) return state.stemBass;
        if (key == QLatin1String("stem_drums")) return state.stemDrums;
        return 0.0;
    };
    for (int row = 0; row < static_cast<int>(std::size(rows)); ++row) {
        auto* label = new QTableWidgetItem(tr(rows[row].label));
        label->setData(Qt::UserRole, QString::fromLatin1(rows[row].key));
        label->setFlags(label->flags() & ~Qt::ItemIsEditable);
        initialTable_->setItem(row, 0, label);
        if (QString::fromLatin1(rows[row].key) == QLatin1String("crossfader")) {
            initialTable_->setItem(row, 1, new QTableWidgetItem(
                QString::number(document_->file().initialCrossfader, 'f', 6)));
            auto* blank = new QTableWidgetItem;
            blank->setFlags(blank->flags() & ~Qt::ItemIsEditable);
            initialTable_->setItem(row, 2, blank);
        } else {
            initialTable_->setItem(row, 1, new QTableWidgetItem(QString::number(
                value(document_->file().initialFrom, QString::fromLatin1(rows[row].key)),
                'f', 6)));
            initialTable_->setItem(row, 2, new QTableWidgetItem(QString::number(
                value(document_->file().initialTo, QString::fromLatin1(rows[row].key)),
                'f', 6)));
        }
    }
}

void TransitionEditorWindow::applyInitialCell(int row, int column)
{
    if (refreshing_ || column < 1 || column > 2 ||
        !initialTable_->item(row, 0) || !initialTable_->item(row, column))
        return;
    bool ok = false;
    const double value = initialTable_->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
        refreshUi();
        return;
    }
    const QString key = initialTable_->item(row, 0)->data(Qt::UserRole).toString();
    if (key == QLatin1String("crossfader")) {
        if (column != 1 || value < 0.0 || value > 1.0) { refreshUi(); return; }
        document_->mutate(tr("Edit initial crossfader"), [value](GvtFile& file) {
            file.initialMixerCaptured = true;
            file.initialCrossfader = value;
        });
        return;
    }
    const bool outgoing = column == 1;
    document_->mutate(tr("Edit initial state"), [=](GvtFile& file) {
        file.initialComplete = true;
        GvtInitialState& state = outgoing ? file.initialFrom : file.initialTo;
        state.captured = true;
        if (key == QLatin1String("playing")) state.playing = value >= 0.5;
        else if (key == QLatin1String("position")) state.positionBeat = value;
        else if (key == QLatin1String("cue")) state.cueBeat = value;
        else if (key == QLatin1String("tempo")) state.tempoRatio = std::clamp(value, 0.01, 4.0);
        else if (key == QLatin1String("fader")) state.fader = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("eq_low")) state.eqLow = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("eq_mid")) state.eqMid = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("eq_high")) state.eqHigh = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("filter")) state.filter = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("quantize")) {
            state.quantizeCaptured = true; state.quantize = value >= 0.5;
        } else if (key == QLatin1String("loop_active")) state.loopActive = value >= 0.5;
        else if (key == QLatin1String("loop_start")) state.loopStartBeat = value;
        else if (key == QLatin1String("loop_end")) state.loopEndBeat = value;
        else if (key == QLatin1String("fx_type")) state.fxType = std::clamp(static_cast<int>(std::round(value)), 0, 2);
        else if (key == QLatin1String("fx_on")) state.fxOn = value >= 0.5;
        else if (key == QLatin1String("fx_wet")) state.fxWet = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("fx_beats")) state.fxBeats = std::clamp(value, 0.25, 4.0);
        else if (key == QLatin1String("stem_vocals")) state.stemVocals = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("stem_melody")) state.stemMelody = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("stem_bass")) state.stemBass = std::clamp(value, 0.0, 1.0);
        else if (key == QLatin1String("stem_drums")) state.stemDrums = std::clamp(value, 0.0, 1.0);
    });
}

void TransitionEditorWindow::applyYaml()
{
    GvtFile parsed;
    QString error;
    QStringList warnings;
    if (!transitionParse(yamlEdit_->toPlainText(), parsed, &error, &warnings)) {
        QMessageBox::warning(this, tr("Invalid transition YAML"), error);
        return;
    }
    if (!warnings.isEmpty() && QMessageBox::warning(
            this, tr("Apply with warnings?"),
            tr("%1\n\nApply the parsed document?")
                .arg(warnings.join(QStringLiteral("\n"))),
            QMessageBox::Apply | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Apply)
        return;
    if (parsed.id != document_->file().id ||
        !sameEndpointProfile(parsed.from, document_->file().from) ||
        !sameEndpointProfile(parsed.to, document_->file().to))
        requiresSaveAs_ = true;
    parsed.filePath = document_->file().filePath;
    parsed.sourceFormat = document_->file().sourceFormat;
    document_->apply(parsed, tr("Apply YAML source"));
    outgoing_ = resolveTrack(parsed, true);
    incoming_ = resolveTrack(parsed, false);
    timeline_->setTracks(outgoing_, incoming_);
}

void TransitionEditorWindow::updateYamlFromModel()
{
    if (yamlEdit_->hasFocus() && yamlEdit_->document()->isModified()) return;
    yamlEdit_->setPlainText(transitionSerialize(document_->file()));
    yamlEdit_->document()->setModified(false);
}

void TransitionEditorWindow::setEndpoint(bool outgoing)
{
    QStringList names;
    std::vector<TrackDataPtr> tracks;
    for (int row = 0; library_ && row < library_->trackCount(); ++row) {
        const TrackDataPtr track = library_->trackAt(row);
        if (!track) continue;
        tracks.push_back(track);
        names.append(track->artist.isEmpty()
                         ? track->title
                         : QStringLiteral("%1 — %2").arg(track->artist, track->title));
    }
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        this, outgoing ? tr("Change outgoing endpoint")
                       : tr("Change incoming endpoint"),
        tr("Canonical song arrangement"), names, 0, false, &accepted);
    if (!accepted) return;
    const int index = names.indexOf(selected);
    if (index < 0) return;
    const TrackDataPtr track = tracks[static_cast<std::size_t>(index)];
    const GvtTrackRef ref = trackReference(*track);
    document_->mutate(outgoing ? tr("Change outgoing endpoint")
                               : tr("Change incoming endpoint"),
                      [outgoing, ref](GvtFile& file) {
        if (outgoing) file.from = ref;
        else file.to = ref;
    });
    if (outgoing) outgoing_ = track; else incoming_ = track;
    if (outgoing) outgoingStems_.reset(); else incomingStems_.reset();
    timeline_->setTracks(outgoing_, incoming_);
    requiresSaveAs_ = true;
    statusBar()->showMessage(
        tr("Changing a canonical endpoint requires Save As; the original is protected."),
        6000);
}

QString TransitionEditorWindow::draftPath() const
{
    QString base = document_->file().id;
    if (base.isEmpty()) base = QString::fromLatin1(
        QCryptographicHash::hash(sourcePath_.toUtf8(), QCryptographicHash::Sha256)
            .toHex().left(24));
    const QString directory = QDir::homePath() + QStringLiteral("/.gravitino/drafts");
    return directory + QLatin1Char('/') + base + QStringLiteral(".draft.json");
}

QString TransitionEditorWindow::sourceDigest() const
{
    return QString::fromLatin1(sourceHash_.toHex());
}

void TransitionEditorWindow::scheduleDraftSave()
{
    if ((!document_->isDirty() && !isNew_) || !draftTimer_) return;
    draftTimer_->start();
}

void TransitionEditorWindow::writeDraft()
{
    if (!document_->isDirty() && !isNew_) return;
    const QString path = draftPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject root;
    root.insert(QStringLiteral("source_path"), sourcePath_);
    root.insert(QStringLiteral("source_sha256"), sourceDigest());
    root.insert(QStringLiteral("saved_at"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("transition_yaml"),
                transitionSerialize(document_->file()));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        statusBar()->showMessage(tr("Could not autosave the editor draft"), 5000);
    }
}

void TransitionEditorWindow::removeDraft()
{
    QFile::remove(draftPath());
}

bool TransitionEditorWindow::maybeResolveDraft(GvtFile& file)
{
    QString base = file.id;
    if (base.isEmpty()) base = QString::fromLatin1(
        QCryptographicHash::hash(sourcePath_.toUtf8(), QCryptographicHash::Sha256)
            .toHex().left(24));
    const QString path = QDir::homePath() +
        QStringLiteral("/.gravitino/drafts/") + base +
        QStringLiteral(".draft.json");
    QFile draft(path);
    if (!draft.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument json = QJsonDocument::fromJson(draft.readAll());
    if (!json.isObject()) return false;
    const QJsonObject object = json.object();
    const QString yaml = object.value(QStringLiteral("transition_yaml")).toString();
    if (yaml.isEmpty()) return false;
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, tr("Recover transition draft?"),
        tr("An autosaved draft exists from %1. Restore it instead of the saved file?")
            .arg(object.value(QStringLiteral("saved_at")).toString()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) {
        QFile::remove(path);
        return false;
    }
    GvtFile recovered;
    QString error;
    if (!transitionParse(yaml, recovered, &error, nullptr)) {
        QMessageBox::warning(this, tr("Draft is invalid"), error);
        return false;
    }
    recovered.filePath = file.filePath;
    recovered.sourceFormat = file.sourceFormat;
    const QByteArray draftSourceHash = QByteArray::fromHex(
        object.value(QStringLiteral("source_sha256"))
            .toString().toLatin1());
    if (!draftSourceHash.isEmpty()) sourceHash_ = draftSourceHash;
    file = std::move(recovered);
    return true;
}

bool TransitionEditorWindow::maybeRecoverUnsavedDraft()
{
    QDir directory(QDir::homePath() + QStringLiteral("/.gravitino/drafts"));
    const QFileInfoList drafts = directory.entryInfoList(
        {QStringLiteral("*.draft.json")}, QDir::Files,
        QDir::Time | QDir::Reversed);
    for (auto it = drafts.crbegin(); it != drafts.crend(); ++it) {
        QFile draft(it->absoluteFilePath());
        if (!draft.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument json = QJsonDocument::fromJson(draft.readAll());
        if (!json.isObject()) continue;
        const QJsonObject object = json.object();
        if (!object.value(QStringLiteral("source_path")).toString().isEmpty())
            continue;
        GvtFile recovered;
        QString error;
        if (!transitionParse(
                object.value(QStringLiteral("transition_yaml")).toString(),
                recovered, &error, nullptr))
            continue;

        QMessageBox recovery(this);
        recovery.setIcon(QMessageBox::Question);
        recovery.setWindowTitle(tr("Recover unsaved transition?"));
        recovery.setText(tr("An autosaved draft for “%1” is available.")
                             .arg(recovered.name.isEmpty()
                                      ? tr("Untitled") : recovered.name));
        recovery.setInformativeText(tr("It was last saved at %1.")
            .arg(object.value(QStringLiteral("saved_at")).toString()));
        QPushButton* restore = recovery.addButton(
            tr("Restore Draft"), QMessageBox::AcceptRole);
        QPushButton* discard = recovery.addButton(
            tr("Discard Draft"), QMessageBox::DestructiveRole);
        recovery.addButton(tr("Create Another"), QMessageBox::RejectRole);
        recovery.setDefaultButton(restore);
        recovery.exec();
        if (recovery.clickedButton() == discard) {
            QFile::remove(it->absoluteFilePath());
            continue;
        }
        if (recovery.clickedButton() != restore) return false;

        sourcePath_.clear();
        sourceHash_.clear();
        outgoing_ = resolveTrack(recovered, true);
        incoming_ = resolveTrack(recovered, false);
        outgoingStems_.reset();
        incomingStems_.reset();
        setWorkingFile(recovered, true);
        statusBar()->showMessage(tr("Recovered unsaved transition draft"),
                                 5000);
        return true;
    }
    return false;
}

bool TransitionEditorWindow::persist(bool forceSaveAs)
{
    const QString previousDraftPath = draftPath();
    const QStringList errors = document_->validationErrors();
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, tr("Transition is not ready to save"),
                             errors.join(QStringLiteral("\n• ")).prepend(QStringLiteral("• ")));
        return false;
    }
    GvtFile output = document_->file();
    QString error;
    QString savedPath;

    if (!sourcePath_.isEmpty() && !sourceHash_.isEmpty() &&
        fileHash(sourcePath_) != sourceHash_ && !forceSaveAs) {
        QMessageBox conflict(this);
        conflict.setIcon(QMessageBox::Warning);
        conflict.setWindowTitle(tr("Transition changed on disk"));
        conflict.setText(tr("The saved transition changed after this editor opened."));
        conflict.setInformativeText(
            tr("Reload the newer saved file, or protect both versions by saving this edit as a new transition."));
        QPushButton* saveCopy = conflict.addButton(
            tr("Save as New…"), QMessageBox::AcceptRole);
        QPushButton* reload = conflict.addButton(
            tr("Reload Saved File"), QMessageBox::DestructiveRole);
        conflict.addButton(QMessageBox::Cancel);
        conflict.setDefaultButton(saveCopy);
        conflict.exec();
        if (conflict.clickedButton() == reload) {
            GvtFile diskFile;
            QString loadError;
            QStringList warnings;
            if (!loadTransitionFile(sourcePath_, diskFile, &loadError,
                                    &warnings)) {
                QMessageBox::warning(this, tr("Could not reload transition"),
                                     loadError);
                return false;
            }
            removeDraft();
            sourceHash_ = fileHash(sourcePath_);
            outgoing_ = resolveTrack(diskFile, true);
            incoming_ = resolveTrack(diskFile, false);
            setWorkingFile(diskFile, false);
            statusBar()->showMessage(tr("Reloaded the newer saved transition"),
                                     5000);
            return false;
        }
        if (conflict.clickedButton() != saveCopy) return false;
        forceSaveAs = true;
    }

    const bool legacy = output.sourceFormat == TransitionSourceFormat::LegacyGvt;
    const bool saveCopy = forceSaveAs || isNew_ || requiresSaveAs_;
    if (saveCopy) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Save transition as"), tr("Transition name"),
            QLineEdit::Normal, output.name, &accepted).trimmed();
        if (!accepted) return false;
        if (name.isEmpty()) {
            QMessageBox::warning(this, tr("Transition needs a name"),
                                 tr("Enter a non-empty transition name."));
            return false;
        }
        output.name = name;
        output.id.clear();
        output.filePath.clear();
        output.sourceFormat = TransitionSourceFormat::Unsaved;
        if (requiresSaveAs_ || forceSaveAs) {
            output.legacySourceId.clear();
            QJsonObject legacyObject = output.extensions
                .value(QStringLiteral("gravitino.legacy")).toObject();
            legacyObject.remove(QStringLiteral("source_id"));
            if (legacyObject.isEmpty())
                output.extensions.remove(QStringLiteral("gravitino.legacy"));
            else
                output.extensions.insert(QStringLiteral("gravitino.legacy"),
                                         legacyObject);
        }
        savedPath = store_->save(output, &error);
    } else if (legacy) {
        if (!store_->update(output, &error)) return false;
        for (const GvtFile& candidate : store_->all()) {
            if (candidate.legacySourceId == output.id) {
                savedPath = candidate.filePath;
                output = candidate;
                break;
            }
        }
    } else if (output.name.trimmed() != originalName_.trimmed()) {
        savedPath = store_->renameTransition(output, output.name, &error);
    } else {
        if (store_->update(output, &error)) savedPath = output.filePath;
    }

    if (savedPath.isEmpty()) {
        QMessageBox::warning(this, tr("Could not save transition"),
                             error.isEmpty() ? tr("The transition store did not return a file path.")
                                             : error);
        return false;
    }
    const auto found = std::find_if(store_->all().begin(), store_->all().end(),
                                    [&savedPath](const GvtFile& file) {
        return QFileInfo(file.filePath).absoluteFilePath() ==
               QFileInfo(savedPath).absoluteFilePath();
    });
    if (found != store_->all().end()) output = *found;
    else {
        output.filePath = savedPath;
        output.sourceFormat = TransitionSourceFormat::PortableYaml;
    }
    sourcePath_ = savedPath;
    sourceHash_ = fileHash(savedPath);
    isNew_ = false;
    requiresSaveAs_ = false;
    originalName_ = output.name;
    document_->reset(output);
    outgoing_ = resolveTrack(output, true);
    incoming_ = resolveTrack(output, false);
    timeline_->setTracks(outgoing_, incoming_);
    QFile::remove(previousDraftPath);
    removeDraft();
    emit transitionSaved(savedPath);
    emit statusMessage(tr("Saved transition “%1”").arg(output.name), 4000);
    statusBar()->showMessage(tr("Saved %1").arg(savedPath), 5000);
    return true;
}

void TransitionEditorWindow::save()
{
    persist(requiresSaveAs_ || isNew_);
}

void TransitionEditorWindow::saveAs()
{
    persist(true);
}

bool TransitionEditorWindow::ensureCanDiscard()
{
    if (!document_->isDirty() && !isNew_) return true;
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowTitle(tr("Unsaved transition changes"));
    prompt.setText(tr("Keep an autosaved draft before leaving this transition?"));
    prompt.setInformativeText(
        tr("Keeping a draft does not modify the saved .transition file."));
    QPushButton* keep = prompt.addButton(tr("Keep Draft"), QMessageBox::AcceptRole);
    QPushButton* discard = prompt.addButton(
        tr("Discard Changes"), QMessageBox::DestructiveRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(keep);
    prompt.exec();
    if (prompt.clickedButton() == keep) writeDraft();
    else if (prompt.clickedButton() == discard) removeDraft();
    else return false;
    return true;
}

void TransitionEditorWindow::closeEvent(QCloseEvent* event)
{
    if (!ensureCanDiscard()) {
        event->ignore();
        return;
    }
    stopPreview();
    QSettings().setValue(QStringLiteral("transitionEditor/geometry"),
                         saveGeometry());
    event->accept();
}

void TransitionEditorWindow::startOrPausePreview()
{
    if (preview_->active) {
        if (preview_->leased) {
            liveEngine_->releaseExclusivePreview(preview_.get());
            preview_->leased = false;
        }
        preview_->active = false;
        previewTimer_->stop();
        emit previewStateChanged(false);
        playButton_->setText(tr("▶ RESUME FROM CURSOR"));
        finishAutomationTake(true);
        return;
    }
    if (!outgoing_ || !incoming_) {
        QMessageBox::information(this, tr("Preview audio unavailable"),
            tr("Choose or bind compatible local audio for both endpoints first."));
        return;
    }
    if ((recorder_ && recorder_->isRecording()) ||
        (player_ && player_->isActive())) {
        QMessageBox::information(this, tr("Finish the active transition"),
            tr("Editor preview cannot take the master output during recording, Perform, Prime, or Tutorial."));
        return;
    }
    if (masterRecorder_ && masterRecorder_->isRecording()) {
        QMessageBox::information(this, tr("Stop master recording"),
            tr("Editor preview is intentionally excluded from master recordings."));
        return;
    }
    if (!requiredStemsReady()) {
        QMessageBox::information(
            this, tr("Prepare stems before preview"),
            stemSeparator_
                ? tr("This transition automates separated stems. Click PREPARE STEMS and wait for both required endpoints before auditioning it.")
                : tr("This transition automates separated stems, but stem separation is not available in this build."));
        return;
    }
    const QStringList errors = document_->validationErrors();
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, tr("Preview is blocked"),
            errors.join(QStringLiteral("\n• ")).prepend(QStringLiteral("• ")));
        return;
    }

    statusBar()->showMessage(tr("Preparing exact state at beat %1…")
                                 .arg(timeline_->playheadBeat(), 0, 'f', 3));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    preview_->reset(document_->file(), outgoing_, incoming_,
                    outgoingStems_, incomingStems_);
    preview_->primeTo(timeline_->playheadBeat());
    QApplication::restoreOverrideCursor();
    QString error;
    if (!liveEngine_->acquireExclusivePreview(preview_.get(), &error)) {
        QMessageBox::warning(this, tr("Could not start preview"), error);
        return;
    }
    preview_->leased = true;
    preview_->active = true;
    emit previewStateChanged(true);
    preview_->produce();
    previewTimer_->start();
    playButton_->setText(tr("❚❚ PAUSE PREVIEW"));
    stopButton_->setEnabled(true);
    if (writeAutomationCheck_->isChecked()) beginAutomationTake();
    statusBar()->showMessage(
        tr("Editor preview owns MASTER; the live decks are frozen and unchanged."));
}

void TransitionEditorWindow::stopPreview()
{
    if (!preview_) return;
    const bool wasActive = preview_->active || preview_->leased;
    previewTimer_->stop();
    if (preview_->leased && liveEngine_) {
        liveEngine_->releaseExclusivePreview(preview_.get());
        preview_->leased = false;
    }
    const bool hadTake = !takeEvents_.empty();
    preview_->active = false;
    if (hadTake) finishAutomationTake(true);
    if (playButton_) playButton_->setText(tr("▶ PREVIEW FROM CURSOR"));
    if (stopButton_) stopButton_->setEnabled(false);
    if (wasActive) emit previewStateChanged(false);
}

void TransitionEditorWindow::updatePreviewTick()
{
    preview_->produce();
    timeline_->setPlayheadBeat(std::min(preview_->beat, preview_->endBeat));
    playheadLabel_->setText(tr("Beat %1").arg(preview_->beat, 0, 'f', 3));
    const int x = kTimelineLeft + static_cast<int>(
        timeline_->playheadBeat() * timeline_->pixelsPerBeat());
    timelineScroll_->horizontalScrollBar()->setValue(
        std::max(0, x - timelineScroll_->viewport()->width() / 2));
    if (!preview_->active) stopPreview();
}

void TransitionEditorWindow::recordControlValue(Role role, ControlId control,
                                                double value)
{
    if (preview_->active && isStemControl(control) &&
        ((role == Role::FromDeck && !outgoingStems_) ||
         (role == Role::ToDeck && !incomingStems_))) {
        statusBar()->showMessage(
            tr("Stop preview, add the stem action, then PREPARE STEMS before auditioning it."),
            6000);
        return;
    }
    GvtEvent event;
    event.role = role;
    event.control = control;
    event.value = value;
    event.curve = controlIsTrigger(control) ? Curve::Step : Curve::Linear;
    event.beat = preview_->active ? preview_->beat : timeline_->playheadBeat();
    if (preview_->active) {
        preview_->dispatch(role, control, value);
        if (writeAutomationCheck_->isChecked())
            takeEvents_.push_back(event);
        // With Write off, the performance strip is a non-destructive audition
        // surface. With Write on, touched values stay in the pending take and
        // are committed together when preview stops or pauses.
        return;
    }
    document_->mutate(tr("Add control action"), [event](GvtFile& file) {
        file.events.push_back(event);
        std::stable_sort(file.events.begin(), file.events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    });
}

void TransitionEditorWindow::beginAutomationTake()
{
    takeEvents_.clear();
    statusBar()->showMessage(tr("Automation write armed — touched lanes will be punch-replaced."));
}

void TransitionEditorWindow::finishAutomationTake(bool commit)
{
    if (takeEvents_.empty()) return;
    std::vector<GvtEvent> captured = std::move(takeEvents_);
    takeEvents_.clear();
    if (!commit) return;

    // Thin redundant high-rate slider samples while preserving first/last and
    // any meaningful bend. Then punch-replace only each touched stream span.
    std::map<std::pair<int, int>, std::vector<GvtEvent>> streams;
    for (const GvtEvent& event : captured)
        streams[{static_cast<int>(event.role), static_cast<int>(event.control)}]
            .push_back(event);
    std::vector<GvtEvent> thinned;
    std::map<std::pair<int, int>, std::pair<double, double>> ranges;
    for (auto& [key, events] : streams) {
        std::stable_sort(events.begin(), events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
        ranges[key] = {events.front().beat, events.back().beat};
        for (std::size_t index = 0; index < events.size(); ++index) {
            const bool endpoint = index == 0 || index + 1 == events.size();
            const bool changed = index > 0 &&
                std::fabs(events[index].value - events[index - 1].value) >= 0.005;
            const bool spaced = index > 0 &&
                events[index].beat - events[index - 1].beat >= 0.05;
            if (endpoint || changed || spaced) thinned.push_back(events[index]);
        }
    }
    document_->mutate(tr("Record automation take"),
                      [ranges, thinned](GvtFile& file) mutable {
        file.events.erase(std::remove_if(file.events.begin(), file.events.end(),
            [&ranges](const GvtEvent& event) {
                const auto found = ranges.find(
                    {static_cast<int>(event.role), static_cast<int>(event.control)});
                return found != ranges.end() && event.beat >= found->second.first &&
                       event.beat <= found->second.second;
            }), file.events.end());
        for (GvtEvent event : thinned) {
            if (controlIsTrigger(event.control)) event.curve = Curve::Step;
            file.events.push_back(std::move(event));
        }
        std::stable_sort(file.events.begin(), file.events.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    });
}

} // namespace gvt
