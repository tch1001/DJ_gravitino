// Offline set orchestration. Musical actions always use ControlBus and the
// normal TransitionPlayer; only the clock and disk sink differ from live DJing.
#include "SetRenderer.h"
#include "TransitionFields.h"
#include "TransitionGraph.h"
#include "TransitionPlayback.h"
#include "TransitionPlayerExt.h"
#include "TransitionTransportTrace.h"
#include "../analysis/AnalysisInternal.h"
#include "../audio/RecordingWav.h"
#include "../library/TrackLibrary.h"
#include "../../third_party/miniaudio.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace gvt {
namespace {
QString defaultCatalog() { return qEnvironmentVariable("GRAVITINO_CATALOG_PATH",QDir::homePath()+"/.gravitino/catalog.json"); }
QString stemDirectory(const SetRenderRequest& r, const TrackData& t) {
    const QString root = r.stemsRoot.isEmpty() ? QDir::homePath() + "/.gravitino/stems" : r.stemsRoot;
    return root + '/' + t.fingerprint.section(':', -1);
}
bool needsStems(const GvtFile& f, Role role) {
    const auto& s = role == Role::FromDeck ? f.initialFrom : f.initialTo;
    if (f.initialComplete && s.captured &&
        (s.stemVocals < .999 || s.stemMelody < .999 || s.stemBass < .999 || s.stemDrums < .999)) return true;
    return std::any_of(f.events.begin(), f.events.end(), [role](const GvtEvent& e) {
        return e.role == role && (e.control == ControlId::StemVocals || e.control == ControlId::StemMelody ||
            e.control == ControlId::StemBass || e.control == ControlId::StemDrums);
    });
}
bool cachedStems(const SetRenderRequest& r, const TrackData& t) {
    if (t.fingerprint.isEmpty()) return false;
    for (const char* name : {"vocals", "other", "bass", "drums"})
        if (!QFileInfo::exists(stemDirectory(r, t) + '/' + name + ".wav")) return false;
    return true;
}
QString normalizedTitle(QString title) {
    title = title.toCaseFolded(); title.replace(QRegularExpression("[^\\p{L}\\p{N}]+"), " ");
    return title.simplified();
}
bool titleMatches(const QString& title, const QString& query) {
    return (" " + normalizedTitle(title) + " ").contains(" " + normalizedTitle(query) + " ");
}
bool matches(const GvtFile& f, bool outgoing, const TrackData& t, const SongCatalog& catalog) {
    const auto song = catalog.songIdForEndpoint(f.id, outgoing);
    if (!song.isEmpty()) return song == t.songId;
    return transitionTrackMatchReliable(f, outgoing ? f.from : f.to, t);
}
double finalIncomingBpm(const GvtFile& f) {
    double bpm = f.to.bpm * (f.initialTo.captured ? f.initialTo.tempoRatio : 1.0);
    for (const auto& e : f.events) {
        if (e.role == Role::ToDeck && e.control == ControlId::Tempo) bpm = f.to.bpm * e.value;
        if (e.role == Role::ToDeck && e.control == ControlId::TempoSync) bpm = f.masterBpm;
    }
    return bpm;
}
QJsonObject readJson(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 32 * 1024 * 1024) {
        if (error) *error = "Cannot read JSON file (32 MiB limit): " + path; return {};
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (!doc.isObject() || parse.error != QJsonParseError::NoError) {
        if (error) *error = "Invalid JSON: " + path; return {};
    }
    return doc.object();
}
TrackDataPtr decodeAsset(const TrackData& profile, QString* error) {
    auto track = setAssetProfile(profile);
    QFile file(profile.filePath);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) { *error = "Cannot hash audio: " + profile.filePath; return {}; }
    track->assetSha256 = QString::fromLatin1(hash.result().toHex());
    if (!profile.assetSha256.isEmpty() && track->assetSha256 != profile.assetSha256) {
        *error = "Audio changed since catalog analysis. Rescan it before exporting: " + profile.filePath;
        return {};
    }
    if (!detail::decodeAudioStereo48k(profile.filePath, track->pcm, error)) return {};
    track->durationSec = double(track->frameCount()) / kSampleRate;
    const auto fingerprint = computeFingerprint(track->pcm.data(), track->frameCount());
    if (!profile.fingerprint.isEmpty() && fingerprint != profile.fingerprint) {
        *error = "Decoded audio no longer matches the selected asset: " + profile.filePath; return {};
    }
    track->fingerprint = fingerprint;
    return track;
}
StemSetPtr decodeStems(const SetRenderRequest& r, const TrackData& t, QString* error) {
    auto stems = std::make_shared<StemSet>();
    std::vector<int16_t>* outputs[] = {&stems->vocals, &stems->melody, &stems->bass, &stems->drums};
    const char* names[] = {"vocals", "other", "bass", "drums"};
    for (int i = 0; i < 4; ++i) {
        const auto path = stemDirectory(r, t) + '/' + names[i] + ".wav";
        const auto config = ma_decoder_config_init(ma_format_s16, 2, kSampleRate);
        ma_decoder decoder;
        if (ma_decoder_init_file(path.toUtf8().constData(), &config, &decoder) != MA_SUCCESS) {
            *error = "Cannot decode cached stems: " + path; return {};
        }
        outputs[i]->assign(size_t(t.frameCount()) * 2, 0);
        ma_uint64 read = 0;
        const auto status = ma_decoder_read_pcm_frames(&decoder, outputs[i]->data(), t.frameCount(), &read);
        ma_decoder_uninit(&decoder);
        if ((status != MA_SUCCESS && status != MA_AT_END) || read == 0) {
            *error = "Invalid/empty cached stem: " + path; return {};
        }
    }
    return stems;
}
}

