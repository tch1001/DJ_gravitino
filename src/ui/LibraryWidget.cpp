#include "LibraryWidget.h"
#include "Theme.h"

#include <QAbstractTableModel>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace gvt {

// --------------------------------------------------------- CrateFilterProxy

// Composes two filters over the TrackLibrary model:
//  - a crate path prefix ("" = All Tracks): the track's file path must live
//    under that directory;
//  - the search box regexp over Title (col 0) and Artist (col 1).
class CrateFilterProxy : public QSortFilterProxyModel {
public:
    CrateFilterProxy(TrackLibrary* lib, QObject* parent)
        : QSortFilterProxyModel(parent), lib_(lib)
    {
    }

    // dir = crate directory (no trailing slash); empty = no path filter.
    void setCratePath(const QString& dir)
    {
        beginFilterChange();
        cratePrefix_ = dir.isEmpty() ? QString()
                                     : dir + QStringLiteral("/");
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }
    QString cratePath() const
    {
        return cratePrefix_.isEmpty() ? QString()
                                      : cratePrefix_.chopped(1);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        if (!cratePrefix_.isEmpty() &&
            !lib_->pathAt(row).startsWith(cratePrefix_))
            return false;
        const QRegularExpression re = filterRegularExpression();
        if (re.pattern().isEmpty()) return true;
        for (int col : {0, 1}) {
            QModelIndex idx = sourceModel()->index(row, col, parent);
            if (sourceModel()->data(idx).toString().contains(re))
                return true;
        }
        return false;
    }

private:
    TrackLibrary* lib_;
    QString cratePrefix_; // with trailing '/', or empty
};

// ------------------------------------------------------------- HistoryModel

// Read-only table over gvt::History entries (already newest-first).
// Columns: Time, Title, Artist, BPM, Key, Deck.
class HistoryModel : public QAbstractTableModel {
public:
    enum Col { ColTime, ColTitle, ColArtist, ColBpm, ColKey, ColDeck,
               ColCount };

    HistoryModel(History* history, QObject* parent)
        : QAbstractTableModel(parent), history_(history)
    {
        // History prepends the entry before emitting, so the row already
        // exists in the store; announce the insertion at index 0.
        connect(history_, &History::entryAdded, this, [this] {
            beginInsertRows(QModelIndex(), 0, 0);
            endInsertRows();
        });
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : (int)history_->entries().size();
    }
    int columnCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : ColCount;
    }

    QVariant data(const QModelIndex& idx, int role) const override
    {
        const auto& entries = history_->entries();
        if (!idx.isValid() || idx.row() >= (int)entries.size())
            return {};
        const History::Entry& e = entries[(size_t)idx.row()];
        if (role == Qt::TextAlignmentRole) {
            if (idx.column() == ColBpm || idx.column() == ColKey ||
                idx.column() == ColDeck)
                return (int)(Qt::AlignRight | Qt::AlignVCenter);
            return {};
        }
        if (role != Qt::DisplayRole) return {};
        switch (idx.column()) {
        case ColTime:
            return e.startedAt.toString(
                QStringLiteral("MMM d  hh:mm:ss"));
        case ColTitle:  return e.title;
        case ColArtist: return e.artist;
        case ColBpm:
            return e.bpm > 0.0 ? QString::asprintf("%.1f", e.bpm)
                               : QString();
        case ColKey:    return e.key;
        case ColDeck:
            return e.deck == 0 ? QStringLiteral("A") : QStringLiteral("B");
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation o,
                        int role) const override
    {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case ColTime:   return QObject::tr("Loaded");
        case ColTitle:  return QObject::tr("Title");
        case ColArtist: return QObject::tr("Artist");
        case ColBpm:    return QObject::tr("BPM");
        case ColKey:    return QObject::tr("Key");
        case ColDeck:   return QObject::tr("Deck");
        }
        return {};
    }

private:
    History* history_;
};

// ------------------------------------------------------------ LibraryWidget

// The crate root is derived from the tracks themselves (the deepest common
// directory of every scanned path) so no TrackLibrary change is needed;
// crates are that root's immediate subdirectories, tracks counted
// recursively. Tracks sitting directly in the root only appear under
// "All Tracks".
static QString commonRootDir(const QStringList& dirs)
{
    if (dirs.isEmpty()) return {};
    QStringList parts = dirs.first().split(QLatin1Char('/'));
    for (const QString& d : dirs) {
        const QStringList p = d.split(QLatin1Char('/'));
        int n = 0;
        while (n < parts.size() && n < p.size() && parts[n] == p[n]) ++n;
        parts = parts.mid(0, n);
        if (parts.isEmpty()) break;
    }
    return parts.join(QLatin1Char('/'));
}

