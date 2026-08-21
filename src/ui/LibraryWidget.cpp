#include "LibraryWidget.h"
#include "FitButton.h"
#include "Theme.h"

#include <QAbstractTableModel>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTimer>
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

// ------------------------------------------------------ TransitionEdgeModel

constexpr int kTransitionFromPlayingRole = Qt::UserRole + 1;

// Read-only graph edge list over every saved transition. Each row is one
// directed From -> To edge, which makes possible set routes easy to scan and
// sort without first loading a pair onto the decks.
class TransitionEdgeModel : public QAbstractTableModel {
public:
    enum Col { ColFrom, ColArrow, ColTo, ColName, ColBpm, ColLength,
               ColCues, ColCount };

    TransitionEdgeModel(TransitionStore* store, AudioEngine* engine,
                        QObject* parent)
        : QAbstractTableModel(parent), store_(store), engine_(engine)
    {
        if (store_) {
            connect(store_, &TransitionStore::changed, this, [this] {
                beginResetModel();
                endResetModel();
            });
        }
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() || !store_ ? 0 : (int)store_->all().size();
    }

    int columnCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : ColCount;
    }

    QVariant data(const QModelIndex& idx, int role) const override
    {
        if (!store_ || !idx.isValid() ||
            idx.row() >= (int)store_->all().size())
            return {};
        const GvtFile& file = store_->all()[(size_t)idx.row()];
        if (role == kTransitionFromPlayingRole) {
            if (!engine_) return false;
            for (int deck = 0; deck < kNumDecks; ++deck) {
                if (!engine_->deck(deck).playing.load()) continue;
                const TrackDataPtr track = engine_->deck(deck).track();
                if (track && isReliableTrackMatch(
                                 matchTrack(file.from, *track))) {
                    return true;
                }
            }
            return false;
        }
        double endBeat = 0.0;
        for (const GvtEvent& event : file.events)
            endBeat = std::max(endBeat, event.beat);
        if (role == Qt::TextAlignmentRole) {
            if (idx.column() == ColArrow || idx.column() == ColBpm ||
                idx.column() == ColLength || idx.column() == ColCues)
                return (int)(Qt::AlignCenter | Qt::AlignVCenter);
            return {};
        }
        if (role == Qt::ToolTipRole) {
            const QString from = file.from.artist.isEmpty()
                                     ? file.from.title
                                     : QStringLiteral("%1 — %2")
                                           .arg(file.from.artist, file.from.title);
            const QString to = file.to.artist.isEmpty()
                                   ? file.to.title
                                   : QStringLiteral("%1 — %2")
                                         .arg(file.to.artist, file.to.title);
            return QObject::tr("%1 → %2\nTransition: %3")
                .arg(from, to, file.name);
        }
        // Keep table sorting natural: BPM, length and cue counts sort as
        // numbers, while the descriptive columns remain case-insensitive
        // strings through the proxy's sort role.
        if (role == Qt::UserRole) {
            switch (idx.column()) {
            case ColFrom:   return file.from.title;
            case ColArrow:  return QStringLiteral("→");
            case ColTo:     return file.to.title;
            case ColName:   return file.name;
            case ColBpm:    return file.masterBpm;
            case ColLength: return endBeat;
            case ColCues:   return (int)file.cues.size();
            }
        }
        if (role != Qt::DisplayRole) return {};
        switch (idx.column()) {
        case ColFrom:
            return file.from.title.isEmpty() ? QObject::tr("Unknown track")
                                             : file.from.title;
        case ColArrow: return QStringLiteral("→");
        case ColTo:
            return file.to.title.isEmpty() ? QObject::tr("Unknown track")
                                           : file.to.title;
        case ColName:
            return file.name.isEmpty() ? QObject::tr("Untitled transition")
                                       : file.name;
        case ColBpm:
            return file.masterBpm > 0.0
                       ? QString::number(file.masterBpm, 'f', 1)
                       : QString();
        case ColLength:
            return QObject::tr("%1 beats").arg(endBeat, 0, 'f', 1);
        case ColCues: return (int)file.cues.size();
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case ColFrom:   return QObject::tr("From track");
        case ColArrow:  return QStringLiteral("→");
        case ColTo:     return QObject::tr("To track");
        case ColName:   return QObject::tr("Transition");
        case ColBpm:    return QObject::tr("BPM");
        case ColLength: return QObject::tr("Length");
        case ColCues:   return QObject::tr("Cues");
        }
        return {};
    }

private:
    TransitionStore* store_;
    AudioEngine* engine_;
};

