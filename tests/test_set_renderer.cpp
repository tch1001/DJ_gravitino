// Guard set continuity, tempo integration, source safety and portable WAV data.
#include "SetRenderFixtures.h"
#include "transitions/TransitionGraph.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

namespace {
int failures=0;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(0)
QByteArray audio(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {};
    const auto h=f.read(44); if(h.size()!=44) return {};
    return f.read(qFromLittleEndian<quint32>(h.constData()+40));
}
double audioRms(const QByteArray& pcm,qint64 from,qint64 to) {
    if(from<0 || to<=from || to*4>pcm.size()) return 0;
    double squares=0;
    for(qint64 frame=from;frame<to;++frame) {
        const double sample=qFromLittleEndian<qint16>(pcm.constData()+frame*4)/32768.0;
        squares+=sample*sample;
    }
    return std::sqrt(squares/(to-from));
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv); QTemporaryDir dir; CHECK(dir.isValid());
    using namespace gvt;
    auto request=setFixtureRequest(dir.path()); CHECK(request.assets.size()==3);
    if(request.assets.size()!=3) return 1;
    CHECK(setTempoRampSeconds(8,120,132)==960.0/252.0);
    CHECK(setTempoRampBpm(5,10,120,132)==126);
    CHECK(setTempoRampBpm(15,10,132,120)==120);
    const SetEqValues eqFrom{0,1,.2},eqTo{1,.5,.8};
    CHECK(setEqRampValues(-1,10,eqFrom,eqTo)==eqFrom);
    const auto eqHalf=setEqRampValues(5,10,eqFrom,eqTo);
    CHECK(std::fabs(eqHalf[0]-.5)<1e-12 && std::fabs(eqHalf[1]-.75)<1e-12 && std::fabs(eqHalf[2]-.5)<1e-12);
    CHECK(setEqRampValues(11,10,eqFrom,eqTo)==eqTo);
    CHECK(setEqRampValues(0,0,eqFrom,eqTo)==eqTo);
    QStringList errors;
    CHECK(transitionsForSetSongs({"Fixture A","Fixture B","Fixture C"},request.transitions,&errors).size()==2);
    CHECK(errors.isEmpty());
    auto duplicates=request.transitions; duplicates.push_back(duplicates.front());
    CHECK(transitionsForSetSongs({"Fixture A","Fixture B"},duplicates,&errors).empty()); CHECK(errors.join(' ').contains("ambiguous"));
    errors.clear(); CHECK(transitionsForSetSongs({"Fixture C","Fixture A"},request.transitions,&errors).empty());
    CHECK(errors.join(' ').contains("no .transition"));
    const auto plan=planTransitionSet(request); CHECK(plan.valid());
    if(!plan.valid()) std::fprintf(stderr,"%s\n",qPrintable(plan.errors.join('\n')));
    const auto output=dir.filePath("mix.wav");
    const auto result=renderTransitionSet(request,output); CHECK(result.completed);
    if(!result.completed) { std::fprintf(stderr,"%s\n",qPrintable(result.error)); return 1; }
    const auto manifest=readRecordingManifest(output);
    CHECK(manifest.value("format")=="gravitino.recording"); CHECK(manifest.value("sample_rate")==48000);
    CHECK(manifest.value("channels")==2); CHECK(manifest.value("bits_per_sample")==16);
    const auto transitions=manifest.value("transitions").toArray(); CHECK(transitions.size()==2);
    CHECK(transitions[0].toObject().value("transition_yaml")==transitionSerialize(request.transitions[0]));
    CHECK(transitions[1].toObject().value("transition_yaml")==transitionSerialize(request.transitions[1]));
    const auto ramps=manifest.value("tempo_ramps").toArray(); CHECK(ramps.size()==3);
    const auto middle=ramps[1].toObject(); CHECK(std::fabs(middle.value("from_bpm").toDouble()-120)<1e-6);
    CHECK(middle.value("to_bpm")==132); CHECK(middle.value("curve")=="linear-time");
    const auto expected=setTempoRampSeconds(middle.value("to_track_beat").toDouble()-middle.value("from_track_beat").toDouble(),120,132);
    CHECK(std::fabs((middle.value("end_frame").toDouble()-middle.value("start_frame").toDouble())/48000-expected)<=1.0/48000);
    CHECK(transitions[0].toObject().value("start_frame")==192000); // exact outgoing song beat 8
    CHECK(audio(output).size()==qint64(manifest.value("frames").toDouble())*4);
    CHECK(manifest.value("rms").toDouble()>.01); CHECK(manifest.value("peak").toDouble()<1);
    CHECK(std::fabs(manifest.value("duration_seconds").toDouble()-plan.estimatedSeconds)<.01);
    CHECK(request.assets[0]->hotCues[0]==.25);

