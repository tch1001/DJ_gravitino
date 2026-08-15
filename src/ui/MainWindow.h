#pragma once
#include <QMainWindow>
#include "../audio/AudioEngine.h"
#include "../control/ControlBus.h"
#include "../library/TrackLibrary.h"
#include "../midi/MidiEngine.h"
#include "../transitions/TransitionEngine.h"

class QLabel;

namespace gvt {

class DeckWidget;
class DetailWaveformView;
class MixerWidget;
class LibraryWidget;
class TransitionPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ControlBus* bus, AudioEngine* engine, TrackLibrary* library,
               TransitionStore* store, TransitionRecorder* recorder,
               TransitionPlayer* player, MidiEngine* midi,
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

private:
    ControlBus* bus_;
    AudioEngine* engine_;
    TrackLibrary* library_;
    TransitionStore* store_;
    MidiEngine* midi_;

    DeckWidget* deckA_ = nullptr;
    DeckWidget* deckB_ = nullptr;
    DetailWaveformView* detailWave_ = nullptr;
    MixerWidget* mixer_ = nullptr;
    LibraryWidget* libraryWidget_ = nullptr;
    TransitionPanel* transitionPanel_ = nullptr;
    QLabel* midiLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;
};

} // namespace gvt