TrackDataPtr setAssetProfile(const TrackData& s)
{
    auto p = std::make_shared<TrackData>();
    p->filePath=s.filePath; p->title=s.title; p->artist=s.artist; p->album=s.album;
    p->isrc=s.isrc; p->musicBrainzRecording=s.musicBrainzRecording;
    p->durationSec=s.durationSec; p->audibleDurationSec=s.audibleDurationSec;
    p->bpm=s.bpm; p->firstBeatSec=s.firstBeatSec; p->canonicalBeatOffset=s.canonicalBeatOffset;
    p->fingerprint=s.fingerprint; p->structureFingerprint=s.structureFingerprint;
    p->assetSha256=s.assetSha256; p->songId=s.songId;
    std::copy(std::begin(s.hotCues), std::end(s.hotCues), std::begin(p->hotCues));
    std::copy(std::begin(s.savedLoops), std::end(s.savedLoops), std::begin(p->savedLoops));
    return p;
}

std::vector<TrackDataPtr> readSetCatalog(const QString& requested, QString* error)
{
    const auto path = requested.isEmpty() ? defaultCatalog() : requested;
    const auto root = readJson(path, error);
    if (root.value("version").toInt() != 1 || !root.value("assets").isObject()) {
        if (error) *error = "Unsupported or missing local song catalog: " + path;
        return {};
    }
    std::vector<TrackDataPtr> tracks;
    const auto assets = root.value("assets").toObject();
    for (const auto& value : assets) {
        const auto o = value.toObject();
        auto t = std::make_shared<TrackData>();
        t->filePath=o.value("path").toString(); t->title=o.value("title").toString();
        t->artist=o.value("artist").toString(); t->bpm=o.value("bpm").toDouble();
        t->durationSec=o.value("durationSec").toDouble(); t->firstBeatSec=o.value("firstBeatSec").toDouble();
        t->audibleDurationSec=o.value("audibleDurationSec").toDouble();
        t->canonicalBeatOffset=o.value("canonicalBeatOffset").toDouble();
        t->fingerprint=o.value("fingerprint").toString(); t->structureFingerprint=o.value("structureFingerprint").toString();
        t->assetSha256=o.value("assetSha256").toString(); t->songId=o.value("songId").toString();
        t->isrc=o.value("isrc").toString(); t->musicBrainzRecording=o.value("musicBrainzRecording").toString();
        const auto cacheRoot = qEnvironmentVariable("GRAVITINO_CACHE_DIR", QDir::homePath() + "/.gravitino/cache");
        const auto key = QCryptographicHash::hash(t->filePath.toUtf8(), QCryptographicHash::Sha1).toHex();
        const auto cache = readJson(cacheRoot + '/' + key + ".json", nullptr);
        const auto cues = cache.value("hotCues").toArray(), loops = cache.value("savedLoops").toArray();
        for (int i=0; i<8; ++i) {
            if (i<cues.size()) t->hotCues[i]=cues[i].toDouble(-1);
            if (i<loops.size()) {
                const auto loop=loops[i].toObject();
                t->savedLoops[i]={loop.value("startSec").toDouble(-1),loop.value("endSec").toDouble(-1),loop.value("label").toString()};
            }
        }
        tracks.push_back(t);
    }
    return tracks;
}