    // Tone notes reach the exact same master/WAV path and travel in its
    // embedded recipe. The original source files and request remain untouched.
    auto toneRequest=request;
    TonePlayPattern pattern;
    pattern.sourceStartBeat=1;pattern.sourceEndBeat=2;
    pattern.notes={{.125,.5,60,.8},{1,.5,67,.8}};
    toneRequest.transitions[0].tonePlay=pattern;
    const auto toneResult=renderTransitionSet(toneRequest,dir.filePath("tone-play.wav"));
    CHECK(toneResult.completed);
    CHECK(audio(toneResult.outputPath)!=audio(output));
    CHECK(audio(toneResult.outputPath).size()==audio(output).size());
    const auto toneYaml=readRecordingManifest(toneResult.outputPath).value("transitions").toArray()[0]
        .toObject().value("transition_yaml").toString();
    GvtFile embedded;QString toneError;
    CHECK(transitionParse(toneYaml,embedded,&toneError));
    CHECK(embedded.tonePlay && embedded.tonePlay->notes.size()==2);
    CHECK(embedded.tonePlay && !embedded.tonePlay->replaceOutgoing);
    CHECK(!gvtSaveFile(toneRequest.transitions[0],dir.filePath("unsupported.gvt"),&toneError));
    CHECK(!QFileInfo::exists(dir.filePath("unsupported.gvt")));
    toneRequest.transitions[0].tonePlay->sourceStartBeat=-100;
    CHECK(!planTransitionSet(toneRequest).valid());

    // Compatibility crossfader automation changes embedded data, never sound.
    auto compatibility=request;
    for(auto& f:compatibility.transitions) {
        f.initialCrossfaderPresent=true; f.initialCrossfader=.99;
        f.events.insert(f.events.begin(),{0,Role::Mixer,ControlId::Crossfader,0,Curve::Step});
    }
    auto same=renderTransitionSet(compatibility,dir.filePath("inert-crossfader.wav")); CHECK(same.completed);
    CHECK(audio(output)==audio(same.outputPath));
    auto overwrite=renderTransitionSet(request,output); CHECK(!overwrite.completed); CHECK(overwrite.error.contains("already exists"));
    CHECK(audio(output)==audio(same.outputPath));
    int checks=0;
    auto cancelled=renderTransitionSet(request,dir.filePath("cancelled.wav"),{},[&]{return ++checks>20;});
    CHECK(cancelled.cancelled); CHECK(!QFileInfo::exists(cancelled.outputPath));

    auto bad=request; bad.transitions[1].anchorFromBeat=1;
    CHECK(!planTransitionSet(bad).valid());
    bad=request; bad.transitions[1].from=bad.transitions[0].from; CHECK(!planTransitionSet(bad).valid());
    bad=request; bad.assets[0]=setAssetProfile(*bad.assets[0]); bad.assets[0]->filePath=dir.filePath("missing.wav");
    CHECK(!planTransitionSet(bad).valid());
    bad=request; bad.transitions[0].requirements={"unsupported.test.v1"}; CHECK(!planTransitionSet(bad).valid());
    bad=request; bad.transitions[0].events.push_back({2.25,Role::ToDeck,ControlId::StemBass,0,Curve::Step});
    CHECK(planTransitionSet(bad).errors.join(' ').contains("Prepare stems"));
    bad=request; bad.assets[0]=setAssetProfile(*bad.assets[0]); bad.assets[0]->assetSha256="stale";
    auto stale=renderTransitionSet(bad,dir.filePath("stale.wav")); CHECK(!stale.completed); CHECK(!QFileInfo::exists(stale.outputPath));
    bad=request; bad.assets.push_back(setAssetProfile(*bad.assets[0]));
    CHECK(!planTransitionSet(bad).valid()); // never silently pick among compatible assets

