// Queue transition snapshots, validate their chain, and export without taking
// MASTER or touching live transport. All worker Qt/audio objects are private.
#include "SetRenderWindow.h"
#include "../analysis/StemSeparator.h"
#include "../audio/RecordingWav.h"
#include "../library/TrackLibrary.h"
#include "../transitions/LiveSetSession.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace gvt {
SetRenderWindow::SetRenderWindow(TrackLibrary* library,TransitionStore* store,StemSeparator* stems,QWidget* parent)
    : QMainWindow(parent),library_(library),store_(store),stems_(stems),cancelled_(std::make_shared<std::atomic<bool>>(false))
{
    setObjectName("setRenderWindow"); setWindowFlag(Qt::Window,true);
    setWindowTitle(tr("Set Recorder — Gravitino")); resize(1180,740); setMinimumSize(900,620);
    auto* central=new QWidget(this); auto* layout=new QVBoxLayout(central); setCentralWidget(central);
    auto* heading=new QLabel(tr("Build a set → export one continuous WAV"),central);
    heading->setObjectName("setHeading");
    heading->setStyleSheet("font-size:18px; font-weight:600;"); layout->addWidget(heading);
    auto* help=new QLabel(tr("Choose transitions in performance order. Recorded transitions stay intact; BPM and LOW / MID / HIGH EQ ramp smoothly across the solo sections to match the next setup. The first song starts at the beginning and the final song plays to its end."),central);
    help->setWordWrap(true); layout->addWidget(help);
    editing_=new QWidget(central); auto* editLayout=new QVBoxLayout(editing_); editLayout->setContentsMargins(0,0,0,0);
    auto* tools=new QHBoxLayout;
    title_=new QLineEdit(request_.title,editing_); title_->setObjectName("setTitle");
    tools->addWidget(new QLabel(tr("Set name"),editing_)); tools->addWidget(title_,1);
    auto* songs=new QPushButton(tr("From song list…"),editing_); songs->setObjectName("setFromSongs"); tools->addWidget(songs);
    auto* load=new QPushButton(tr("Open set…"),editing_); tools->addWidget(load);
    auto* save=new QPushButton(tr("Save set…"),editing_); tools->addWidget(save); editLayout->addLayout(tools);
    auto* split=new QSplitter(Qt::Horizontal,editing_);
    auto* availableHost=new QWidget(split); auto* left=new QVBoxLayout(availableHost); left->setContentsMargins(0,0,0,0);
    left->addWidget(new QLabel(tr("AVAILABLE TRANSITIONS"),availableHost));
    search_=new QLineEdit(availableHost); search_->setPlaceholderText(tr("Search transitions or songs…")); left->addWidget(search_);
    legacy_=new QCheckBox(tr("Include legacy .gvt copies"),availableHost); left->addWidget(legacy_);
    availableTable_=new QTableWidget(availableHost); availableTable_->setObjectName("setAvailableTransitions");
    availableTable_->setColumnCount(2); availableTable_->setHorizontalHeaderLabels({tr("Use"),tr("Transition")});
    availableTable_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    availableTable_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    availableTable_->setEditTriggers(QAbstractItemView::NoEditTriggers); left->addWidget(availableTable_,1);
    auto* add=new QPushButton(tr("Add checked →"),availableHost); add->setObjectName("setAddChecked"); left->addWidget(add);
    auto* queueHost=new QWidget(split); auto* right=new QVBoxLayout(queueHost); right->setContentsMargins(8,0,0,0);
    right->addWidget(new QLabel(tr("PLAY ORDER"),queueHost));
    queue_=new QTableWidget(queueHost); queue_->setObjectName("setQueue"); queue_->setColumnCount(3);
    queue_->setHorizontalHeaderLabels({tr("Transition"),tr("Songs"),tr("BPM")});
    queue_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    queue_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    queue_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    queue_->setEditTriggers(QAbstractItemView::NoEditTriggers); queue_->setSelectionBehavior(QAbstractItemView::SelectRows);
    queue_->setSelectionMode(QAbstractItemView::SingleSelection); right->addWidget(queue_,1);
    auto* order=new QHBoxLayout;
    auto* up=new QPushButton(tr("↑ Up"),queueHost); up->setObjectName("setMoveUp"); order->addWidget(up);
    auto* down=new QPushButton(tr("↓ Down"),queueHost); down->setObjectName("setMoveDown"); order->addWidget(down);
    auto* remove=new QPushButton(tr("Remove"),queueHost); remove->setObjectName("setRemove"); order->addWidget(remove);
    auto* assets=new QPushButton(tr("Audio files…"),queueHost); order->addWidget(assets);
    right->addLayout(order); split->setSizes({380,760}); editLayout->addWidget(split,1);
    auto* options=new QHBoxLayout;
    keyLock_=new QCheckBox(tr("Preserve musical pitch (key lock)"),editing_); keyLock_->setChecked(true); options->addWidget(keyLock_);
    prepare_=new QPushButton(tr("Prepare missing stems"),editing_); options->addWidget(prepare_); options->addStretch();
    editLayout->addLayout(options); layout->addWidget(editing_,1);
    status_=new QLabel(central); status_->setObjectName("setStatus"); status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); layout->addWidget(status_);
    progress_=new QProgressBar(central); progress_->setRange(0,1000); progress_->setValue(0); layout->addWidget(progress_);
    auto* actions=new QHBoxLayout;
    auto* format=new QLabel(tr("WAV • 48 kHz • stereo • 16-bit • embedded recipes + chapter markers"),central);
    format->setWordWrap(true); actions->addWidget(format,1);
    open_=new QPushButton(tr("Open WAV"),central); open_->setEnabled(false); actions->addWidget(open_);
    cancel_=new QPushButton(tr("Cancel export"),central); cancel_->setObjectName("setCancelExport"); cancel_->setEnabled(false); actions->addWidget(cancel_);
    export_=new QPushButton(tr("EXPORT WAV…"),central); export_->setObjectName("setExport"); actions->addWidget(export_); layout->addLayout(actions);
    connect(songs,&QPushButton::clicked,this,&SetRenderWindow::fromSongList);
    connect(load,&QPushButton::clicked,this,[this] { const auto path=QFileDialog::getOpenFileName(this,tr("Open set"),{},tr("Gravitino set (*.json)")); if(!path.isEmpty()) openSet(path); });
    connect(save,&QPushButton::clicked,this,&SetRenderWindow::saveSet);
    connect(search_,&QLineEdit::textChanged,this,[this](const QString& query) {
        for(int i=0;i<availableTable_->rowCount();++i) availableTable_->setRowHidden(i,!availableTable_->item(i,1)->toolTip().contains(query,Qt::CaseInsensitive));
    });
    connect(legacy_,&QCheckBox::toggled,this,&SetRenderWindow::refreshAvailable);
    connect(add,&QPushButton::clicked,this,&SetRenderWindow::addChecked);
    connect(up,&QPushButton::clicked,this,[this]{moveSelected(-1);});
    connect(down,&QPushButton::clicked,this,[this]{moveSelected(1);});
    connect(remove,&QPushButton::clicked,this,[this] {
        const int row=queue_->currentRow(); if(row<0 || row>=int(request_.transitions.size())) return;
        request_.transitions.erase(request_.transitions.begin()+row); request_.assetPaths.clear(); refreshQueue();
        if(queue_->rowCount()) queue_->selectRow(std::min(row,queue_->rowCount()-1));
    });
    connect(assets,&QPushButton::clicked,this,&SetRenderWindow::chooseAssets);
    connect(prepare_,&QPushButton::clicked,this,&SetRenderWindow::prepareStems);
    connect(cancel_,&QPushButton::clicked,this,[this]{cancelled_->store(true); status_->setText(tr("Cancelling safely…"));});
    connect(open_,&QPushButton::clicked,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(completedPath_));});
    connect(export_,&QPushButton::clicked,this,[this] {
        const auto dir=QDir::homePath()+"/Music/Gravitino/Recordings"; QDir().mkpath(dir);
        const auto suggested=dir+"/set-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")+".wav";
        const auto path=QFileDialog::getSaveFileName(this,tr("Export set to a new WAV"),suggested,tr("WAV audio (*.wav)"));
        if(!path.isEmpty()) exportTo(path);
    });
    connect(&watcher_,&QFutureWatcher<SetRenderResult>::finished,this,[this] {
        const auto result=watcher_.result(); editing_->setEnabled(true); cancel_->setEnabled(false); refreshQueue();
        if(liveStart_) liveStart_->setEnabled(!live_->protectedRouting());
        if(result.completed) showRecording(result.outputPath);
        else status_->setText(result.error);
        emit exportFinished(result.outputPath,result.completed);
    });
    if(store_) connect(store_,&TransitionStore::changed,this,&SetRenderWindow::refreshAvailable);
    if(stems_) {
        connect(stems_,&StemSeparator::progress,this,[this](const QString&,const QString& stage) {
            if(!watcher_.isRunning()) status_->setText(tr("Stems: %1").arg(stage));
        });
        connect(stems_,&StemSeparator::stemsReady,this,[this](const QString&,StemSetPtr) { if(!watcher_.isRunning()) refreshQueue(); });
        connect(stems_,&StemSeparator::stemsFailed,this,[this](const QString&,const QString& error) { if(!watcher_.isRunning()) status_->setText(error); });
    }
    QString error; request_.assets=readSetCatalog({},&error);
    if(library_) {
        request_.catalogPath=library_->songCatalog()->path();
        for(int i=0;i<library_->trackCount();++i) if(auto t=library_->trackAt(i)) {
            const auto found=std::find_if(request_.assets.begin(),request_.assets.end(),[&](const auto& a){return a->filePath==t->filePath;});
            if(found!=request_.assets.end()) *found=setAssetProfile(*t); else request_.assets.push_back(setAssetProfile(*t));
        }
    }
    refreshAvailable(); refreshQueue();
}

