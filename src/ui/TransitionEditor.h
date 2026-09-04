// Full transition authoring window: owns an undoable typed working document,
// beat timeline, property inspectors, recovery drafts, and isolated preview.
#pragma once

#include <QMainWindow>
#include <QPointer>
#include <functional>
#include <memory>
#include "../analysis/TrackData.h"
#include "../transitions/Transition.h"

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPainter;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTimer;
class QUndoStack;

namespace gvt {

class AudioEngine;
class MasterRecorder;
class StemSeparator;
class TrackLibrary;
class TransitionPlayer;
class TransitionRecorder;
class TransitionStore;

class TransitionEditorDocument final : public QObject {
    Q_OBJECT
public:
    explicit TransitionEditorDocument(QObject* parent = nullptr);
    const GvtFile& file() const noexcept { return file_; }
    QUndoStack* undoStack() const noexcept { return undo_; }
    bool isDirty() const;
    void reset(const GvtFile& file);
    void apply(const GvtFile& file, const QString& description);
    void mutate(const QString& description,
                const std::function<void(GvtFile&)>& mutation);
    void markSaved();
    QStringList validationErrors() const;
    double effectiveEndBeat() const;
    // QUndoCommand callback; public only so the command can live in the
    // implementation file without exposing its type in this header.
    void setFromCommand(const GvtFile& file);

signals:
    void changed();
    void dirtyChanged(bool dirty);

private:
    GvtFile file_;
    QUndoStack* undo_ = nullptr;
};

class TransitionTimelineView final : public QWidget {
    Q_OBJECT
public:
    explicit TransitionTimelineView(TransitionEditorDocument* document,
                                    QWidget* parent = nullptr);
    void setTracks(TrackDataPtr outgoing, TrackDataPtr incoming);
    void setPixelsPerBeat(double value);
    double pixelsPerBeat() const noexcept { return pixelsPerBeat_; }
    void setPlayheadBeat(double beat);
    double playheadBeat() const noexcept { return playheadBeat_; }
    void setSnapBeats(double beats) noexcept { snapBeats_ = beats; }
    void setSelectedEvent(int index);

signals:
    void eventSelected(int index);
    void playheadChanged(double beat);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    struct Lane { Role role; ControlId control; };
    enum class DragDefinition { None, Cue, LoopStart, LoopEnd, Label };
    std::vector<Lane> lanes() const;
    void updateCanvasSize();
    double beatAtX(double x, Qt::KeyboardModifiers modifiers) const;
    double valueAtY(ControlId control, double y, int laneTop) const;
    double normalizedValue(ControlId control, double value) const;
    int eventAt(const QPointF& point) const;
    int laneAtY(double y) const;
    QRect waveformRect(int roleIndex) const;
    QRect actionRect() const;
    QRect automationRect(int laneIndex) const;
    double incomingLaunchBeat() const;
    void drawWaveform(QPainter& painter, const QRect& rect,
                      const TrackDataPtr& track, Role role);

    TransitionEditorDocument* document_ = nullptr;
    TrackDataPtr outgoing_;
    TrackDataPtr incoming_;
    double pixelsPerBeat_ = 28.0;
    double snapBeats_ = 0.25;
    double playheadBeat_ = 0.0;
    int selectedEvent_ = -1;
    int dragEvent_ = -1;
    bool dragEnd_ = false;
    QPointF dragStart_;
    GvtEvent dragOriginal_;
    GvtEvent dragPreview_;
    double dragOriginalEnd_ = 0.0;
    double dragPreviewEnd_ = 0.0;
    DragDefinition dragDefinition_ = DragDefinition::None;
    int dragDefinitionIndex_ = -1;
    Role dragDefinitionRole_ = Role::FromDeck;
    double dragDefinitionOriginalBeat_ = 0.0;
    double dragDefinitionPreviewBeat_ = 0.0;
};

class TransitionEditorWindow final : public QMainWindow {
    Q_OBJECT
public:
    TransitionEditorWindow(AudioEngine* liveEngine, TrackLibrary* library,
                           TransitionStore* store,
                           TransitionRecorder* recorder,
                           TransitionPlayer* player,
                           MasterRecorder* masterRecorder = nullptr,
                           StemSeparator* stemSeparator = nullptr,
                           QWidget* parent = nullptr);
    ~TransitionEditorWindow() override;

