#pragma once
#include <QWidget>
#include "../audio/AudioEngine.h"
#include "../library/History.h"
#include "../library/TrackLibrary.h"

class QLineEdit;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTableView;
class QTreeWidget;
class QTreeWidgetItem;

namespace gvt {

class CrateFilterProxy;   // path-prefix + title/artist search proxy
class HistoryModel;       // table model over gvt::History (newest first)

// Serato-style library chrome: a collapsible left crate sidebar (one crate
// per subdirectory of the scanned folder), a right-aligned [Library]
// [History] segmented tab control, and the sortable/searchable track table
// (or the session-history table) underneath. Load-to-deck buttons and
// double-click loading as before.
class LibraryWidget : public QWidget {
    Q_OBJECT
public:
    LibraryWidget(TrackLibrary* library, AudioEngine* engine,
                  History* history = nullptr, QWidget* parent = nullptr);

signals:
    void trackLoaded(int deck);          // a track was loaded onto deck 0/1
    void statusMessage(const QString& msg, int timeoutMs);

private slots:
    void loadSelectedTo(int deck);
    void onDoubleClicked(const QModelIndex& proxyIndex);
    void rebuildCrates();                // model rows changed
    void onCrateSelected();
    void showTab(int index);             // 0 = Library, 1 = History

private:
    int sourceRowFor(const QModelIndex& proxyIndex) const;
    void loadRowTo(int sourceRow, int deck);

    TrackLibrary* library_;
    AudioEngine* engine_;
    History* history_;
    CrateFilterProxy* proxy_ = nullptr;
    HistoryModel* historyModel_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QTreeWidget* crateTree_ = nullptr;
    QTreeWidgetItem* allTracksItem_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QTableView* table_ = nullptr;        // library page
    QTableView* historyTable_ = nullptr; // history page
    QLineEdit* search_ = nullptr;
    QPushButton* libraryTabBtn_ = nullptr;
    QPushButton* historyTabBtn_ = nullptr;
};

} // namespace gvt