// The user's chosen column/order remains the secondary sort. The live
// currently-playing FROM group is always pinned first, including when the
// secondary column is descending.
class TransitionSortProxy : public QSortFilterProxyModel {
public:
    TransitionSortProxy(AudioEngine* engine, QObject* parent)
        : QSortFilterProxyModel(parent), engine_(engine)
    {
    }

    void refreshPlayingPriority()
    {
        std::array<QString, kNumDecks> current;
        if (engine_) {
            for (int deck = 0; deck < kNumDecks; ++deck) {
                if (!engine_->deck(deck).playing.load()) continue;
                const TrackDataPtr track = engine_->deck(deck).track();
                if (!track) continue;
                current[static_cast<std::size_t>(deck)] =
                    !track->fingerprint.isEmpty()
                        ? track->fingerprint : track->filePath;
            }
        }
        if (current == playingTrackKeys_)
            return;
        playingTrackKeys_ = std::move(current);
        invalidate();
        sort(sortColumn(), sortOrder());
    }

protected:
    bool lessThan(const QModelIndex& left,
                  const QModelIndex& right) const override
    {
        const bool leftPlaying =
            left.data(kTransitionFromPlayingRole).toBool();
        const bool rightPlaying =
            right.data(kTransitionFromPlayingRole).toBool();
        if (leftPlaying != rightPlaying) {
            // QSortFilterProxyModel reverses this comparator for descending
            // sorts, so reverse our group key too to keep playing rows first.
            return sortOrder() == Qt::AscendingOrder
                ? leftPlaying : !leftPlaying;
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    AudioEngine* engine_ = nullptr;
    std::array<QString, kNumDecks> playingTrackKeys_ {};
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
                             TransitionStore* transitions, History* history,
                             QWidget* parent)
    : QWidget(parent), library_(library), engine_(engine),
      transitions_(transitions), history_(history)
{
    setObjectName(QStringLiteral("libraryWidget"));
    setProperty("panel", true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(4);

    // Top chrome row: header, search, right-aligned segmented tabs
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
    libraryTabBtn_ = new FitPushButton(tr("Library"));
    historyTabBtn_ = new FitPushButton(tr("History"));
    transitionTabBtn_ = new FitPushButton(tr("Transitions"));
    for (QPushButton* b : {libraryTabBtn_, historyTabBtn_, transitionTabBtn_}) {
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedHeight(20);
        tabs->addWidget(b);
    }
    libraryTabBtn_->setChecked(true);
    topRow->addLayout(tabs);
    topRow->addSpacing(6);

    loadABtn_ = new FitPushButton(tr("Load ▶ A"));
    loadBBtn_ = new FitPushButton(tr("Load ▶ B"));
    const auto loadStyle = [](const QColor& accent) {
        return QStringLiteral(
            "QPushButton { color:%1; font-weight:bold; }"
            "QPushButton:disabled { color:#555b66; background:#252830; "
            "border-color:#30343c; }")
            .arg(accent.name());
    };
    loadABtn_->setStyleSheet(loadStyle(deckAccent(0)));
    loadBBtn_->setStyleSheet(loadStyle(deckAccent(1)));
    topRow->addWidget(loadABtn_);
    topRow->addWidget(loadBBtn_);
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
        historyPageIndex_ = stack_->addWidget(historyTable_);
    } else {
        historyTabBtn_->hide();
    }

    transitionModel_ = new TransitionEdgeModel(transitions_, engine_, this);
    transitionProxy_ = new TransitionSortProxy(engine_, this);
    transitionProxy_->setSourceModel(transitionModel_);
    transitionProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    transitionProxy_->setFilterKeyColumn(-1);
    transitionProxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    transitionProxy_->setSortRole(Qt::UserRole);
    transitionTable_ = new QTableView;
    transitionTable_->setModel(transitionProxy_);
    transitionTable_->setSortingEnabled(true);
    transitionTable_->sortByColumn(TransitionEdgeModel::ColFrom,
                                   Qt::AscendingOrder);
    transitionTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    transitionTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    transitionTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transitionTable_->verticalHeader()->setVisible(false);
    transitionTable_->horizontalHeader()->setStretchLastSection(false);
    transitionTable_->horizontalHeader()->setSectionResizeMode(
        TransitionEdgeModel::ColFrom, QHeaderView::Stretch);
    transitionTable_->horizontalHeader()->setSectionResizeMode(
        TransitionEdgeModel::ColArrow, QHeaderView::ResizeToContents);
    transitionTable_->horizontalHeader()->setSectionResizeMode(
        TransitionEdgeModel::ColTo, QHeaderView::Stretch);
    transitionTable_->horizontalHeader()->setSectionResizeMode(
        TransitionEdgeModel::ColName, QHeaderView::Stretch);
    for (int col : {TransitionEdgeModel::ColBpm,
                    TransitionEdgeModel::ColLength,
                    TransitionEdgeModel::ColCues})
        transitionTable_->horizontalHeader()->setSectionResizeMode(
            col, QHeaderView::ResizeToContents);
    transitionTable_->setAlternatingRowColors(true);
    transitionPageIndex_ = stack_->addWidget(transitionTable_);

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
        transitionProxy_->setFilterFixedString(t);
    });
    connect(loadABtn_, &QPushButton::clicked, this,
            [this] { loadSelectedTo(0); });
    connect(loadBBtn_, &QPushButton::clicked, this,
            [this] { loadSelectedTo(1); });
    connect(table_, &QTableView::doubleClicked, this,
            &LibraryWidget::onDoubleClicked);
    connect(transitionTable_, &QTableView::clicked, this,
            &LibraryWidget::onTransitionClicked);
    connect(libraryTabBtn_, &QPushButton::clicked, this,
            [this] { showTab(0); });
    connect(historyTabBtn_, &QPushButton::clicked, this,
            [this] { showTab(1); });
    connect(transitionTabBtn_, &QPushButton::clicked, this,
            [this] { showTab(2); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this] { updateLoadButtons(); });
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

