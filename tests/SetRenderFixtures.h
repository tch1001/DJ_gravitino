// Synthetic audio/recipes for set-render tests; never reads/writes user music.
#pragma once
#include "analysis/AnalysisInternal.h"
#include "audio/RecordingWav.h"
#include "transitions/SetRenderer.h"
#include <QCryptographicHash>
#include <QFile>
#include <QUuid>
#include <cmath>

inline gvt::TrackDataPtr setFixtureTrack(const QString& dir,const QString& name,double frequency)
{
    auto t=std::make_shared<gvt::TrackData>();
    t->filePath=dir+'/'+name+".wav"; t->title=name; t->bpm=120; t->durationSec=12;
    std::vector<float> samples(12*gvt::kSampleRate*2);
    for(size_t i=0;i<samples.size()/2;++i) samples[i*2]=samples[i*2+1]=float(.2*std::sin(i*frequency*6.283185307179586/gvt::kSampleRate));
    gvt::RecordingWav file; QString error;
    if(!file.open(t->filePath,&error) || !file.write(samples.data(),12*gvt::kSampleRate,&error) ||
        !file.finish({{"title",name}},&error)) return {};
    if(!gvt::detail::decodeAudioStereo48k(t->filePath,t->pcm,&error)) return {};
    t->fingerprint=gvt::computeFingerprint(t->pcm.data(),t->frameCount());
    QFile input(t->filePath); if(!input.open(QIODevice::ReadOnly)) return {};
    t->assetSha256=QString::fromLatin1(QCryptographicHash::hash(input.readAll(),QCryptographicHash::Sha256).toHex());
    t->hotCues[0]=.25; // must remain intact throughout all exports
    return t;
}
inline gvt::GvtFile setFixtureTransition(const gvt::TrackDataPtr& from,const gvt::TrackDataPtr& to,double bpm=120)
{
    gvt::GvtFile f; f.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    f.sourceFormat=gvt::TransitionSourceFormat::PortableYaml;
    f.name=from->title+" to "+to->title; f.masterBpm=bpm; f.endBeat=2.25; f.anchorFromBeat=8; f.anchorToBeat=2;
    const auto ref=[](const auto& t) {
        gvt::GvtTrackRef r; r.title=t->title; r.bpm=t->bpm; r.durationSec=t->durationSec;
        r.durationBeats=24; r.fingerprint=t->fingerprint;
        r.fingerprints.push_back({"gvfp1",t->fingerprint,{}}); return r;
    };
    f.from=ref(from); f.to=ref(to); f.initialComplete=true;
    f.initialFrom.captured=f.initialTo.captured=true; f.initialFrom.playing=true;
    f.initialTo.fader=0; f.initialTo.positionBeat=2;
    f.events={{.25,gvt::Role::ToDeck,gvt::ControlId::Play,1,gvt::Curve::Step},
              {1.75,gvt::Role::ToDeck,gvt::ControlId::Fader,1,gvt::Curve::Linear},
              {2.25,gvt::Role::FromDeck,gvt::ControlId::Fader,0,gvt::Curve::Linear}};
    return f;
}
inline gvt::SetRenderRequest setFixtureRequest(const QString& directory)
{
    const auto a=setFixtureTrack(directory,"Fixture A",220),b=setFixtureTrack(directory,"Fixture B",330),c=setFixtureTrack(directory,"Fixture C",440);
    if(!a || !b || !c) return {};
    gvt::SetRenderRequest r; r.title="Synthetic set"; r.catalogPath=directory+"/catalog.json"; r.stemsRoot=directory+"/stems";
    r.assets={gvt::setAssetProfile(*a),gvt::setAssetProfile(*b),gvt::setAssetProfile(*c)};
    r.transitions={setFixtureTransition(a,b),setFixtureTransition(b,c,132)}; r.keyLock=false; return r;
}
