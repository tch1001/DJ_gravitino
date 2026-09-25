// A separate, non-destructive set queue and background WAV export surface.
#pragma once
#include "../transitions/SetRenderer.h"
#include <QFutureWatcher>
#include <QMainWindow>
#include <atomic>
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
namespace gvt {
class TrackLibrary;
class TransitionStore;
class StemSeparator;
class LiveSetSession;
class SetRenderWindow final : public QMainWindow {
    Q_OBJECT
public:
    SetRenderWindow(TrackLibrary* library, TransitionStore* store,
                    StemSeparator* stems=nullptr, QWidget* parent=nullptr);
    ~SetRenderWindow() override;
    void setRequest(const SetRenderRequest& request);
    bool openSet(const QString& path);
    void exportTo(const QString& path);
    void showRecording(const QString& path);
    void enableLiveQueue(LiveSetSession* session);
signals:
    void exportFinished(const QString& path, bool success);
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void refreshAvailable();
    void refreshQueue();
    void addChecked();
    void moveSelected(int delta);
    void fromSongList();
    void chooseAssets();
    void prepareStems();
    void saveSet();
    SetRenderRequest snapshot() const;
    TrackLibrary* library_;
    TransitionStore* store_;
    StemSeparator* stems_;
    SetRenderRequest request_;
    std::vector<GvtFile> available_;
    QWidget* editing_;
    QLineEdit* title_;
    QLineEdit* search_;
    QCheckBox* legacy_;
    QCheckBox* keyLock_;
    QTableWidget* availableTable_;
    QTableWidget* queue_;
    QLabel* status_;
    QProgressBar* progress_;
    QPushButton* export_;
    QPushButton* cancel_;
    QPushButton* open_;
    QPushButton* prepare_;
    QFutureWatcher<SetRenderResult> watcher_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    QString completedPath_;
    LiveSetSession* live_=nullptr;
    QLabel* liveStatus_=nullptr;
    QPushButton* liveStart_=nullptr;
    QPushButton* liveAppend_=nullptr;
    QPushButton* livePause_=nullptr;
    QPushButton* liveStop_=nullptr;
    QPushButton* liveLeave_=nullptr;
};
}