    loadStateTimer_ = new QTimer(this);
    loadStateTimer_->setInterval(100);
    connect(loadStateTimer_, &QTimer::timeout, this, [this] {
        updateLoadButtons();
        transitionProxy_->refreshPlayingPriority();
    });
    loadStateTimer_->start();
    updateLoadButtons();
    transitionProxy_->refreshPlayingPriority();
}

void LibraryWidget::showTab(int index)
{
    const bool lib = index == 0;
    const bool history = index == 1 && historyPageIndex_ >= 0;
    const bool transitions = index == 2 && transitionPageIndex_ >= 0;
    if (history)
        stack_->setCurrentIndex(historyPageIndex_);
    else if (transitions)
        stack_->setCurrentIndex(transitionPageIndex_);
    else
        stack_->setCurrentIndex(0);

    libraryTabBtn_->setChecked(lib);
    historyTabBtn_->setChecked(history);
    transitionTabBtn_->setChecked(transitions);
    libraryTabBtn_->setStyleSheet(
        QLatin1String(lib ? kTabStyleActive : kTabStyleIdle));
    historyTabBtn_->setStyleSheet(
        QLatin1String(history ? kTabStyleActive : kTabStyleIdle));
    transitionTabBtn_->setStyleSheet(
        QLatin1String(transitions ? kTabStyleActive : kTabStyleIdle));
    search_->setEnabled(lib || transitions);
    search_->setPlaceholderText(
        transitions ? tr("Search transition edges…")
                    : tr("Search title / artist…"));
    crateTree_->setVisible(lib);
    updateLoadButtons();
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

void LibraryWidget::browseBy(int rows)
{
    if (rows == 0)
        return;

    // Hardware browsing always targets tracks, even if History or the
    // transition edge list was most recently open.
    showTab(0);
    const int count = proxy_->rowCount();
    if (count <= 0) {
        emit statusMessage(tr("The library has no tracks to select"), 3000);
        return;
    }

    int row = table_->currentIndex().row();
    // Treat no current row as just outside the relevant edge so one tick
    // lands on the first/last visible track. Accelerated ticks still skip the
    // corresponding number of rows.
    if (row < 0)
        row = rows > 0 ? -1 : count;
    row = std::clamp(row + rows, 0, count - 1);

    const QModelIndex index = proxy_->index(row, 0);
    table_->setCurrentIndex(index);
    table_->selectRow(row);
    table_->scrollTo(index, QAbstractItemView::EnsureVisible);
    table_->setFocus(Qt::OtherFocusReason);
    updateLoadButtons();
}

void LibraryWidget::confirmBrowseSelection()
{
    showTab(0);
    if (proxy_->rowCount() <= 0) {
        emit statusMessage(tr("The library has no tracks to select"), 3000);
        return;
    }
    if (!table_->currentIndex().isValid()) {
        browseBy(1);
        return;
    }

    const int row = table_->currentIndex().row();
    table_->selectRow(row);
    table_->scrollTo(proxy_->index(row, 0),
                     QAbstractItemView::EnsureVisible);
    table_->setFocus(Qt::OtherFocusReason);
    updateLoadButtons();
}

void LibraryWidget::loadSelectedTo(int deck)
{
    if (deck < 0 || deck >= kNumDecks)
        return;
    // A hardware LOAD should never act on a selection hidden behind History
    // or Transitions. Reveal the track library before resolving the row.
    showTab(0);
    if (engine_->deck(deck).playing.load()) {
        emit statusMessage(
            tr("⚠ Stop deck %1 before loading another track")
                .arg(deck == 0 ? QStringLiteral("A") : QStringLiteral("B")),
            4000);
        updateLoadButtons();
        return;
    }
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

int LibraryWidget::trackRowFor(const GvtTrackRef& ref) const
{
    int bestRow = -1;
    MatchQuality best = MatchQuality::None;
    for (int row = 0; row < library_->trackCount(); ++row) {
        const TrackDataPtr track = library_->trackAt(row);
        if (!track) continue;
        const MatchQuality quality = matchTrack(ref, *track);
        if (quality > best) {
            best = quality;
            bestRow = row;
        }
    }
    return isReliableTrackMatch(best) ? bestRow : -1;
}

void LibraryWidget::onTransitionClicked(const QModelIndex& proxyIndex)
{
    if (!transitions_ || !proxyIndex.isValid()) return;
    const QModelIndex source = transitionProxy_->mapToSource(proxyIndex);
    if (!source.isValid() || source.row() < 0 ||
        source.row() >= static_cast<int>(transitions_->all().size())) {
        return;
    }
    const GvtFile& transition =
        transitions_->all()[static_cast<std::size_t>(source.row())];

    const bool playingA = engine_->deck(0).playing.load();
    const bool playingB = engine_->deck(1).playing.load();
    const int playingCount = static_cast<int>(playingA) +
                             static_cast<int>(playingB);
    if (playingCount == 2) {
        emit statusMessage(
            tr("⚠ Transition not loaded: both decks are playing, so neither track can be replaced"),
            5500);
        return;
    }

    const int fromRow = trackRowFor(transition.from);
    const int toRow = trackRowFor(transition.to);
    if (playingCount == 0) {
        if (fromRow < 0 || toRow < 0) {
            emit statusMessage(
                tr("Transition tracks are not ready in the current library"),
                5000);
            return;
        }
        loadRowTo(fromRow, 0);
        loadRowTo(toRow, 1);
        emit statusMessage(
            tr("Loaded transition '%1': FROM on deck A, TO on deck B")
                .arg(transition.name),
            5000);
        return;
    }

    const int playingDeck = playingA ? 0 : 1;
    const TrackDataPtr playingTrack = engine_->deck(playingDeck).track();
    if (!playingTrack ||
        !isReliableTrackMatch(matchTrack(transition.from, *playingTrack))) {
        emit statusMessage(
            tr("Transition not loaded: the playing track does not match its FROM track"),
            5500);
        return;
    }
    if (toRow < 0) {
        emit statusMessage(
            tr("Transition TO track is not ready in the current library"),
            5000);
        return;
    }

    const int toDeck = 1 - playingDeck;
    loadRowTo(toRow, toDeck);
    emit statusMessage(
        tr("FROM matched deck %1; loaded TO on deck %2")
            .arg(playingDeck == 0 ? QStringLiteral("A")
                                  : QStringLiteral("B"))
            .arg(toDeck == 0 ? QStringLiteral("A")
                             : QStringLiteral("B")),
        5000);
}

void LibraryWidget::loadRowTo(int sourceRow, int deck)
{
    if (deck < 0 || deck >= kNumDecks)
        return;
    if (engine_->deck(deck).playing.load()) {
        emit statusMessage(
            tr("⚠ Stop deck %1 before loading another track")
                .arg(deck == 0 ? QStringLiteral("A") : QStringLiteral("B")),
            4000);
        updateLoadButtons();
        return;
    }
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
    updateLoadButtons();
}

void LibraryWidget::updateLoadButtons()
{
    if (!loadABtn_ || !loadBBtn_) return;
    const bool libraryPage = stack_ && stack_->currentIndex() == 0;
    const int sourceRow = sourceRowFor(table_->currentIndex());
    const bool selected = sourceRow >= 0;
    const bool ready = selected && library_->trackAt(sourceRow);

    const auto update = [&](QPushButton* button, int deck) {
        const bool playing = engine_->deck(deck).playing.load();
        button->setEnabled(libraryPage && ready && !playing);
        const QString deckName = deck == 0 ? QStringLiteral("A")
                                           : QStringLiteral("B");
        if (!libraryPage)
            button->setToolTip(tr("Open the Library tab to load a track"));
        else if (playing)
            button->setToolTip(
                tr("Stop deck %1 before loading another track").arg(deckName));
        else if (!selected)
            button->setToolTip(tr("Select a track first"));
        else if (!ready)
            button->setToolTip(tr("This track is still being analyzed"));
        else
            button->setToolTip(tr("Load the selected track onto deck %1")
                                   .arg(deckName));
    };
    update(loadABtn_, 0);
    update(loadBBtn_, 1);
}

} // namespace gvt
