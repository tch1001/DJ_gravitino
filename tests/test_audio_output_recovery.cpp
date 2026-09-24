// Protect reconnection from stale started flags without touching real hardware.
#include "audio/AudioOutputRecovery.h"
#include "audio/AudioDeviceTestAccess.h"
#include <QCoreApplication>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)
using namespace gvt;
struct SilentPreview : AudioPreviewSource {
    void read(float* out, int frames) noexcept override { std::fill_n(out, frames * 2, 0.0f); }
};
void explicit_headphones_wait_for_the_same_output_while_default_follows_macos()
{
    QList<AudioOutputDevice> choices{{"Speakers",true},{"External Headphones",false},{"DDJ-FLX4",false}};
    CHECK(audioOutputIndex(choices,{})==0);
    CHECK(audioOutputIndex(choices,"External Headphones")==1);
    choices.removeAt(1);
    CHECK(audioOutputIndex(choices,"External Headphones")==-1);
    choices.append({"External Headphones",true}); choices[0].isDefault=false;
    CHECK(audioOutputIndex(choices,"External Headphones")==2);
    CHECK(audioOutputIndex(choices,{})==2);
    choices[2].isDefault=false; choices[1].isDefault=true;
    CHECK(audioOutputIndex(choices,{})==1); // FLX4 primary needs four channels.
}
void recovery_reopens_stale_or_changed_streams_but_leaves_healthy_and_offline_ones_alone()
{
    CHECK(audioOutputRecovery(true,true,true,false,true)==AudioOutputRecovery::Reopen);
    CHECK(audioOutputRecovery(true,true,true,true,false)==AudioOutputRecovery::Reopen);
    CHECK(audioOutputRecovery(true,true,false,false,true)==AudioOutputRecovery::Reopen);
    CHECK(audioOutputRecovery(true,true,true,true,true)==AudioOutputRecovery::None);
    CHECK(audioOutputRecovery(true,false,true,false,false)==AudioOutputRecovery::Close);
    CHECK(audioOutputRecovery(true,false,false,false,false)==AudioOutputRecovery::None);
    CHECK(audioOutputRecovery(false,true,false,false,false)==AudioOutputRecovery::None);
    AudioCallbackWatchdog watch;
    watch.reset(0,0);
    CHECK(!watch.stalled(0,2999)); CHECK(watch.stalled(0,3000));
    CHECK(!watch.stalled(256,3001)); CHECK(!watch.stalled(512,6001));
    CHECK(watch.stalled(512,9001));
    watch.reset(512,10000); CHECK(!watch.stalled(512,10001));
}
void real_backend_stop_and_notifications_reopen_without_resetting_decks()
{
    ControlBus bus; AudioEngine engine(&bus);
    CHECK(detail::AudioDeviceTestAccess::useNullBackend(engine));
    engine.refreshOutputDevices();
    CHECK(!detail::AudioDeviceTestAccess::backendActive(engine));
    QString error;
    CHECK(!engine.start("Missing headphones",&error));
    engine.refreshOutputDevices();
    CHECK(!detail::AudioDeviceTestAccess::backendActive(engine));
    CHECK(engine.outputDevicePreference()=="Missing headphones");
    CHECK(engine.outputDeviceName().isEmpty());
    auto track=std::make_shared<TrackData>();
    track->bpm=120; track->durationSec=8; track->pcm.resize(kSampleRate*8*2);
    engine.deck(0).loadTrack(track); engine.deck(0).seekSec(2.25);
    engine.deck(0).cuePointSec.store(1.75); engine.deck(0).eqLow.store(.37);
    CHECK(engine.start(&error));
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine));
    int routeChanges=0;
    QObject::connect(&engine,&AudioEngine::outputDeviceChanged,[&]{++routeChanges;});
    engine.refreshOutputDevices(); CHECK(routeChanges==0);
    detail::AudioDeviceTestAccess::stopBackend(engine);
    CHECK(!detail::AudioDeviceTestAccess::backendActive(engine));
    engine.refreshOutputDevices();
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine)); CHECK(routeChanges==1);
    CHECK(!engine.deck(0).playing.load());
    CHECK(std::fabs(engine.deck(0).positionSec()-2.25)<1e-6);
    CHECK(engine.deck(0).track()==track);
    CHECK(std::fabs(engine.deck(0).cuePointSec.load()-1.75)<1e-6);
    CHECK(std::fabs(engine.deck(0).eqLow.load()-.37)<1e-6);
    detail::AudioDeviceTestAccess::notifyInterruption(engine);
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine));
    engine.refreshOutputDevices(); CHECK(routeChanges==2);
    engine.deck(0).play();
    detail::AudioDeviceTestAccess::stopBackend(engine);
    CHECK(engine.start(engine.outputDevicePreference(),&error));
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine));
    CHECK(engine.deck(0).playing.load()); CHECK(engine.deck(0).positionSec()>=2.25);
    detail::AudioDeviceTestAccess::stopBackend(engine);
    CHECK(engine.switchOutputDevice(engine.outputDevicePreference(),&error));
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine));
    CHECK(!engine.switchOutputDevice("Missing headphones",&error));
    CHECK(engine.outputDevicePreference().isEmpty()); // Previous default retained.
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine));
    engine.deck(0).stop();
    SilentPreview preview;
    CHECK(engine.acquireExclusivePreview(&preview,&error));
    detail::AudioDeviceTestAccess::stopBackend(engine);
    // stop() is asynchronous to the callback. Let its in-flight block finish
    // before measuring the cursor that recovery must preserve.
    const double cursor = engine.deck(0).positionSec();
    engine.refreshOutputDevices();
    CHECK(detail::AudioDeviceTestAccess::backendActive(engine));
    CHECK(engine.exclusivePreviewActive());
    CHECK(std::fabs(engine.deck(0).positionSec()-cursor)<1e-9);
    engine.releaseExclusivePreview(&preview);
    engine.stopDevice(); engine.refreshOutputDevices();
    CHECK(!detail::AudioDeviceTestAccess::backendActive(engine));
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    explicit_headphones_wait_for_the_same_output_while_default_follows_macos();
    recovery_reopens_stale_or_changed_streams_but_leaves_healthy_and_offline_ones_alone();
    real_backend_stop_and_notifications_reopen_without_resetting_decks();
    return failures ? 1 : 0;
}
