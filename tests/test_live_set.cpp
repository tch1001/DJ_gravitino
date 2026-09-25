// Protect the public program bus from preparation and prove streamed sets use
// the export scheduler, with safe append-only changes and bounded buffering.
#include "SetRenderFixtures.h"
#include "audio/AudioDeviceTestAccess.h"
#include "transitions/LiveSetSession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

namespace {
int failures=0;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(0)
using namespace gvt;
struct Constant : AudioPreviewSource {
    float value;
    explicit Constant(float v):value(v){}
    void read(float* out,int frames) noexcept override { std::fill_n(out,frames*2,value); }
};
void preparation_and_editor_audio_never_reach_the_live_program() {
    ControlBus bus; AudioEngine engine(&bus); Constant program(.125f),preview(.75f);
    QString error;
    CHECK(!engine.acquireLiveProgram(&program,&error));
    detail::AudioDeviceTestAccess::setOfflineHeadphones(engine,true);
    CHECK(engine.acquireLiveProgram(&program,&error));
    CHECK(!engine.acquireLiveProgram(&preview,&error));
    auto song=std::make_shared<TrackData>(); song->durationSec=2; song->bpm=120;
    song->pcm.resize(kSampleRate*4,.2f); engine.deck(0).loadTrack(song);
    bus.dispatch({0,ControlId::Play,1},Origin::Midi);
    float channels[1024]{};
    engine.renderOfflineFourChannel(channels,256);
    CHECK(engine.deck(0).positionSec()>0);
    CHECK(channels[2]>0 && channels[2]!=.125f);
    for(int i=0;i<256;++i) CHECK(channels[4*i]==.125f && channels[4*i+1]==.125f);
    for(auto id:{ControlId::Fader,ControlId::EqLow,ControlId::FxWet,ControlId::StemMelody,ControlId::Crossfader})
        bus.dispatch({0,id,0},Origin::Midi);
    engine.renderOfflineFourChannel(channels,256);
    for(int i=0;i<256;++i) CHECK(channels[4*i]==.125f);
    CHECK(!engine.releaseLiveProgram(&program,&error)); // a playing prep deck cannot leak
    CHECK(engine.acquireExclusivePreview(&preview,&error));
    const auto position=engine.deck(0).positionSec();
    engine.renderOfflineFourChannel(channels,256);
    for(int i=0;i<256;++i) CHECK(channels[4*i]==.125f && channels[4*i+2]==.75f);
    CHECK(engine.deck(0).positionSec()==position);
    CHECK(!engine.releaseLiveProgram(&program,&error));
    detail::AudioDeviceTestAccess::setOfflineHeadphones(engine,false); // unplug
    float stereo[512]{};
    engine.renderOffline(stereo,256);
    for(float value:stereo) CHECK(value==.125f); // never fall back to MASTER
    engine.releaseExclusivePreview(&preview);
    bus.dispatch({0,ControlId::Stop,1},Origin::Midi);
    CHECK(engine.releaseLiveProgram(&program,&error)); CHECK(!engine.liveProgramActive());
    CHECK(!engine.acquireLiveProgram(&program,&error));
}

void stream_audio_matches_export_and_append_preserves_the_accepted_prefix(const SetRenderRequest& request,const QString& root) {
    std::vector<float> streamed;
    SetStreamHooks hooks;
    hooks.write=[&](const float* p,int frames){streamed.insert(streamed.end(),p,p+frames*2);return true;};
    CHECK(streamTransitionSet(request,hooks).completed);
    const auto disk=renderTransitionSet(request,root+"/parity.wav"); CHECK(disk.completed);
    std::vector<float> decoded; QString error;
    CHECK(detail::decodeAudioStereo48k(disk.outputPath,decoded,&error));
    CHECK(std::abs(qint64(decoded.size())-qint64(streamed.size()))<=2);
    double difference=0;
    for(size_t i=0;i<std::min(decoded.size(),streamed.size());++i)
        difference=std::max(difference,std::fabs(double(decoded[i]-streamed[i])));
    CHECK(difference<.00007);
    auto shortSet=request; shortSet.transitions.resize(1);
    for(int scenario=0;scenario<3;++scenario) {
        bool tail=false,sent=false,accepted=false; QString message;
        int transitions=0; qint64 frames=0,tailFrame=0;
        hooks.write=[&](const float*,int n){frames+=n;return true;};
        hooks.section=[&](qint64 frame,const QString& name,int) {
            if(name.startsWith("Transition ")) ++transitions;
            if(name.startsWith("Final song:") && !tail) { tail=true; tailFrame=frame; }
        };
        hooks.takeAppend=[&]() -> std::optional<SetRenderRequest> {
            if(!tail || sent || (scenario==2 && frames<tailFrame+3*kSampleRate)) return {};
            sent=true; auto update=request;
            if(scenario==1) update.transitions[0].name="Edited accepted recipe";
            return update;
        };
        hooks.appendResult=[&](bool ok,const QString& why){accepted=ok;message=why;};
        const auto result=streamTransitionSet(shortSet,hooks);
        CHECK(result.completed); CHECK(sent); CHECK(accepted==(scenario==0));
        CHECK(transitions==(scenario==0 ? 2 : 1));
        if(scenario==1) CHECK(message.contains("changed or reordered"));
        if(scenario==2) CHECK(message.contains("Too late"));
    }
}
void realtime_ring_pauses_and_finishes_without_returning_prep_to_speakers(const SetRenderRequest& request) {
    ControlBus bus; AudioEngine engine(&bus);
    detail::AudioDeviceTestAccess::setOfflineHeadphones(engine,true);
    LiveSetSession session(&engine); QString error;
    CHECK(session.start(request,&error)); CHECK(session.protectedRouting());
    session.pause(true); float out[2048]{};
    engine.renderOffline(out,1024); CHECK(session.elapsedSeconds()==0);
    for(float v:out) CHECK(v==0);
    session.pause(false);
    QElapsedTimer limit; limit.start(); double squares=0;
    while(session.running() && limit.elapsed()<15000) {
        QCoreApplication::processEvents();
        engine.renderOffline(out,1024);
        for(float v:out) squares+=v*v;
        QThread::msleep(1);
    }
    CHECK(!session.running()); CHECK(squares>1); CHECK(session.elapsedSeconds()>10);
    CHECK(session.protectedRouting()); // EOF retains the isolation lease
    engine.renderOffline(out,1024); for(float v:out) CHECK(v==0);
    CHECK(session.leave(&error)); CHECK(!session.protectedRouting());
    // A slow-clock version fills the ring without needing long fixture files.
    auto longSet=request;
    for(auto& f:longSet.transitions) {
        f.masterBpm=30;
        f.initialFrom.tempoRatio=.25; f.initialTo.tempoRatio=.25;
    }
    CHECK(session.start(longSet,&error));
    limit.restart();
    while(session.bufferedSeconds()<29.99 && limit.elapsed()<5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    CHECK(session.bufferedSeconds()>29.9 && session.bufferedSeconds()<=30.0);
    session.stop();
    limit.restart();
    while(!session.leave(&error) && limit.elapsed()<5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    CHECK(!session.protectedRouting());
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTemporaryDir dir;
    qputenv("GRAVITINO_CACHE_DIR",dir.filePath("cache").toUtf8());
    qputenv("GRAVITINO_CATALOG_PATH",dir.filePath("catalog.json").toUtf8());
    const auto request=setFixtureRequest(dir.path()); CHECK(request.assets.size()==3);
    preparation_and_editor_audio_never_reach_the_live_program();
    stream_audio_matches_export_and_append_preserves_the_accepted_prefix(request,dir.path());
    realtime_ring_pauses_and_finishes_without_returning_prep_to_speakers(request);
    std::printf("Live queue: %d failures\n",failures); return failures ? 1 : 0;
}
