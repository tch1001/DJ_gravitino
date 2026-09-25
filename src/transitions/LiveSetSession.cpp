// Backpressure lives exclusively on the producer thread; preparation, source
// loading and GUI stalls cannot dispatch controls into the queued mix engine.
#include "LiveSetSession.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QThread>
#include <algorithm>

namespace gvt {
LiveSetSession::LiveSetSession(AudioEngine* output,QObject* parent)
    : QObject(parent),output_(output),ring_(capacity_*2)
{
    producerPool_.setMaxThreadCount(1); // never wait behind library/stem jobs
    timer_.setInterval(100);
    connect(&timer_,&QTimer::timeout,this,[this] {
        if (done_.load() && read_.load()==written_.load()) running_=false;
        emit changed();
    });
    connect(&worker_,&QFutureWatcher<SetRenderResult>::finished,this,[this] {
        const auto result=worker_.result();
        producerActive_=false;
        failed_=!result.completed && !result.cancelled;
        resultMessage_=result.completed ? tr("Queue finished — preparation remains isolated") :
            result.cancelled ? tr("Queue stopped — preparation remains isolated") : result.error;
        done_.store(true);
        if (appendPending_) {
            appendPending_=false;
            emit appendFinished(false,tr("Queue ended before the append could be accepted."));
        }
        emit changed();
    });
}
LiveSetSession::~LiveSetSession()
{
    cancelled_.store(true); worker_.waitForFinished();
    // Session is main-window owned, not destroyed when its queue window hides.
    // On application shutdown drain device callbacks before freeing the ring.
    output_->shutdownLiveProgram(this);
}
bool LiveSetSession::protectedRouting() const { return output_->liveProgramActive(); }
bool LiveSetSession::start(const SetRenderRequest& request,QString* error)
{
    if (producerActive_ || protectedRouting()) {
        if(error) *error=tr("Leave the previous live session before starting another."); return false;
    }
    const auto plan=planTransitionSet(request);
    if(!plan.valid()) { if(error) *error=plan.errors.join('\n'); return false; }
    written_=0; read_=0; underruns_=0; cancelled_=false; paused_=false; buffering_=true; done_=false;
    sections_.clear(); resultMessage_.clear(); appendPending_=false; failed_=false;
    { std::lock_guard lock(appendMutex_); appendRequest_.reset(); }
    if(!output_->acquireLiveProgram(this,error)) return false;
    acceptedTransitions_=int(request.transitions.size());
    running_=true; producerActive_=true;
    emit routingChanged(true); timer_.start(); emit changed();
    worker_.setFuture(QtConcurrent::run(&producerPool_,[this,request] {
        SetStreamHooks hooks;
        hooks.write=[this](const float* samples,int frames) { return write(samples,frames); };
        hooks.takeAppend=[this] {
            std::lock_guard lock(appendMutex_);
            auto request=std::move(appendRequest_); appendRequest_.reset(); return request;
        };
        hooks.appendResult=[this](bool ok,const QString& message) {
            QMetaObject::invokeMethod(this,[this,ok,message] {
                if(ok) acceptedTransitions_=requestedCount_;
                appendPending_=false; emit appendFinished(ok,message);
            },Qt::QueuedConnection);
        };
        hooks.section=[this](qint64 frame,const QString& name,int index) {
            QMetaObject::invokeMethod(this,[this,frame,name,index] { sections_.push_back({frame,name,index}); },Qt::QueuedConnection);
        };
        return streamTransitionSet(request,hooks,[this] { return cancelled_.load(); });
    }));
    return true;
}
bool LiveSetSession::append(const SetRenderRequest& request,QString* error)
{
    if (!running_ || done_.load() || cancelled_.load() || appendPending_) {
        if(error) *error=tr("Queue is stopped/finished, or another append is awaiting its next safe song boundary.");
        return false;
    }
    { std::lock_guard lock(appendMutex_); appendRequest_=request; }
    requestedCount_=int(request.transitions.size());
    appendPending_=true; return true;
}
void LiveSetSession::pause(bool paused) { paused_=paused; emit changed(); }
void LiveSetSession::stop() {
    paused_=true; cancelled_=true; running_=false; emit changed();
}
bool LiveSetSession::leave(QString* error) {
    // Wait for the GUI's finished notification, not just worker completion:
    // reusing the watcher before its old notification arrives can observe a
    // new future from the old slot and block the GUI on a live producer.
    if (producerActive_) { if(error) *error=tr("Stop the queue and wait for its renderer to finish first."); return false; }
    if (running_) { if(error) *error=tr("Stop the queue first."); return false; }
    if (!output_->releaseLiveProgram(this,error)) return false;
    timer_.stop(); emit routingChanged(false); emit changed(); return true;
}
double LiveSetSession::elapsedSeconds() const { return read_.load()/double(kSampleRate); }
double LiveSetSession::bufferedSeconds() const {
    const auto consumed=read_.load();
    return (written_.load()-consumed)/double(kSampleRate);
}
QString LiveSetSession::status() const {
    QString song=tr("Preparing live audio…");
    const auto frame=qint64(read_.load());
    for(const auto& section:sections_) { if(section.frame>frame) break; song=section.name; }
    if(!running_) song=resultMessage_.isEmpty() ? tr("Stopped — preparation remains isolated") : resultMessage_;
    else if(paused_.load()) song=tr("PAUSED — ")+song;
    else if(buffering_.load()) song=tr("BUFFERING — ")+song;
    if(underruns_.load()) song+=tr(" • audio underruns: %1").arg(underruns_.load());
    if(appendPending_) song+=tr(" • append awaiting next safe boundary");
    if(running_ && failed_) song+=tr(" • QUEUE ERROR; buffered audio will end: ")+resultMessage_;
    if(!output_->headphoneOutputAvailable()) song+=tr(" • PREP MUTED: reconnect FLX4 headphones output");
    return song;
}
bool LiveSetSession::write(const float* samples,int frames) {
    const auto count=uint64_t(frames);
    const auto start=written_.load(std::memory_order_relaxed);
    while(start-read_.load(std::memory_order_acquire)+count>capacity_) {
        if(cancelled_.load()) return false;
        QThread::msleep(2);
    }
    if(cancelled_.load()) return false;
    for(uint64_t i=0;i<count;++i) {
        const auto target=((start+i)%capacity_)*2;
        ring_[target]=samples[i*2]; ring_[target+1]=samples[i*2+1];
    }
    written_.store(start+count,std::memory_order_release); return true;
}
void LiveSetSession::read(float* out,int frames) noexcept {
    std::fill_n(out,frames*2,0.0f);
    if(paused_.load() || cancelled_.load()) return;
    const auto start=read_.load(std::memory_order_relaxed);
    const auto available=written_.load(std::memory_order_acquire)-start;
    if(buffering_.load()) {
        if(available<uint64_t(kSampleRate)*2 && !done_.load()) return;
        buffering_=false;
    }
    const auto count=std::min(available,uint64_t(frames));
    for(uint64_t i=0;i<count;++i) {
        const auto source=((start+i)%capacity_)*2;
        out[i*2]=ring_[source]; out[i*2+1]=ring_[source+1];
    }
    read_.store(start+count,std::memory_order_release);
    if(count<uint64_t(frames) && !done_.load()) { ++underruns_; buffering_=true; }
}
}
