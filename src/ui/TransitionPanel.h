#pragma once
#include <QList>
#include <QStringList>
#include <QWidget>
#include <vector>
#include "../audio/AudioEngine.h"
#include "../control/ControlBus.h"
#include "../library/TrackLibrary.h"
#include "../transitions/Transition.h"
#include "../transitions/TransitionEngine.h"

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

namespace gvt {

class Flx4TutorialWidget;

// Transition browser + record/perform controls.
// Left: .gvt files matching the currently loaded (A,B) pair (both orderings).
// Right: REC / STOP&SAVE / PERFORM / TUTORIAL / ABORT with progress + rec
// indicator, plus tutorial prompt banner and accuracy toasts.
class TransitionPanel : public QWidget {
    Q_OBJECT
public:
    TransitionPanel(ControlBus* bus, AudioEngine* engine,
                    TransitionStore* store, TransitionRecorder* recorder,
                    TransitionPlayer* player, QWidget* parent = nullptr);

signals:
    void statusMessage(const QString& msg, int timeoutMs);
    // Recorded entry point of the selected transition, for waveform markers:
    // deck = physical deck holding the [from] track, sec = position of the
    // anchor beat in that track (< 0 clears the marker).
    void entryMarkerChanged(int deck, double sec);
    // Labeled cue markers for the selected transition, already converted to
    // seconds for each physical deck's beatgrid.
    void cueMarkersChanged(int deck, const QList<double>& seconds,
                           const QStringList& labels);

public slots:
    void refreshMatches();  // call on trackLoaded / store changed

private slots:
    void onRec();
    void onStopSave();
    void onPerform();
    void onPrime();
    void startReplay(gvt::PlayerMode mode, bool prime);
    void onAbort();
    void onRename();
    void onDelete();
    void onLabelCue();
    void onApplySetup();
    void onProgress(double beatsIn, double beatsTotal);
    void onFinished(bool completed);
    void onEventCaptured(int count);
    void onTutorialPrompt(const gvt::GvtEvent& e, double beatsAhead);
    void onTutorialScored(const gvt::GvtEvent& e, double beatError,
                          double valueError);

private:
    struct Match {
        const GvtFile* file = nullptr;
        int fromDeck = 0;          // physical deck holding the [from] track
        MatchQuality quality = MatchQuality::None;
    };
    void showBanner(const QString& text, const QColor& color, int timeoutMs);
    int selectedMatch() const;     // index into matches_, -1 if none
    void announceEntryMarker();    // emit entryMarkerChanged for selection
    void updatePreview();
    void updateControls();
    void updateSetupStatus();
    void applyInitialSetup(const Match& match, bool announce,
                           bool prepareFromTransport = false,
                           bool prepareToTransport = false);
    bool setupMatches(const Match& match, QStringList* differences = nullptr) const;
    QStringList primeReadinessIssues(const Match& match) const;
    double expectedTempoRatio(const Match& match, bool fromRole) const;
    QString automaticCueLabel(const GvtEvent& event) const;
    QString cueLabelAt(const GvtFile& file, double beat) const;
    QStringList tutorialWarnings(const Match& match) const;
    QString tutorialWarningForEvent(const Match& match,
                                    const GvtEvent& event) const;
    bool tutorialEventCanActivate(const Match& match,
                                  const GvtEvent& event) const;
    ControlEvent tutorialPhysicalEvent(const GvtEvent& event) const;
    QString tutorialInstruction(const GvtEvent& event,
                                const ControlEvent& physical) const;
    QString tutorialDetail(const GvtEvent& event) const;
    void ensureTutorialOverlay();
    void layoutTutorialOverlay();
    void showNextTutorialPrompt();
    void closeTutorialOverlay();

    ControlBus* bus_;
    AudioEngine* engine_;
    TransitionStore* store_;
    TransitionRecorder* recorder_;
    TransitionPlayer* player_;

    std::vector<Match> matches_;
    QListWidget* list_ = nullptr;
    QTableWidget* preview_ = nullptr;
    QPushButton* recBtn_ = nullptr;
    QPushButton* stopSaveBtn_ = nullptr;
    QPushButton* performBtn_ = nullptr;
    QPushButton* primeBtn_ = nullptr;
    QPushButton* tutorialBtn_ = nullptr;
    QPushButton* abortBtn_ = nullptr;
    QPushButton* renameBtn_ = nullptr;
    QPushButton* deleteBtn_ = nullptr;
    QPushButton* labelCueBtn_ = nullptr;
    QPushButton* applySetupBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* recIndicator_ = nullptr;
    QLabel* setupLabel_ = nullptr;
    QLabel* banner_ = nullptr;     // translucent overlay on the main window
    QTimer* bannerTimer_ = nullptr;
    QTimer* stateTimer_ = nullptr;
    int capturedCount_ = 0;
    QString selectedPath_;
    Flx4TutorialWidget* tutorialOverlay_ = nullptr;
    std::vector<GvtEvent> tutorialPrompts_;
    int tutorialFromDeck_ = 0;
    double tutorialBeatsIn_ = 0.0;
    bool tutorialActive_ = false;
};

} // namespace gvt
