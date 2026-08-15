#pragma once
#include <QMainWindow>
#include "../audio/AudioEngine.h"
#include "../control/ControlBus.h"
#include "../library/TrackLibrary.h"
#include "../midi/MidiEngine.h"
#include "../transitions/TransitionEngine.h"

class QLabel;
class QPushButton;
class QTimer;

namespace gvt {

class History;
class MasterRecorder;
class StemSeparator;
class DeckWidget;
class DetailWaveformView;
class MixerWidget;
class LibraryWidget;
class TransitionPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `rec` may be null (e.g. tests / older callers): the master-record
    // button is then hidden entirely. Wiring rec into the engine's master
    // tap is main.cpp's (the orchestrator's) job.
    MainWindow(ControlBus* bus, AudioEngine* engine, TrackLibrary* library,
               TransitionStore* store, TransitionRecorder* recorder,
               TransitionPlayer* player, MidiEngine* midi,
               MasterRecorder* rec = nullptr, StemSeparator* stems = nullptr,
               QWidget* parent = nullptr);
    // Dev hook (--autoload): a track was loaded onto `deck` outside the
    // library widget; refresh deck display + transition matches.
    void notifyTrackLoaded(int deck);

public slots:
    // Relays the transition entry point (sec < 0 = none) to the deck
    // overview waveform and the center detail waveforms. Called by the
    // orchestrator's transition entry-point logic.
    void setTransitionEntryMarker(int deck, double sec);

private slots:
    void openMusicFolder();
    void openTransitionsFolder();
    void about();
    void onMidiConnection(bool connected, const QString& name);
    void onRecClicked();
    void onRecordingChanged(bool active, const QString& path);
    void onStemsRequested(int deck);

private:
    ControlBus* bus_;
    AudioEngine* engine_;
    TrackLibrary* library_;
    TransitionStore* store_;
    MidiEngine* midi_;
    History* history_ = nullptr; // owned (QObject child); load log + panel

    DeckWidget* deckA_ = nullptr;
    DeckWidget* deckB_ = nullptr;
    DetailWaveformView* detailWave_ = nullptr;
    MixerWidget* mixer_ = nullptr;
    LibraryWidget* libraryWidget_ = nullptr;
    TransitionPanel* transitionPanel_ = nullptr;
    QLabel* midiLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;

    // Master recording (null rec_ = feature hidden).
    MasterRecorder* rec_ = nullptr;
    QPushButton* recBtn_ = nullptr;
    QTimer* recTimer_ = nullptr; // 1 s elapsed-time updates while recording

    // Stem separation (null stems_ = STEMS button stays inert).
    // Results are matched to decks by track fingerprint: a deck whose track
    // swapped while demucs ran simply doesn't match and the result is
    // dropped (the WAV cache keeps it for the next load).
    StemSeparator* stems_ = nullptr;
    DeckWidget* deckWidget(int i) { return i == 0 ? deckA_ : deckB_; }
};

} // namespace gvt