    void openTransition(const GvtFile& file);
    bool createTransition();

signals:
    void transitionSaved(const QString& path);
    void statusMessage(const QString& message, int timeoutMs);
    void previewStateChanged(bool active);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void save();
    void saveAs();
    void addEvent();
    void deleteSelectedEvent();
    void duplicateSelectedEvent();
    void applyEventInspector();
    void applyYaml();
    void refreshUi();
    void startOrPausePreview();
    void stopPreview();
    void recordControlValue(Role role, ControlId control, double value);

private:
    struct Preview;
    void buildUi();
    void setWorkingFile(const GvtFile& file, bool isNew);
    TrackDataPtr resolveTrack(const GvtFile& file, bool outgoing) const;
    GvtTrackRef trackReference(const TrackData& track) const;
    bool chooseEndpoints(TrackDataPtr& outgoing, TrackDataPtr& incoming);
    bool maybeRecoverUnsavedDraft();
    bool maybeResolveDraft(GvtFile& file);
    void scheduleDraftSave();
    void writeDraft();
    void removeDraft();
    QString draftPath() const;
    QString sourceDigest() const;
    bool ensureCanDiscard();
    bool persist(bool forceSaveAs);
    void selectEvent(int index);
    void rebuildEventTable();
    void rebuildPerformanceDefinitions();
    void rebuildInitialStateTable();
    void updateEventInspector();
    void updateValidation();
    void updateYamlFromModel();
    void setEndpoint(bool outgoing);
    void addCue();
    void addLoop();
    void deletePerformanceDefinition();
    void applyPerformanceDefinition();
    void applyInitialCell(int row, int column);
    void beginAutomationTake();
    void finishAutomationTake(bool commit);
    void updatePreviewTick();
    void prepareRequiredStems();
    bool endpointNeedsStems(Role role) const;
    bool requiredStemsReady() const;

    AudioEngine* liveEngine_ = nullptr;
    TrackLibrary* library_ = nullptr;
    TransitionStore* store_ = nullptr;
    TransitionRecorder* recorder_ = nullptr;
    TransitionPlayer* player_ = nullptr;
    MasterRecorder* masterRecorder_ = nullptr;
    StemSeparator* stemSeparator_ = nullptr;
    TransitionEditorDocument* document_ = nullptr;
    std::unique_ptr<Preview> preview_;
    TrackDataPtr outgoing_;
    TrackDataPtr incoming_;
    StemSetPtr outgoingStems_;
    StemSetPtr incomingStems_;
    QString sourcePath_;
    QString originalName_;
    QByteArray sourceHash_;
    bool isNew_ = false;
    bool requiresSaveAs_ = false;
    bool refreshing_ = false;
    int selectedEvent_ = -1;

    TransitionTimelineView* timeline_ = nullptr;
    QScrollArea* timelineScroll_ = nullptr;
    QTabWidget* inspectorTabs_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* authorEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QDoubleSpinBox* masterBpmSpin_ = nullptr;
    QDoubleSpinBox* endBeatSpin_ = nullptr;
    QDoubleSpinBox* outgoingAnchorSpin_ = nullptr;
    QDoubleSpinBox* incomingAnchorSpin_ = nullptr;
    QLabel* outgoingTrackLabel_ = nullptr;
    QLabel* incomingTrackLabel_ = nullptr;
    QTableWidget* eventTable_ = nullptr;
    QComboBox* roleCombo_ = nullptr;
    QComboBox* controlCombo_ = nullptr;
    QDoubleSpinBox* eventBeatSpin_ = nullptr;
    QDoubleSpinBox* eventValueSpin_ = nullptr;
    QComboBox* curveCombo_ = nullptr;
    QComboBox* gestureControlCombo_ = nullptr;
    QComboBox* gesturePadModeCombo_ = nullptr;
    QLineEdit* eventReferenceEdit_ = nullptr;
    QPushButton* applyEventButton_ = nullptr;
    QPushButton* deleteEventButton_ = nullptr;
    QTableWidget* performanceTable_ = nullptr;
    QTableWidget* initialTable_ = nullptr;
    QPlainTextEdit* yamlEdit_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QPushButton* prepareStemsButton_ = nullptr;
    QLabel* playheadLabel_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QCheckBox* writeAutomationCheck_ = nullptr;
    QComboBox* snapCombo_ = nullptr;
    QTimer* draftTimer_ = nullptr;
    QTimer* previewTimer_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    std::vector<GvtEvent> takeEvents_;
};

} // namespace gvt