std::vector<GvtFile> transitionsForSetSongs(const QStringList& songs,
    const std::vector<GvtFile>& files, QStringList* errors)
{
    std::vector<GvtFile> result;
    if (songs.size()<2) { if (errors) errors->append("Enter at least two songs, one per line."); return {}; }
    for (int i=0; i+1<songs.size(); ++i) {
        std::vector<const GvtFile*> found;
        for (const auto& f : files)
            if (f.sourceFormat == TransitionSourceFormat::PortableYaml &&
                titleMatches(f.from.title,songs[i]) && titleMatches(f.to.title,songs[i+1])) found.push_back(&f);
        if (found.size()!=1) {
            QStringList names; for (const auto* f:found) names << f->name;
            if (errors) errors->append(QString("%1 → %2: %3").arg(songs[i],songs[i+1],found.empty()
                ? "no .transition found" : "ambiguous: " + names.join(", ")));
        } else result.push_back(*found.front());
    }
    return result.size()+1==size_t(songs.size()) ? result : std::vector<GvtFile>{};
}

double setTempoRampSeconds(double beats,double from,double to) {
    return beats > 0 && from > 0 && to > 0 ? 120.0*beats/(from+to) : 0.0;
}
double setTempoRampBpm(double elapsed,double duration,double from,double to) {
    return duration>0 ? from+(to-from)*std::clamp(elapsed/duration,0.0,1.0) : to;
}
SetEqValues setEqRampValues(double elapsed,double duration,
    const SetEqValues& from,const SetEqValues& to) {
    const double progress=duration>0 ? std::clamp(elapsed/duration,0.0,1.0) : 1.0;
    SetEqValues result;
    for(size_t i=0;i<result.size();++i) result[i]=from[i]+(to[i]-from[i])*progress;
    return result;
}