static const char* kTabStyleActive =
    "QPushButton { background:#35c8e8; color:black; font-weight:bold; "
    "border-radius:0px; padding: 2px 12px; }";
static const char* kTabStyleIdle =
    "QPushButton { background:#2a2e37; color:#8a909c; "
    "border-radius:0px; padding: 2px 12px; }";

LibraryWidget::LibraryWidget(TrackLibrary* library, AudioEngine* engine,
                             History* history, QWidget* parent)
    : QWidget(parent), library_(library), engine_(engine), history_(history)
{
    setObjectName(QStringLiteral("libraryWidget"));
    setProperty("panel", true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(4);

    // Top chrome row: header, search, right-aligned [Library][History]
    // segmented tabs, load buttons.
    auto* topRow = new QHBoxLayout;
    auto* header = new QLabel(tr("LIBRARY"));
    header->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                         "letter-spacing:2px;")
                              .arg(themeText().name()));
    topRow->addWidget(header);

    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search title / artist…"));
    search_->setClearButtonEnabled(true);
    topRow->addWidget(search_, 1);

    // Serato-style segmented tab control (right side of the chrome row,
    // directly above the table).
    auto* tabs = new QHBoxLayout;
    tabs->setSpacing(0);
    libraryTabBtn_ = new QPushButton(tr("Library"));
    historyTabBtn_ = new QPushButton(tr("History"));
    for (QPushButton* b : {libraryTabBtn_, historyTabBtn_}) {
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedHeight(20);
        tabs->addWidget(b);
    }
    libraryTabBtn_->setChecked(true);
    topRow->addLayout(tabs);
    topRow->addSpacing(6);

    auto* loadA = new QPushButton(tr("Load ▶ A"));
    auto* loadB = new QPushButton(tr("Load ▶ B"));
    loadA->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                             .arg(deckAccent(0).name()));
    loadB->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                             .arg(deckAccent(1).name()));
    topRow->addWidget(loadA);
    topRow->addWidget(loadB);
    root->addLayout(topRow);

    proxy_ = new CrateFilterProxy(library_, this);
    proxy_->setSourceModel(library_);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    // Left crate sidebar (collapsible via the splitter handle).
    crateTree_ = new QTreeWidget;
    crateTree_->setHeaderHidden(true);
    crateTree_->setRootIsDecorated(false);
    crateTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    crateTree_->setMinimumWidth(90);
    crateTree_->setStyleSheet(
        QStringLiteral("QTreeWidget { border: 1px solid #383d48; }"
                       "QTreeWidget::item { height: 20px; }"));

    // Library table.
    table_ = new QTableView;
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setAlternatingRowColors(true);

    stack_ = new QStackedWidget;
    stack_->addWidget(table_);

    // History table (only when a History store was provided).
    if (history_) {
        historyModel_ = new HistoryModel(history_, this);
        historyTable_ = new QTableView;
        historyTable_->setModel(historyModel_);
        historyTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        historyTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        historyTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        historyTable_->verticalHeader()->setVisible(false);
        historyTable_->horizontalHeader()->setStretchLastSection(true);
        historyTable_->horizontalHeader()->setSectionResizeMode(
            HistoryModel::ColTitle, QHeaderView::Stretch);
        historyTable_->setAlternatingRowColors(true);
        stack_->addWidget(historyTable_);
    } else {
        historyTabBtn_->hide();
    }

    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->addWidget(crateTree_);
    splitter_->addWidget(stack_);
    splitter_->setCollapsible(0, true);
    splitter_->setCollapsible(1, false);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({170, 800});
    root->addWidget(splitter_, 1);

    rebuildCrates();

    // -- wiring --
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
        proxy_->setFilterFixedString(t);
    });
    connect(loadA, &QPushButton::clicked, this,
            [this] { loadSelectedTo(0); });
    connect(loadB, &QPushButton::clicked, this,
            [this] { loadSelectedTo(1); });
    connect(table_, &QTableView::doubleClicked, this,
            &LibraryWidget::onDoubleClicked);
    connect(libraryTabBtn_, &QPushButton::clicked, this,
            [this] { showTab(0); });
    connect(historyTabBtn_, &QPushButton::clicked, this,
            [this] { showTab(1); });
    showTab(0); // initial tab styles
    connect(crateTree_, &QTreeWidget::itemSelectionChanged, this,
            &LibraryWidget::onCrateSelected);
    // Crate list follows the model's rows (rescan or incremental changes).
    connect(library_, &QAbstractItemModel::modelReset, this,
            &LibraryWidget::rebuildCrates);
    connect(library_, &QAbstractItemModel::rowsInserted, this,
            &LibraryWidget::rebuildCrates);
    connect(library_, &QAbstractItemModel::rowsRemoved, this,
            &LibraryWidget::rebuildCrates);
    connect(library_, &TrackLibrary::scanProgress, this,
            [this](int analyzed, int total) {
                emit statusMessage(
                    tr("Analyzing library… %1 / %2").arg(analyzed).arg(total),
                    2000);
            });
}

