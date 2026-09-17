// Keep disabled crossfader input inaudible across UI/MIDI and editor preview;
// the bypass is runtime-only and must not leak into authored transitions.
#include "audio/AudioEngine.h"
#include "midi/MidiEngine.h"
#include "transitions/TransitionEngine.h"
#include "transitions/TransitionPlayback.h"
#include "ui/MixerWidget.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QSlider>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
using namespace gvt;
int failures=0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)
TrackDataPtr track(float value) {
    auto t=std::make_shared<TrackData>(); t->bpm=120; t->durationSec=8;
    t->pcm.assign(kSampleRate*8*2,value); return t;
}
float render(AudioEngine& e) {
    float buffer[512]; e.renderOffline(buffer,256); return buffer[500];
}
void default_bypass_keeps_centered_levels_despite_any_crossfader_origin()
{
    ControlBus bus; AudioEngine engine(&bus);
    CHECK(!engine.crossfaderEnabled.load());
    engine.deck(0).loadTrack(track(.2f)); engine.deck(1).loadTrack(track(.4f));
    engine.deck(0).play(); engine.deck(1).play();
    const float centered=render(engine);
    CHECK(std::fabs(centered-std::tanh(.6/std::sqrt(2.)))<1e-5);
    for (Origin origin : {Origin::Ui,Origin::Midi,Origin::Replay,Origin::System})
        for (double xf : {0.,.37,1.}) {
            bus.dispatch({kNoDeck,ControlId::Crossfader,xf},origin);
            CHECK(std::fabs(render(engine)-centered)<1e-6);
            CHECK(engine.crossfader.load()==.5f);
        }
    bus.dispatch({kNoDeck,ControlId::CrossfaderEnabled,1},Origin::Ui);
    CHECK(std::fabs(render(engine)-centered)<1e-6);
    bus.dispatch({kNoDeck,ControlId::Crossfader,0},Origin::Midi);
    CHECK(std::fabs(render(engine)-std::tanh(.2))<1e-5);
    bus.dispatch({kNoDeck,ControlId::Crossfader,1},Origin::Ui);
    CHECK(std::fabs(render(engine)-std::tanh(.4))<1e-5);
    bus.dispatch({kNoDeck,ControlId::CrossfaderEnabled,0},Origin::Ui);
    CHECK(std::fabs(render(engine)-centered)<1e-6);
    CHECK(engine.crossfader.load()==.5f);
    bus.dispatch({0,ControlId::Fader,0},Origin::Ui);
    CHECK(render(engine)<centered); // Channel faders remain effective.

    engine.deck(0).seekSec(1.0); engine.deck(1).seekSec(1.0);
    TransitionRecorder recorder(&bus,&engine); recorder.start(0);
    bus.dispatch({kNoDeck,ControlId::CrossfaderEnabled,1},Origin::Ui);
    bus.dispatch({kNoDeck,ControlId::Crossfader,1},Origin::Ui);
    bus.dispatch({0,ControlId::Fader,.7},Origin::Ui);
    const auto recorded=recorder.finish();
    CHECK(!recorded.initialCrossfaderPresent);
    CHECK(std::none_of(recorded.events.begin(),recorded.events.end(),[](const auto& e){
        return e.control==ControlId::Crossfader || e.control==ControlId::CrossfaderEnabled;
    }));

    ControlBus previewBus; AudioEngine preview(&previewBus);
    copyTransitionPlaybackContext(engine,1,previewBus,preview);
    CHECK(preview.crossfaderEnabled.load()); CHECK(preview.crossfader.load()==0);
    bus.dispatch({kNoDeck,ControlId::CrossfaderEnabled,0},Origin::Ui);
    copyTransitionPlaybackContext(engine,0,previewBus,preview);
    CHECK(!preview.crossfaderEnabled.load()); CHECK(preview.crossfader.load()==.5f);
}
void the_checkbox_is_compact_default_off_and_pickup_remains_control_local(QApplication& app)
{
    ControlBus bus; AudioEngine engine(&bus); MidiEngine midi(&bus,&engine);
    MixerWidget mixer(&bus); mixer.resize(340,280); mixer.show(); app.processEvents();
    auto* check=mixer.findChild<QCheckBox*>("disableCrossfader");
    auto* slider=qobject_cast<QSlider*>(mixer.controlWidget(kNoDeck,ControlId::Crossfader));
    CHECK(check && slider); if(!check || !slider) return;
    CHECK(check->isChecked()); CHECK(!slider->isEnabled());
    CHECK(mixer.width()<=340); CHECK(check->isVisible());
    CHECK(mixer.rect().contains(QRect(check->mapTo(&mixer,QPoint(0,0)),check->size())));
    CHECK(midi.hardwareControlStates().size()==16);
    check->click();
    CHECK(engine.crossfaderEnabled.load()); CHECK(slider->isEnabled());
    CHECK(midi.hardwareControlStates().size()==17);
    const auto pending=midi.pendingTakeovers();
    CHECK(pending.size()==1 && pending.front().control==ControlId::Crossfader);
    CHECK(!pending.empty() && pending.front().targetValue==.5);
    slider->setValue(200); CHECK(std::fabs(engine.crossfader.load()-.2)<1e-6);
    midi.setHardwareInputFrozen(true); midi.setHardwareInputFrozen(false);
    CHECK(midi.pendingTakeovers().size()==17);
    check->click();
    CHECK(!engine.crossfaderEnabled.load()); CHECK(!slider->isEnabled());
    CHECK(slider->value()==500); CHECK(midi.pendingTakeovers().size()==16);
    const auto remaining=midi.pendingTakeovers();
    CHECK(std::none_of(remaining.begin(),remaining.end(),[](const auto& s){return s.control==ControlId::Crossfader;}));
    bus.dispatch({kNoDeck,ControlId::Crossfader,1},Origin::Midi);
    CHECK(slider->value()==500);
    if (const auto path=qgetenv("GRAVITINO_MIXER_QA_IMAGE"); !path.isEmpty())
        CHECK(mixer.grab().save(QString::fromUtf8(path)));
}
}
int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen"); QApplication app(argc,argv);
    app.setStyleSheet(gvt::appStyleSheet());
    default_bypass_keeps_centered_levels_despite_any_crossfader_origin();
    the_checkbox_is_compact_default_off_and_pickup_remains_control_local(app);
    return failures ? 1 : 0;
}