SetRenderPlan planTransitionSet(const SetRenderRequest& r)
{
    SetRenderPlan plan;
    if (r.transitions.empty() || r.transitions.size()>200) {
        plan.errors << "Select between 1 and 200 transitions."; return plan;
    }
    SongCatalog catalog(r.catalogPath);
    for (size_t index=0; index<=r.transitions.size(); ++index) {
        const auto& f=r.transitions[index==0 ? 0 : index-1];
        const bool from=index==0;
        const auto ref=from ? f.from : f.to;
        std::vector<TrackDataPtr> candidates;
        for (const auto& t:r.assets) {
            if (!t || !QFileInfo::exists(t->filePath) || !matches(f,from,*t,catalog)) continue;
            if (index<r.transitions.size() && !matches(r.transitions[index],true,*t,catalog)) continue;
            if (int(index)<r.assetPaths.size() && !r.assetPaths[int(index)].isEmpty() &&
                QFileInfo(t->filePath).absoluteFilePath()!=QFileInfo(r.assetPaths[int(index)]).absoluteFilePath()) continue;
            candidates.push_back(t);
        }
        if (candidates.size()!=1) {
            plan.errors << QString("Song %1 (%2): %3").arg(index+1).arg(ref.title,
                candidates.empty() ? "no compatible local asset / disconnected transition chain" :
                "multiple compatible files — choose its audio asset");
            plan.tracks.push_back({}); continue;
        }
        plan.tracks.push_back(candidates.front());
        const auto& t=*candidates.front();
        if (!std::isfinite(t.bpm) || t.bpm<=0 || !std::isfinite(t.durationSec) || t.durationSec<=0)
            plan.errors << "Invalid local beat grid: " + t.title;
        const bool stems=(index>0 && needsStems(r.transitions[index-1],Role::ToDeck)) ||
            (index<r.transitions.size() && needsStems(r.transitions[index],Role::FromDeck));
        if (stems && !cachedStems(r,t)) plan.errors << "Prepare stems before export: " + t.title;
    }
    if (!plan.errors.isEmpty()) return plan;
    double previousBeat=0, entranceBpm=r.transitions.front().masterBpm;
    for (size_t i=0; i<r.transitions.size(); ++i) {
        const auto& f=r.transitions[i];
        GvtFile parsed; QString error;
        if (!transitionParse(transitionSerialize(f),parsed,&error) || !parsed.unsupportedRequirements.isEmpty()) {
            plan.errors << f.name + ": " + (error.isEmpty() ? "unsupported required capabilities" : error); continue;
        }
        std::vector<GvtEvent> events;
        if (!resolveTransitionPlaybackEvents(f,plan.tracks[i],plan.tracks[i+1],events,&error)) {
            plan.errors << f.name + ": " + error; continue;
        }
        const auto& out=*plan.tracks[i];
        if (i==0) previousBeat=transitionBeatAtSec(f,out,0);
        const double anchorSeconds=transitionSecAtBeat(f,out,f.anchorFromBeat);
        if (anchorSeconds<0 || anchorSeconds>=out.durationSec || f.anchorFromBeat<previousBeat-0.02)
            plan.errors << f.name + ": outgoing entry is outside the audio or has already passed after the previous transition.";
        const double gap=f.anchorFromBeat-previousBeat;
        if (gap<0.02 && std::fabs(entranceBpm-f.masterBpm)>0.02)
            plan.errors << f.name + ": no solo section remains for the requested BPM ramp.";
        plan.estimatedSeconds+=setTempoRampSeconds(gap,entranceBpm,f.masterBpm);
        const double end=transitionGraphEffectiveEndBeat(f);
        plan.estimatedSeconds+=end*60/f.masterBpm;
        const auto trace=buildTransitionTransportTrace(f,plan.tracks[i],plan.tracks[i+1],end);
        const auto incoming=trace.positionAt(Role::ToDeck,end);
        if (!incoming || !incoming->audible || !incoming->loopId.isEmpty())
            plan.errors << f.name + ": incoming song does not finish playing freely (stopped or still looping).";
        else {
            const auto& nextFile=i+1<r.transitions.size() ? r.transitions[i+1] : f;
            const auto& track=*plan.tracks[i+1];
            previousBeat=transitionBeatAtSec(nextFile,track,track.secAtCanonicalBeat(incoming->sourceBeat));
        }
        entranceBpm=finalIncomingBpm(f);
        if (!std::isfinite(entranceBpm) || entranceBpm<=0) plan.errors << f.name + ": invalid exit BPM.";
    }
    const auto& last=*plan.tracks.back();
    const auto& lastFile=r.transitions.back();
    const double end=transitionBeatAtSec(lastFile,last,last.durationSec);
    plan.estimatedSeconds+=std::max(0.0,end-previousBeat)*60/entranceBpm;
    if (plan.estimatedSeconds>5.5*3600) plan.errors << "Set is too long for standard 16-bit stereo WAV (4 GiB limit).";
    return plan;
}

bool setSongNeedsStems(const SetRenderRequest& r,size_t i) {
    return (i>0 && i<=r.transitions.size() && needsStems(r.transitions[i-1],Role::ToDeck)) ||
        (i<r.transitions.size() && needsStems(r.transitions[i],Role::FromDeck));
}
std::vector<TrackDataPtr> setAssetCandidates(const SetRenderRequest& r,size_t i) {
    std::vector<TrackDataPtr> result;
    if (r.transitions.empty() || i>r.transitions.size()) return result;
    SongCatalog catalog(r.catalogPath);
    const auto& f=r.transitions[i==0 ? 0 : i-1];
    for (const auto& t:r.assets)
        if (t && QFileInfo::exists(t->filePath) && matches(f,i==0,*t,catalog) &&
            (i==r.transitions.size() || matches(r.transitions[i],true,*t,catalog))) result.push_back(t);
    return result;
}

