#pragma once
#include <QWidget>
#include "../audio/AudioEngine.h"
#include "../library/History.h"
#include "../library/TrackLibrary.h"

class QLineEdit;
class QPoint;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTableView;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace gvt {

class CrateFilterProxy;   // path-prefix + title/artist search proxy
class HistoryModel;       // table model over gvt::History (newest first)
class TransitionEdgeModel; // all saved transitions as from -> to rows
class TransitionSortProxy; // pins currently-playing FROM tracks above the rest

// Serato-style library chrome: a collapsible left crate sidebar (one crate
// per subdirectory of the scanned folder), a right-aligned [Library]
// [History] [Transitions] segmented tab control, and sortable track/history/
// transition-edge tables underneath. Load-to-deck buttons never replace a
// playing deck.
class LibraryWidget : public QWidget {
    Q_OBJECT
public:
    LibraryWidget(TrackLibrary* library, AudioEngine* engine,
                  TransitionStore* transitions, History* history = nullptr,
                  QWidget* parent = nullptr);

signals:
    void trackLoaded(int deck);          // a track was loaded onto deck 0/1
    void statusMessage(const QString& msg, int timeoutMs);
    void transitionSelected(const QString& filePath);
    void transitionEditRequested(const QString& filePath);

public slots:
    void browseBy(int rows);             // physical browser encoder
    void confirmBrowseSelection();       // physical browser encoder press
    void loadSelectedTo(int deck);       // physical or on-screen LOAD
    void setTransitionEditingEnabled(bool enabled);

private slots:
    void onDoubleClicked(const QModelIndex& proxyIndex);
    void onTransitionClicked(const QModelIndex& proxyIndex);
    void rebuildCrates();                // model rows changed
    void onCrateSelected();
    void showTab(int index);             // 0 Library, 1 History, 2 Transitions
    void updateLoadButtons();
    void updateTransitionButtons();
    void showTransitionContextMenu(const QPoint& pos);
    void renameSelectedTransition();
    void deleteSelectedTransition();

private:
    int sourceRowFor(const QModelIndex& proxyIndex) const;
    int trackRowFor(const GvtTrackRef& ref) const;
    void loadRowTo(int sourceRow, int deck);
    int selectedTransitionSourceRow() const;

    TrackLibrary* library_;
    AudioEngine* engine_;
    TransitionStore* transitions_;
    History* history_;
    CrateFilterProxy* proxy_ = nullptr;
    HistoryModel* historyModel_ = nullptr;
    TransitionEdgeModel* transitionModel_ = nullptr;
    TransitionSortProxy* transitionProxy_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QTreeWidget* crateTree_ = nullptr;
    QTreeWidgetItem* allTracksItem_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QTableView* table_ = nullptr;        // library page
    QTableView* historyTable_ = nullptr; // history page
    QTableView* transitionTable_ = nullptr; // all transition edges
    int historyPageIndex_ = -1;
    int transitionPageIndex_ = -1;
    QLineEdit* search_ = nullptr;
    QPushButton* libraryTabBtn_ = nullptr;
    QPushButton* historyTabBtn_ = nullptr;
    QPushButton* transitionTabBtn_ = nullptr;
    QPushButton* renameTransitionBtn_ = nullptr;
    QPushButton* deleteTransitionBtn_ = nullptr;
    QPushButton* loadABtn_ = nullptr;
    QPushButton* loadBBtn_ = nullptr;
    QTimer* loadStateTimer_ = nullptr;
    bool transitionEditingEnabled_ = true;
    QString selectedTransitionPath_;
};

} // namespace gvt
