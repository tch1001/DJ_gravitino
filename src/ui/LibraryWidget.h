#pragma once
#include <QWidget>
#include "../audio/AudioEngine.h"
#include "../library/TrackLibrary.h"

class QLineEdit;
class QSortFilterProxyModel;
class QTableView;

namespace gvt {

// Sortable/searchable track table over the TrackLibrary model, with
// load-to-deck buttons and double-click loading.
class LibraryWidget : public QWidget {
    Q_OBJECT
public:
    LibraryWidget(TrackLibrary* library, AudioEngine* engine,
                  QWidget* parent = nullptr);

signals:
    void trackLoaded(int deck);          // a track was loaded onto deck 0/1
    void statusMessage(const QString& msg, int timeoutMs);

private slots:
    void loadSelectedTo(int deck);
    void onDoubleClicked(const QModelIndex& proxyIndex);

private:
    int sourceRowFor(const QModelIndex& proxyIndex) const;
    void loadRowTo(int sourceRow, int deck);

    TrackLibrary* library_;
    AudioEngine* engine_;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QLineEdit* search_ = nullptr;
};

} // namespace gvt