    // A temporary saved loop must exit before handing the song to the next edge.
    bad=request; TransitionSavedLoop loop; loop.id="intro-loop"; loop.role=Role::ToDeck;
    loop.startTrackBeat=2; loop.endTrackBeat=3; bad.transitions[0].transitionLoops={loop};
    GvtEvent press{0,Role::ToDeck,ControlId::SavedLoop1,1,Curve::Step}; press.loopId=loop.id;
    bad.transitions[0].events.insert(bad.transitions[0].events.begin(),press);
    CHECK(!planTransitionSet(bad).valid());
    bad.transitions[0].events.insert(bad.transitions[0].events.end()-1,{2.0,Role::ToDeck,ControlId::LoopExit,1,Curve::Step});
    CHECK(planTransitionSet(bad).valid());
    auto looped=renderTransitionSet(bad,dir.filePath("looped.wav")); CHECK(looped.completed);
    CHECK(looped.manifest.value("duration_seconds").toDouble()>manifest.value("duration_seconds").toDouble());

    // The real rendered audio must not lose a boosted band at the next edge.
    // This is the Hotel Room Service regression, tested for LOW/MID/HIGH,
    // with both boosts being removed and cuts being restored.
    const ControlId eqControls[]{ControlId::EqLow,ControlId::EqMid,ControlId::EqHigh};
    const char* eqNames[]{"low","mid","high"};
    const double frequencies[]{100,1000,8000};
    for(int band=0;band<3;++band) {
        const auto tone=setFixtureTrack(dir.path(),QString("EQ tone %1").arg(band),frequencies[band]);
        CHECK(bool(tone)); if(!tone) continue;
        auto eqRequest=request; eqRequest.assets[1]=setAssetProfile(*tone);
        eqRequest.transitions={setFixtureTransition(eqRequest.assets[0],tone),setFixtureTransition(tone,eqRequest.assets[2])};
        // Hold the outgoing fader initially, so this comparison isolates EQ
        // instead of measuring the fixture's authored fade from beat zero.
        eqRequest.transitions[1].events.back().curve=Curve::Step;
        const double exitValue=band==2 ? 0.2 : 1.0;
        eqRequest.transitions[0].events.push_back({2.25,Role::ToDeck,eqControls[band],exitValue,Curve::Step});
        const auto originalFirst=transitionSerialize(eqRequest.transitions[0]);
        const auto originalSecond=transitionSerialize(eqRequest.transitions[1]);
        auto eqResult=renderTransitionSet(eqRequest,dir.filePath(QString("eq-%1.wav").arg(band)));
        CHECK(eqResult.completed); if(!eqResult.completed) { std::fprintf(stderr,"%s\n",qPrintable(eqResult.error)); continue; }
        const auto changes=eqResult.manifest.value("eq_ramps").toArray(); CHECK(changes.size()==3);
        const auto bridge=changes[1].toObject();
        CHECK(std::fabs(bridge.value("from_values").toObject().value(eqNames[band]).toDouble()-exitValue)<1e-6);
        CHECK(bridge.value("to_values").toObject().value(eqNames[band])==.5);
        CHECK(bridge.value("curve")=="linear-time"); CHECK(bridge.value("domain")=="normalized-knob");
        const auto eqTransitions=eqResult.manifest.value("transitions").toArray();
        const qint64 start=qint64(bridge.value("start_frame").toDouble());
        const qint64 end=qint64(bridge.value("end_frame").toDouble());
        CHECK(start==eqTransitions[0].toObject().value("end_frame").toDouble());
        CHECK(end==eqTransitions[1].toObject().value("start_frame").toDouble());
        const auto pcm=audio(eqResult.outputPath);
        const double before=audioRms(pcm,end-2880,end-480),after=audioRms(pcm,end+480,end+2880);
        const double boundaryDb=20*std::log10(before/after);
        std::printf("EQ %s: bridge %.6fs, before %.8f after %.8f, boundary %.4f dB\n",eqNames[band],(end-start)/48000.0,before,after,boundaryDb);
        CHECK(std::fabs(boundaryDb)<.4); // old implementation jumped by several dB
        const double early=audioRms(pcm,start+480,start+2880);
        CHECK(exitValue>.5 ? early>before*1.1 : early<before*.9);
        CHECK(eqTransitions[0].toObject().value("transition_yaml")==originalFirst);
        CHECK(eqTransitions[1].toObject().value("transition_yaml")==originalSecond);
        CHECK(transitionSerialize(eqRequest.transitions[0])==originalFirst);
        CHECK(transitionSerialize(eqRequest.transitions[1])==originalSecond);
        CHECK(std::fabs(eqResult.manifest.value("duration_seconds").toDouble()-planTransitionSet(eqRequest).estimatedSeconds)<.01);
        // No room for smoothing must fail, not silently snap an audible knob.
        eqRequest.transitions[1].anchorFromBeat=eqResult.manifest.value("tempo_ramps").toArray()[1].toObject().value("from_track_beat").toDouble();
        const auto noGap=renderTransitionSet(eqRequest,dir.filePath(QString("eq-no-gap-%1.wav").arg(band)));
        CHECK(!noGap.completed); CHECK(noGap.error.contains("smooth EQ ramp"));
        CHECK(!QFileInfo::exists(noGap.outputPath));
    }

