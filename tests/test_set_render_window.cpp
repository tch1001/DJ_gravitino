// Exercise queue editing and threaded export in the actual compact GUI.
#include "SetRenderFixtures.h"
#include "ui/SetRenderWindow.h"
#include "ui/Theme.h"
#include "library/TrackLibrary.h"
#include "transitions/LiveSetSession.h"
#include "audio/AudioDeviceTestAccess.h"
#include <QElapsedTimer>
#include <QThread>
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen"); QApplication app(argc,argv); app.setStyleSheet(gvt::appStyleSheet());
    QTemporaryDir dir; int failures=0;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(0)
    auto request=setFixtureRequest(dir.path()); CHECK(request.assets.size()==3);
    const auto previousTransitions=qgetenv("GRAVITINO_TRANSITIONS_DIR");
    qputenv("GRAVITINO_TRANSITIONS_DIR",dir.filePath("transitions").toUtf8());
    gvt::TransitionStore store; QString error;
    for(auto f:request.transitions) CHECK(!store.save(f,&error).isEmpty());
    gvt::SetRenderWindow window(nullptr,&store); window.setRequest(request); window.resize(1100,700); window.show(); app.processEvents();
    auto* queue=window.findChild<QTableWidget*>("setQueue"); auto* exportButton=window.findChild<QPushButton*>("setExport");
    auto* up=window.findChild<QPushButton*>("setMoveUp"); auto* down=window.findChild<QPushButton*>("setMoveDown");
    auto* remove=window.findChild<QPushButton*>("setRemove");
    CHECK(queue && exportButton && up && down && remove); if(!queue || !exportButton || !up || !down || !remove) return 1;
    CHECK(queue->rowCount()==2); CHECK(exportButton->isEnabled()); CHECK(window.size()==QSize(1100,700));
    queue->selectRow(1); up->click(); CHECK(!exportButton->isEnabled()); CHECK(queue->currentRow()==0);
    down->click(); CHECK(exportButton->isEnabled());
    queue->selectRow(1); remove->click(); CHECK(queue->rowCount()==1); CHECK(exportButton->isEnabled());
    window.setRequest(request);
    QEventLoop loop; bool completed=false;
    QObject::connect(&window,&gvt::SetRenderWindow::exportFinished,&loop,[&](const QString&,bool ok){completed=ok;loop.quit();});
    QTimer timeout; timeout.setSingleShot(true); QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit); timeout.start(15000);
    window.exportTo(dir.filePath("gui-set.wav")); CHECK(!exportButton->isEnabled()); loop.exec(); CHECK(completed);
    CHECK(exportButton->isEnabled()); CHECK(!gvt::readRecordingManifest(dir.filePath("gui-set.wav")).isEmpty());
    auto empty=request; empty.transitions.clear(); window.setRequest(empty);
    auto* available=window.findChild<QTableWidget*>("setAvailableTransitions");
    auto* add=window.findChild<QPushButton*>("setAddChecked"); CHECK(available && add);
    if(available && add) {
        CHECK(available->rowCount()==2);
        for(int i=0;i<available->rowCount();++i) available->item(i,0)->setCheckState(Qt::Checked);
        add->click(); CHECK(queue->rowCount()==2); CHECK(exportButton->isEnabled());
    }
    qputenv("GRAVITINO_TRANSITIONS_DIR",previousTransitions);
    if(app.arguments().contains("--demo")) {
        const auto path=app.arguments().value(app.arguments().indexOf("--demo")+1);
        gvt::SetRenderRequest demo;
        store.reload();
        CHECK(gvt::readSetRequest(path,demo,&error)); window.setRequest(demo); app.processEvents();
    }
    gvt::ControlBus bus; gvt::AudioEngine engine(&bus);
    gvt::LiveSetSession live(&engine);
    window.enableLiveQueue(&live); app.processEvents();
    CHECK(window.findChild<QPushButton*>("liveQueueStart")->isEnabled());
    CHECK(!window.findChild<QPushButton*>("liveQueueAppend")->isEnabled());
    CHECK(!window.findChild<QPushButton*>("liveQueueLeave")->isEnabled());
    CHECK(window.findChild<QLabel*>("liveQueueStatus")->text().contains("off"));
    gvt::detail::AudioDeviceTestAccess::setOfflineHeadphones(engine,true);
    CHECK(live.start(request,&error)); app.processEvents();
    CHECK(!window.findChild<QPushButton*>("liveQueueStart")->isEnabled());
    CHECK(window.findChild<QPushButton*>("liveQueueAppend")->isEnabled());
    CHECK(window.findChild<QPushButton*>("liveQueuePause")->isEnabled());
    CHECK(!window.findChild<QPushButton*>("liveQueueLeave")->isEnabled());
    CHECK(!exportButton->isEnabled());
    CHECK(window.findChild<QLabel*>("liveQueueStatus")->text().contains("PREP ONLY"));
    const auto screenshot=qEnvironmentVariable("GRAVITINO_SET_QA_IMAGE");
    if(!screenshot.isEmpty()) CHECK(window.grab().save(screenshot));
    live.stop();
    QElapsedTimer deadline; deadline.start();
    while(!live.leave(&error) && deadline.elapsed()<5000) { app.processEvents(); QThread::msleep(1); }
    CHECK(!live.protectedRouting()); CHECK(exportButton->isEnabled());
    std::printf("Set render window: %d failures\n",failures); return failures ? 1 : 0;
}