SetRenderResult renderTransitionSet(const SetRenderRequest& r,const QString& output,
    SetRenderProgress progress,SetRenderCancel cancelled)
{
    SetRenderResult result; result.outputPath=output;
    try {
        const auto plan=planTransitionSet(r);
        if (!plan.valid()) { result.error=plan.errors.join('\n'); return result; }
        // A queue is a snapshot. Never silently export an older recipe after
        // the user edits/deletes its file in another window.
        for(const auto& f:r.transitions) if(!f.filePath.isEmpty()) {
            GvtFile current; QString error;
            if(QFileInfo(f.filePath).size()>2*1024*1024 ||
               !loadTransitionFile(f.filePath,current,&error) ||
               transitionSerialize(current)!=transitionSerialize(f)) {
                result.error=f.name+": source transition changed or is unavailable. Reopen the set or re-add the transition.";
                return result;
            }
        }
        RecordingWav writer; QString error;
        if (!writer.open(output,&error)) { result.error=error; return result; }
        ControlBus bus; AudioEngine engine(&bus); TransitionPlayer player(&bus,&engine);
        transitionPlayerUseExternalClock(&player,true);
        QElapsedTimer notifications; notifications.start();
        QString stage;
        const auto check=[&] {
            if (cancelled && cancelled()) throw std::runtime_error("cancelled");
            if (progress && notifications.elapsed()>150) {
                progress(std::min(.99,writer.frames()/double(kSampleRate)/std::max(1.0,plan.estimatedSeconds)),stage);
                notifications.restart();
            }
        };
        const auto require=[&](bool ok) { if (!ok) throw std::runtime_error(error.toStdString()); };
        std::array<float,512*2> buffer{};
        const auto render=[&](int frames) {
            check(); engine.renderOffline(buffer.data(),frames); require(writer.write(buffer.data(),frames,&error));
        };
        QJsonArray tracks,transitions,ramps,eqRamps;
        for (const auto& t:plan.tracks) tracks.append(QJsonObject{{"title",t->title},{"artist",t->artist},
            {"asset_sha256",t->assetSha256},{"fingerprint",t->fingerprint},{"native_bpm",t->bpm},
            {"first_beat_seconds",t->firstBeatSec},{"canonical_beat_offset",t->canonicalBeatOffset}});
        const auto load=[&](size_t index,int deck) {
            stage=QString("Loading song %1/%2: %3").arg(index+1).arg(plan.tracks.size()).arg(plan.tracks[index]->title);
            check();
            auto t=decodeAsset(*plan.tracks[index],&error); require(bool(t));
            engine.deck(deck).loadTrack(t); engine.deck(deck).preservePitch.store(r.keyLock);
            const bool stems=(index>0 && needsStems(r.transitions[index-1],Role::ToDeck)) ||
                (index<r.transitions.size() && needsStems(r.transitions[index],Role::FromDeck));
            if (stems) { auto s=decodeStems(r,*t,&error); require(bool(s)); engine.deck(deck).attachStems(s); }
        };
        load(0,0);
        bus.dispatch({0,ControlId::Tempo,r.transitions.front().masterBpm/engine.deck(0).track()->bpm},Origin::System);
        bus.dispatch({0,ControlId::Play,1},Origin::System);
        const auto solo=[&](int deck,double targetSeconds,double endBpm,size_t songIndex,
                            const GvtInitialState* nextSetup=nullptr) {
            auto& d=engine.deck(deck); const auto t=d.track();
            if (!d.playing.load() || d.loopActive.load()) throw std::runtime_error("Cannot advance a stopped/looping song to its next transition.");
            const double sourceRemaining=targetSeconds-d.positionSec();
            if (sourceRemaining < -.02*60/t->bpm) throw std::runtime_error("The next transition entry has already passed.");
            const double startBpm=d.effectiveBpm();
            const double seconds=setTempoRampSeconds(std::max(0.0,sourceRemaining)*t->bpm/60,startBpm,endBpm);
            const qint64 total=qint64(std::llround(seconds*kSampleRate));
            const qint64 start=writer.frames();
            const double sourceStart=t->canonicalBeatAtSec(d.positionSec());
            const SetEqValues startEq{d.eqLow.load(),d.eqMid.load(),d.eqHigh.load()};
            const SetEqValues endEq=nextSetup && nextSetup->captured
                ? SetEqValues{nextSetup->eqLow,nextSetup->eqMid,nextSetup->eqHigh} : startEq;
            if(total==0 && start>0) {
                for(size_t band=0;band<startEq.size();++band)
                    if(std::fabs(startEq[band]-endEq[band])>1e-6)
                        throw std::runtime_error("No solo section remains for a smooth EQ ramp before the next transition.");
            }
            const auto applyEq=[&](const SetEqValues& values) {
                constexpr ControlId controls[]{ControlId::EqLow,ControlId::EqMid,ControlId::EqHigh};
                for(size_t band=0;band<values.size();++band)
                    bus.dispatch({deck,controls[band],values[band]},Origin::System);
            };
            for (qint64 done=0; done<total;) {
                const int frames=int(std::min<qint64>(512,total-done));
                const double mid=(done+frames*.5)/kSampleRate;
                bus.dispatch({deck,ControlId::Tempo,setTempoRampBpm(mid,seconds,startBpm,endBpm)/t->bpm},Origin::System);
                applyEq(setEqRampValues(mid,seconds,startEq,endEq));
                render(frames); done+=frames;
            }
            bus.dispatch({deck,ControlId::Tempo,endBpm/t->bpm},Origin::System);
            // Reach the exact authored setup before PRIME applies it again.
            // No incoming audio or in-transition automation is modified here.
            applyEq(endEq);
            ramps.append(QJsonObject{{"song_index",int(songIndex)},{"start_frame",double(start)},
                {"end_frame",double(writer.frames())},{"from_bpm",startBpm},{"to_bpm",endBpm},
                {"curve","linear-time"},{"from_track_beat",sourceStart},
                {"to_track_beat",t->canonicalBeatAtSec(targetSeconds)}});
            const auto valuesJson=[](const SetEqValues& values) {
                return QJsonObject{{"low",values[0]},{"mid",values[1]},{"high",values[2]}};
            };
            eqRamps.append(QJsonObject{{"song_index",int(songIndex)},{"start_frame",double(start)},
                {"end_frame",double(writer.frames())},{"curve","linear-time"},
                {"domain","normalized-knob"},{"from_values",valuesJson(startEq)},
                {"to_values",valuesJson(endEq)}});
        };
        int outgoing=0;
        for (size_t i=0;i<r.transitions.size();++i) {
            const auto& f=r.transitions[i];
            stage="Playing " + plan.tracks[i]->title;
            solo(outgoing,transitionSecAtBeat(f,*engine.deck(outgoing).track(),f.anchorFromBeat),f.masterBpm,i,&f.initialFrom);
            load(i+1,1-outgoing);
            stage=QString("Transition %1/%2: %3").arg(i+1).arg(r.transitions.size()).arg(f.name);
            prepareTransitionSetup(bus,engine,f,outgoing);
            positionTransitionPerform(engine,f,outgoing);
            require(player.arm(f,outgoing,true,&error));
            bus.dispatch({outgoing,ControlId::Play,1},Origin::Replay);
            const qint64 start=writer.frames();
            const double end=transitionGraphEffectiveEndBeat(f);
            double beat=0;
            while (beat<end-1e-9) {
                transitionPlayerAdvanceToBeat(&player,beat);
                const double bpm=engine.deck(outgoing).effectiveBpm();
                if (!(bpm>0 && bpm<1000)) throw std::runtime_error("Invalid transition render clock.");
                double next=end;
                for (const auto& e:f.events)
                    if (transitionEventIsExecutable(e) && e.beat>beat+1e-9) { next=std::min(next,e.beat); break; }
                // Event boundaries (especially song launches) are scheduled to
                // the nearest following sample, not a wall-clock GUI timer.
                const int frames=std::clamp(int(std::ceil((next-beat)*60/bpm*kSampleRate-1e-8)),1,128);
                render(frames); beat+=frames/double(kSampleRate)*bpm/60;
            }
            transitionPlayerAdvanceToBeat(&player,end+1e-8);
            player.abort();
            transitions.append(QJsonObject{{"id",f.id},{"name",f.name},{"start_frame",double(start)},
                {"end_frame",double(writer.frames())},{"start_seconds",double(start)/kSampleRate},
                {"end_seconds",double(writer.frames())/kSampleRate},{"outgoing_song_index",int(i)},
                {"incoming_song_index",int(i+1)},{"transition_yaml",transitionSerialize(f)}});
            bus.dispatch({outgoing,ControlId::Stop,1},Origin::System);
            bus.dispatch({outgoing,ControlId::Fader,0},Origin::System);
            outgoing=1-outgoing;
        }
        stage="Finishing " + plan.tracks.back()->title;
        solo(outgoing,engine.deck(outgoing).track()->durationSec,engine.deck(outgoing).effectiveBpm(),plan.tracks.size()-1);
        QJsonObject manifest{{"format","gravitino.recording"},{"version",1},{"title",r.title},
            {"created_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"renderer","offline-set.v2"},{"key_lock",r.keyLock},{"crossfader",0.5},
            {"tracks",tracks},{"transitions",transitions},{"tempo_ramps",ramps},{"eq_ramps",eqRamps},
            {"handoff_policy","Retire outgoing deck at authored transition end; linearly bridge BPM and EQ to the next setup during the solo section. Retain other incoming state."}};
        check(); require(writer.finish(manifest,&error));
        result.manifest=readRecordingManifest(output,&error); result.completed=true;
        if (progress) progress(1,"Recording ready");
    } catch (const std::exception& e) {
        result.cancelled=QString::fromUtf8(e.what())=="cancelled";
        result.error=result.cancelled ? "Export cancelled; no completed WAV was replaced." : QString::fromUtf8(e.what());
    }
    return result;
}

QJsonObject setRequestJson(const SetRenderRequest& r) {
    QJsonArray paths; for (const auto& f:r.transitions) paths.append(f.filePath);
    return {{"format","gravitino.set"},{"version",1},{"title",r.title},{"key_lock",r.keyLock},
        {"transition_paths",paths},{"asset_paths",QJsonArray::fromStringList(r.assetPaths)}};
}
bool readSetRequest(const QString& path,SetRenderRequest& r,QString* error) {
    const auto json=readJson(path,error);
    if (json.value("format")!="gravitino.set" || json.value("version").toInt()!=1 ||
        !json.value("transition_paths").isArray()) {
        if (error) *error="Unsupported set document."; return false;
    }
    SetRenderRequest result; result.title=json.value("title").toString("Gravitino set");
    result.keyLock=json.value("key_lock").toBool(true);
    const auto paths=json.value("transition_paths").toArray();
    if(paths.isEmpty() || paths.size()>200) {
        if(error) *error="A set must contain 1–200 transitions.";
        return false;
    }
    if(json.contains("asset_paths") && !json.value("asset_paths").isArray()) {
        if(error) *error="Asset paths must be an array.";
        return false;
    }
    const QDir base=QFileInfo(path).absoluteDir();
    for (const auto& p:paths) {
        GvtFile file;
        if (!p.isString() || p.toString().isEmpty()) {
            if(error) *error="Each transition path must be a non-empty string.";
            return false;
        }
        const auto source=base.absoluteFilePath(p.toString());
        if(QFileInfo(source).size()>2*1024*1024) {
            if(error) *error="Transition file exceeds the 2 MiB limit: "+source;
            return false;
        }
        if (!loadTransitionFile(source,file,error)) return false;
        result.transitions.push_back(file);
    }
    for (const auto& p:json.value("asset_paths").toArray()) {
        if(!p.isString()) { if(error) *error="Each asset path must be a string."; return false; }
        result.assetPaths << (p.toString().isEmpty() ? QString{} : base.absoluteFilePath(p.toString()));
    }
    result.catalogPath=r.catalogPath; result.stemsRoot=r.stemsRoot;
    result.assets=r.assets;
    if (result.assets.empty()) result.assets=readSetCatalog(result.catalogPath,error);
    if (result.assets.empty()) return false;
    r=std::move(result); return true;
}
}