    // Local set documents resolve both generations and retain auto asset choice.
    QString error;
    CHECK(transitionSaveFile(request.transitions[0],dir.filePath("first.transition"),&error));
    CHECK(gvtSaveFile(request.transitions[1],dir.filePath("second.gvt"),&error));
    const auto writePlan=[&](QJsonObject document) {
        QFile file(dir.filePath("test.set.json"));
        if(!file.open(QIODevice::WriteOnly)) return false;
        const auto bytes=QJsonDocument(document).toJson(); return file.write(bytes)==bytes.size();
    };
    QJsonObject document{{"format","gravitino.set"},{"version",1},
        {"transition_paths",QJsonArray{"first.transition","second.gvt"}},
        {"asset_paths",QJsonArray{"","",""}}};
    CHECK(writePlan(document)); auto loaded=request;
    CHECK(readSetRequest(dir.filePath("test.set.json"),loaded,&error));
    CHECK(loaded.transitions.size()==2); CHECK(loaded.transitions[1].sourceFormat==TransitionSourceFormat::LegacyGvt);
    CHECK(loaded.assetPaths[0].isEmpty()); CHECK(planTransitionSet(loaded).valid());
    CHECK(renderTransitionSet(loaded,dir.filePath("mixed-formats.wav")).completed);
    auto changed=request.transitions[0]; changed.name="Edited elsewhere";
    CHECK(transitionSaveFile(changed,dir.filePath("first.transition"),&error));
    CHECK(renderTransitionSet(loaded,dir.filePath("stale-recipe.wav")).error.contains("source transition changed"));
    document["transition_paths"]=QJsonArray{}; CHECK(writePlan(document));
    CHECK(!readSetRequest(dir.filePath("test.set.json"),loaded,&error));
    document["transition_paths"]=QJsonArray{3}; CHECK(writePlan(document));
    CHECK(!readSetRequest(dir.filePath("test.set.json"),loaded,&error));
    std::printf("Set renderer: %d failures\n",failures); return failures ? 1 : 0;
}