void LibraryWidget::showTab(int index)
{
    stack_->setCurrentIndex(historyTable_ ? index : 0);
    const bool lib = index == 0 || !historyTable_;
    libraryTabBtn_->setChecked(lib);
    historyTabBtn_->setChecked(!lib);
    libraryTabBtn_->setStyleSheet(
        QLatin1String(lib ? kTabStyleActive : kTabStyleIdle));
    historyTabBtn_->setStyleSheet(
        QLatin1String(lib ? kTabStyleIdle : kTabStyleActive));
    // Search + crates only apply to the library page.
    search_->setEnabled(lib);
    crateTree_->setEnabled(lib);
}

void LibraryWidget::rebuildCrates()
{
    const QString selected =
        proxy_ ? proxy_->cratePath() : QString();

    // root = deepest common directory of all track paths; crates = its
    // immediate subdirectories, tracks counted recursively.
    QStringList dirs;
    const int rows = library_->trackCount();
    dirs.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        const QString p = library_->pathAt(i);
        if (!p.isEmpty()) dirs << QFileInfo(p).path();
    }
    const QString rootDir = commonRootDir(dirs);

    std::map<QString, int> crateCounts; // name -> recursive track count
    if (!rootDir.isEmpty()) {
        const int prefixLen = rootDir.length() + 1;
        for (int i = 0; i < rows; ++i) {
            const QString p = library_->pathAt(i);
            if (p.length() <= prefixLen) continue;
            const QString rel = p.mid(prefixLen);
            const int slash = rel.indexOf(QLatin1Char('/'));
            if (slash > 0) crateCounts[rel.left(slash)]++;
        }
    }

    QSignalBlocker block(crateTree_); // reselect below without re-filtering
    crateTree_->clear();
    allTracksItem_ = new QTreeWidgetItem(
        crateTree_,
        {tr("All Tracks  (%1)").arg(rows)});
    allTracksItem_->setData(0, Qt::UserRole, QString());
    QTreeWidgetItem* toSelect = allTracksItem_;
    for (const auto& [name, count] : crateCounts) {
        auto* item = new QTreeWidgetItem(
            crateTree_,
            {QStringLiteral("%1  (%2)").arg(name).arg(count)});
        const QString dir = rootDir + QLatin1Char('/') + name;
        item->setData(0, Qt::UserRole, dir);
        item->setToolTip(0, dir);
        if (dir == selected) toSelect = item;
    }
    crateTree_->setCurrentItem(toSelect);
    const QString newPath =
        toSelect->data(0, Qt::UserRole).toString();
    if (newPath != selected)
        proxy_->setCratePath(newPath); // previous crate disappeared
}

void LibraryWidget::onCrateSelected()
{
    const auto items = crateTree_->selectedItems();
    proxy_->setCratePath(
        items.isEmpty() ? QString()
                        : items.first()->data(0, Qt::UserRole).toString());
}

int LibraryWidget::sourceRowFor(const QModelIndex& proxyIndex) const
{
    if (!proxyIndex.isValid()) return -1;
    return proxy_->mapToSource(proxyIndex).row();
}

void LibraryWidget::loadSelectedTo(int deck)
{
    int row = sourceRowFor(table_->currentIndex());
    if (row < 0) {
        emit statusMessage(tr("Select a track first"), 3000);
        return;
    }
    loadRowTo(row, deck);
}

void LibraryWidget::onDoubleClicked(const QModelIndex& proxyIndex)
{
    int row = sourceRowFor(proxyIndex);
    if (row < 0) return;
    // Load to the first stopped deck; if both playing, refuse politely.
    int deck = -1;
    for (int i = 0; i < kNumDecks; ++i) {
        if (!engine_->deck(i).playing.load()) { deck = i; break; }
    }
    if (deck < 0) {
        emit statusMessage(tr("Both decks are playing — stop one first"), 4000);
        return;
    }
    loadRowTo(row, deck);
}

void LibraryWidget::loadRowTo(int sourceRow, int deck)
{
    TrackDataPtr t = library_->trackAt(sourceRow);
    if (!t) {
        emit statusMessage(tr("Track is still analyzing — try again shortly"),
                           4000);
        return;
    }
    engine_->deck(deck).loadTrack(t); // direct API per contract
    emit statusMessage(tr("Loaded \"%1\" to deck %2")
                           .arg(t->title.isEmpty() ? t->filePath : t->title)
                           .arg(deck == 0 ? QStringLiteral("A")
                                          : QStringLiteral("B")),
                       4000);
    emit trackLoaded(deck);
}

} // namespace gvt
