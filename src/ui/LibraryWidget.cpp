#include "LibraryWidget.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

namespace gvt {

namespace {
// Filters on Title (col 0) and Artist (col 1) only.
class TitleArtistFilter : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        const QRegularExpression re = filterRegularExpression();
        if (re.pattern().isEmpty()) return true;
        for (int col : {0, 1}) {
            QModelIndex idx = sourceModel()->index(row, col, parent);
            if (sourceModel()->data(idx).toString().contains(re))
                return true;
        }
        return false;
    }
};
} // namespace

LibraryWidget::LibraryWidget(TrackLibrary* library, AudioEngine* engine,
                             QWidget* parent)
    : QWidget(parent), library_(library), engine_(engine)
{
    setObjectName(QStringLiteral("libraryWidget"));
    setProperty("panel", true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

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

    auto* loadA = new QPushButton(tr("Load ▶ A"));
    auto* loadB = new QPushButton(tr("Load ▶ B"));
    loadA->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                             .arg(deckAccent(0).name()));
    loadB->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                             .arg(deckAccent(1).name()));
    topRow->addWidget(loadA);
    topRow->addWidget(loadB);
    root->addLayout(topRow);

    proxy_ = new TitleArtistFilter(this);
    proxy_->setSourceModel(library_);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    table_ = new QTableView;
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    table_->setAlternatingRowColors(true);
    root->addWidget(table_, 1);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
        proxy_->setFilterFixedString(t);
    });
    connect(loadA, &QPushButton::clicked, this,
            [this] { loadSelectedTo(0); });
    connect(loadB, &QPushButton::clicked, this,
            [this] { loadSelectedTo(1); });
    connect(table_, &QTableView::doubleClicked, this,
            &LibraryWidget::onDoubleClicked);
    connect(library_, &TrackLibrary::scanProgress, this,
            [this](int analyzed, int total) {
                emit statusMessage(
                    tr("Analyzing library… %1 / %2").arg(analyzed).arg(total),
                    2000);
            });
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