SetRenderWindow::~SetRenderWindow() { cancelled_->store(true); watcher_.waitForFinished(); }
void SetRenderWindow::enableLiveQueue(LiveSetSession* session) {
    if(live_ || !session) return;
    live_=session;
    setWindowTitle(tr("Live Queue / Set Recorder — Gravitino"));
    findChild<QLabel*>("setHeading")->setText(tr("Build a set → play live or export WAV"));
    auto* layout=qobject_cast<QVBoxLayout*>(centralWidget()->layout());
    auto* explanation=new QLabel(tr("LIVE: queued set → speakers; decks, FLX4 and editor → FLX4 headphones. Alternating PLAY/CUE lights mean PREP ONLY. Hardware output-volume knobs still affect their wired outputs. Append only: accepted songs/recipes stay locked; a 30-second audio buffer locks the near future. New requests need a connecting transition, analyzed audio and required stems."),centralWidget());
    explanation->setWordWrap(true); layout->insertWidget(2,explanation);
    liveStatus_=new QLabel(tr("Live queue is off."),centralWidget()); liveStatus_->setObjectName("liveQueueStatus");
    liveStatus_->setWordWrap(true); layout->addWidget(liveStatus_);
    auto* controls=new QHBoxLayout;
    liveStart_=new QPushButton(tr("START LIVE QUEUE"),centralWidget()); liveStart_->setObjectName("liveQueueStart");
    liveAppend_=new QPushButton(tr("APPEND to live queue"),centralWidget()); liveAppend_->setObjectName("liveQueueAppend");
    livePause_=new QPushButton(tr("Pause live"),centralWidget()); livePause_->setObjectName("liveQueuePause");
    liveStop_=new QPushButton(tr("Stop live"),centralWidget()); liveStop_->setObjectName("liveQueueStop");
    liveLeave_=new QPushButton(tr("Return decks to MASTER…"),centralWidget()); liveLeave_->setObjectName("liveQueueLeave");
    for(auto* button:{liveStart_,liveAppend_,livePause_,liveStop_,liveLeave_}) controls->addWidget(button);
    layout->addLayout(controls);
    connect(liveStart_,&QPushButton::clicked,this,[this] {
        if(watcher_.isRunning()) return;
        if(QMessageBox::question(this,tr("Start isolated live set?"),tr("The queued set will take over the speakers from its beginning. The two decks and editor will move to FLX4 headphones. Confirm your headphones are connected; test their output in Settings first."))!=QMessageBox::Yes) return;
        QString error; if(!live_->start(snapshot(),&error)) QMessageBox::warning(this,tr("Cannot start live queue"),error);
    });
    connect(liveAppend_,&QPushButton::clicked,this,[this] {
        QString error;
        if(!live_->append(snapshot(),&error)) QMessageBox::warning(this,tr("Cannot append"),error);
        else status_->setText(tr("Append requested. It will be checked at the next safe song boundary; existing live songs remain unchanged."));
    });
    connect(live_,&LiveSetSession::appendFinished,this,[this](bool ok,const QString& message) {
        status_->setText(message);
        if(!ok) QMessageBox::warning(this,tr("Live queue unchanged"),message);
    });
    connect(livePause_,&QPushButton::clicked,this,[this]{live_->pause(!live_->paused());});
    connect(liveStop_,&QPushButton::clicked,this,[this] {
        if(QMessageBox::question(this,tr("Stop live music?"),tr("This stops the music on the speakers. Preparation stays isolated."))==QMessageBox::Yes) live_->stop();
    });
    connect(liveLeave_,&QPushButton::clicked,this,[this] {
        if(QMessageBox::question(this,tr("Return to ordinary mixing?"),tr("Stop both preparation decks and editor preview first. After leaving, mouse and controller actions will affect MASTER again."))!=QMessageBox::Yes) return;
        QString error; if(!live_->leave(&error)) QMessageBox::warning(this,tr("Still protected"),error);
    });
    const auto refresh=[this] {
        const bool protectedRoute=live_->protectedRouting();
        liveStart_->setEnabled(!protectedRoute && !watcher_.isRunning());
        liveAppend_->setEnabled(protectedRoute && live_->running());
        livePause_->setEnabled(protectedRoute && live_->running());
        livePause_->setText(live_->paused() ? tr("Resume live") : tr("Pause live"));
        liveStop_->setEnabled(protectedRoute && live_->running());
        liveLeave_->setEnabled(protectedRoute && !live_->running());
        const int seconds=int(live_->elapsedSeconds());
        liveStatus_->setText(!protectedRoute ? tr("Live queue is off.") : tr("LIVE %1:%2 • %3 • buffered %4 s • %5 accepted transitions\nPREP ONLY: decks, FLX4 and editor → headphones. Closing this window does not stop live music.")
            .arg(seconds/60).arg(seconds%60,2,10,QLatin1Char('0')).arg(live_->status()).arg(live_->bufferedSeconds(),0,'f',1).arg(live_->acceptedTransitions()));
    };
    connect(live_,&LiveSetSession::changed,this,refresh); refresh();
    connect(live_,&LiveSetSession::routingChanged,this,[this]{refreshQueue();});
}
SetRenderRequest SetRenderWindow::snapshot() const {
    auto result=request_; result.title=title_->text().trimmed(); result.keyLock=keyLock_->isChecked();
    // Grid edits in the main library should also be used by a long-open queue.
    if(library_) for(int i=0;i<library_->trackCount();++i) if(auto t=library_->trackAt(i)) {
        const auto found=std::find_if(result.assets.begin(),result.assets.end(),[&](const auto& a){return a->filePath==t->filePath;});
        if(found!=result.assets.end()) *found=setAssetProfile(*t); else result.assets.push_back(setAssetProfile(*t));
    }
    return result;
}
void SetRenderWindow::setRequest(const SetRenderRequest& r) {
    if(watcher_.isRunning()) return;
    request_=r; title_->setText(r.title); keyLock_->setChecked(r.keyLock); refreshQueue();
}
void SetRenderWindow::refreshAvailable() {
    if(!store_) return;
    available_.clear();
    for(const auto& f:store_->all()) if(legacy_->isChecked() || f.sourceFormat==TransitionSourceFormat::PortableYaml) available_.push_back(f);
    std::stable_sort(available_.begin(),available_.end(),[](const auto& a,const auto& b){return a.name.localeAwareCompare(b.name)<0;});
    availableTable_->setRowCount(int(available_.size()));
    for(int i=0;i<int(available_.size());++i) {
        const auto& f=available_[i]; auto* check=new QTableWidgetItem; check->setCheckState(Qt::Unchecked); availableTable_->setItem(i,0,check);
        auto* name=new QTableWidgetItem(f.name+(f.sourceFormat==TransitionSourceFormat::LegacyGvt ? " [.gvt]" : ""));
        name->setToolTip(f.name+'\n'+f.from.title+" → "+f.to.title); availableTable_->setItem(i,1,name);
        availableTable_->setRowHidden(i,!name->toolTip().contains(search_->text(),Qt::CaseInsensitive));
    }
}
void SetRenderWindow::refreshQueue() {
    const int selected=queue_->currentRow(); queue_->setRowCount(int(request_.transitions.size()));
    for(int i=0;i<queue_->rowCount();++i) {
        const auto& f=request_.transitions[i];
        const QStringList values{f.name,f.from.title+" → "+f.to.title,QString::number(f.masterBpm,'f',2)};
        for(int j=0;j<3;++j) { auto* item=new QTableWidgetItem(values[j]); item->setToolTip(values[j]); queue_->setItem(i,j,item); }
    }
    if(selected>=0 && selected<queue_->rowCount()) queue_->selectRow(selected);
    const auto plan=planTransitionSet(snapshot());
    export_->setEnabled(plan.valid() && !watcher_.isRunning() && !(live_ && live_->protectedRouting()));
    prepare_->setEnabled(stems_ && !request_.transitions.empty());
    status_->setText(plan.valid() ? tr("%1 transitions • %2 songs • approximately %3:%4 • ready to export offline")
        .arg(request_.transitions.size()).arg(plan.tracks.size()).arg(int(plan.estimatedSeconds)/60)
        .arg(int(plan.estimatedSeconds)%60,2,10,QLatin1Char('0')) : plan.errors.mid(0,3).join('\n')+
            (plan.errors.size()>3 ? tr("\n…and %1 more. Hover here for details.").arg(plan.errors.size()-3) : QString{}));
    status_->setToolTip(plan.errors.join('\n'));
}
void SetRenderWindow::addChecked() {
    for(int i=0;i<availableTable_->rowCount();++i) if(availableTable_->item(i,0)->checkState()==Qt::Checked) {
        request_.transitions.push_back(available_[i]); availableTable_->item(i,0)->setCheckState(Qt::Unchecked);
    }
    request_.assetPaths.clear(); refreshQueue();
}
void SetRenderWindow::moveSelected(int delta) {
    const int row=queue_->currentRow(), next=row+delta;
    if(row<0 || next<0 || next>=int(request_.transitions.size())) return;
    std::swap(request_.transitions[row],request_.transitions[next]); request_.assetPaths.clear(); refreshQueue(); queue_->selectRow(next);
}
void SetRenderWindow::fromSongList() {
    if(!store_) return;
    bool ok=false;
    const auto text=QInputDialog::getMultiLineText(this,tr("Build set from songs"),tr("One song per line, in play order. Every adjacent pair must have exactly one .transition."),{},&ok);
    if(!ok) return;
    QStringList songs; for(const auto& line:text.split('\n')) if(!line.trimmed().isEmpty()) songs << line.trimmed();
    QStringList errors; auto files=transitionsForSetSongs(songs,store_->all(),&errors);
    if(!errors.isEmpty()) { QMessageBox::warning(this,tr("Set needs a choice"),errors.join('\n')); return; }
    request_.transitions=std::move(files); request_.assetPaths.clear(); refreshQueue();
}
bool SetRenderWindow::openSet(const QString& path) {
    auto r=snapshot(); QString error;
    if(!readSetRequest(path,r,&error)) { status_->setText(error); return false; }
    setRequest(r); return true;
}
void SetRenderWindow::saveSet() {
    const auto path=QFileDialog::getSaveFileName(this,tr("Save set queue"),{},tr("Set plan (*.set.json)")); if(path.isEmpty()) return;
    QSaveFile file(path); const auto bytes=QJsonDocument(setRequestJson(snapshot())).toJson();
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) status_->setText(file.errorString());
}
void SetRenderWindow::chooseAssets() {
    if(request_.transitions.empty()) return;
    QDialog dialog(this); dialog.setWindowTitle(tr("Choose each song's audio asset")); dialog.resize(1000,520);
    auto* layout=new QVBoxLayout(&dialog); auto* table=new QTableWidget(int(request_.transitions.size())+1,2,&dialog);
    table->setHorizontalHeaderLabels({tr("Song"),tr("Compatible audio file")}); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    std::vector<QComboBox*> choices;
    for(int i=0;i<table->rowCount();++i) {
        const auto& f=request_.transitions[i==0 ? 0 : i-1]; table->setItem(i,0,new QTableWidgetItem(i==0 ? f.from.title : f.to.title));
        auto* combo=new QComboBox(table); for(const auto& t:setAssetCandidates(request_,i)) combo->addItem(t->filePath,t->filePath);
        if(i<request_.assetPaths.size()) combo->setCurrentIndex(std::max(0,combo->findData(request_.assetPaths[i])));
        choices.push_back(combo); table->setCellWidget(i,1,combo);
    }
    layout->addWidget(table); auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted) return;
    request_.assetPaths.clear(); for(auto* choice:choices) request_.assetPaths << choice->currentData().toString(); refreshQueue();
}
void SetRenderWindow::prepareStems() {
    if(!stems_) return;
    const auto plan=planTransitionSet(snapshot()); int count=0;
    for(size_t i=0;i<plan.tracks.size();++i) if(plan.tracks[i] && setSongNeedsStems(request_,i) && !stems_->hasCached(*plan.tracks[i])) {
        stems_->requestStemsForProfile(*plan.tracks[i]); ++count;
    }
    status_->setText(count ? tr("Preparing stems for %1 songs; export will unlock when ready.").arg(count) : tr("Required stems are cached. Resolve any audio-file choices first."));
}
void SetRenderWindow::exportTo(const QString& path) {
    if(watcher_.isRunning() || (live_ && live_->protectedRouting())) return;
    const auto r=snapshot(); const auto plan=planTransitionSet(r);
    if(!plan.valid()) { status_->setText(plan.errors.join('\n')); return; }
    cancelled_->store(false); editing_->setEnabled(false); export_->setEnabled(false); cancel_->setEnabled(true); progress_->setValue(0);
    if(liveStart_) liveStart_->setEnabled(false);
    const auto cancel=cancelled_;
    watcher_.setFuture(QtConcurrent::run([this,r,path,cancel] {
        return renderTransitionSet(r,path,[this](double fraction,const QString& status) {
            QMetaObject::invokeMethod(this,[this,fraction,status]{progress_->setValue(int(fraction*1000)); status_->setText(status);},Qt::QueuedConnection);
        },[cancel]{return cancel->load();});
    }));
}
void SetRenderWindow::showRecording(const QString& path) {
    QString error; const auto manifest=readRecordingManifest(path,&error);
    if(manifest.isEmpty()) { status_->setText(error); return; }
    completedPath_=path; open_->setEnabled(true); progress_->setValue(1000);
    const int seconds=int(manifest.value("duration_seconds").toDouble());
    status_->setText(tr("Ready: %1:%2 — %3").arg(seconds/60).arg(seconds%60,2,10,QLatin1Char('0')).arg(path));
}
void SetRenderWindow::closeEvent(QCloseEvent* event) {
    if(watcher_.isRunning()) {
        if(QMessageBox::question(this,tr("Cancel export?"),tr("A WAV is still rendering. Cancel the export and close?"),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) { event->ignore(); return; }
        cancelled_->store(true);
    }
    event->accept();
}
}
