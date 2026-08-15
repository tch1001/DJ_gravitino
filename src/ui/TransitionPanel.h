#pragma once
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
class QTimer;

namespace gvt {

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

public slots:
    void refreshMatches();  // call on trackLoaded / store changed

private slots:
    void onRec();
    void onStopSave();
    void onPerform();
    void onPrime();
    void startReplay(gvt::PlayerMode mode, bool prime);
    void onAbort();
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

    ControlBus* bus_;
    AudioEngine* engine_;
    TransitionStore* store_;
    TransitionRecorder* recorder_;
    TransitionPlayer* player_;

    std::vector<Match> matches_;
    QListWidget* list_ = nullptr;
    QPushButton* recBtn_ = nullptr;
    QPushButton* stopSaveBtn_ = nullptr;
    QPushButton* performBtn_ = nullptr;
    QPushButton* primeBtn_ = nullptr;
    QPushButton* tutorialBtn_ = nullptr;
    QPushButton* abortBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* recIndicator_ = nullptr;
    QLabel* banner_ = nullptr;     // translucent overlay on the main window
    QTimer* bannerTimer_ = nullptr;
};

} // namespace gvt
