// Exercises native accessibility caches while the library reorders its rows.
// Synthetic models and temporary storage keep real songs/transitions untouched.
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QFile>
#include <QLineEdit>
#include <QPersistentModelIndex>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>
#include "audio/AudioEngine.h"
#include "control/ControlBus.h"
#include "library/TrackLibrary.h"
#include "ui/LibraryWidget.h"
#include "ui/QtAccessibilityWorkaround.h"

#include <cstdio>

#ifdef Q_OS_MACOS
void retireNativeTableRows(QAccessible::Id tableId);
#endif

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

QAccessible::Id cacheCells(QTableView& table, bool nativeCells)
{
    auto* accessible = QAccessible::queryAccessibleInterface(&table);
    CHECK(accessible && accessible->tableInterface());
    if (!accessible || !accessible->tableInterface()) return 0;
    const auto id = QAccessible::uniqueId(accessible);
    for (int row = 0; row < qMin(10, table.model()->rowCount()); ++row) {
        auto* cell = accessible->tableInterface()->cellAt(row, 0);
        CHECK(cell && cell->isValid());
        if (!cell) continue;
        CHECK(cell->text(QAccessible::Name) == table.model()->index(row, 0).data().toString());
        if (nativeCells) {
            // Force Cocoa to create real cells among synthesized placeholders.
            QAccessibleEvent focus(cell, QAccessible::Focus);
            QAccessible::updateAccessibility(&focus);
        }
        CHECK(QAccessible::accessibleInterface(id) == accessible);
    }
    return id;
}

void table_refreshes_keep_accessibility_alive(QApplication& app)
{
    QStandardItemModel source(40, 7);
    for (int row = 0; row < source.rowCount(); ++row)
        for (int column = 0; column < source.columnCount(); ++column)
            source.setData(source.index(row, column),
                           QStringLiteral("Song %1 / %2").arg(row).arg(column));
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&source);
    auto* tablePointer = new QTableView;
    auto& table = *tablePointer;
    table.setModel(&proxy);
    table.setSortingEnabled(true);
    table.resize(850, 400);
    table.show();
    app.processEvents();
    const auto tableId = QAccessible::uniqueId(QAccessible::queryAccessibleInterface(&table));
    for (int iteration = 0; iteration < 100; ++iteration) {
        // The first pass has Qt-only cached cells: destruction notifications
        // must not create a Cocoa cell that recursively deletes the table.
        CHECK(cacheCells(table, iteration % 2) == tableId);
        QPersistentModelIndex selected(proxy.index(2, 0));
        const auto selectedText = selected.data();
        table.setCurrentIndex(selected);
        proxy.invalidate();
        proxy.sort(0, iteration % 2 ? Qt::AscendingOrder : Qt::DescendingOrder);
        CHECK(selected.data() == selectedText);
        CHECK(table.currentIndex() == selected);
        proxy.setFilterFixedString(QStringLiteral("Song 1"));
        app.processEvents();
        cacheCells(table, true);
        proxy.setFilterFixedString({});
        source.insertRow(0);
        source.setData(source.index(0, 0), QStringLiteral("Inserted"));
        source.removeRow(0);
        app.processEvents();
        CHECK(QAccessible::accessibleInterface(tableId) ==
              QAccessible::queryAccessibleInterface(&table));
        CHECK(cacheCells(table, true) == tableId);
    }
    // Model reset/replacement and window destruction exercise the other cache
    // cleanup paths without disabling accessibility or leaking retired cells.
    QStandardItemModel replacement(3, 2);
    table.setModel(&replacement);
    app.processEvents();
    CHECK(cacheCells(table, true) == tableId);
    replacement.clear();
    app.processEvents();
    delete tablePointer;
    app.processEvents();
    CHECK(QAccessible::accessibleInterface(tableId) == nullptr);
}

void native_row_retirement_keeps_cells_alive_for_search(QApplication& app)
{
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return;
    QStandardItemModel source(40, 7);
    for (int row = 0; row < source.rowCount(); ++row)
        for (int column = 0; column < source.columnCount(); ++column)
            source.setData(source.index(row, column),
                           QStringLiteral("Song %1 / %2").arg(row).arg(column));
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&source);
    auto* tablePointer = new QTableView;
    auto& table = *tablePointer;
    table.setModel(&proxy);
    table.resize(850, 400);
    table.show();
    app.processEvents();
    auto* accessible = QAccessible::queryAccessibleInterface(&table);
    const auto tableId = QAccessible::uniqueId(accessible);
    auto* cell = accessible->tableInterface()->cellAt(2, 0);
    const auto cellId = QAccessible::uniqueId(cell);
    retireNativeTableRows(tableId);
    CHECK(QAccessible::accessibleInterface(tableId) == accessible);
    CHECK(QAccessible::accessibleInterface(cellId) == cell);
    // This is the reported rowsRemoved -> QAccessibleTable::modelChange path.
    // Unlike invalidate(), filtering does not clear the table's child-ID cache.
    proxy.setFilterFixedString(QStringLiteral("Song 1"));
    app.processEvents();
    CHECK(cacheCells(table, true) == tableId);
    QLineEdit search;
    QObject::connect(&search, &QLineEdit::textChanged,
                     &proxy, &QSortFilterProxyModel::setFilterFixedString);
    const QStringList searches{QString(), QStringLiteral("Song 1"),
        QStringLiteral("Song 12"), QStringLiteral("No matches"),
        QStringLiteral("Song 3"), QString()};
    QList<QAccessible::Id> finalCellIds;
    for (int iteration = 0; iteration < 30; ++iteration) {
        for (const QString& text : searches) {
            // Actual textChanged path with partial and empty search results;
            // preserve every live Qt cell ID across native-only retirement.
            search.setText(text);
            app.processEvents();
            QList<QAccessible::Id> ids;
            for (int row = 0; row < proxy.rowCount(); ++row)
                for (int column = 0; column < proxy.columnCount(); ++column)
                    ids.append(QAccessible::uniqueId(
                        accessible->tableInterface()->cellAt(row, column)));
            retireNativeTableRows(tableId);
            for (auto id : ids) {
                auto* current = QAccessible::accessibleInterface(id);
                CHECK(current && current->isValid());
                if (!current || !current->isValid()) continue;
                auto* position = current->tableCellInterface();
                CHECK(position);
                if (position)
                    CHECK(current->text(QAccessible::Name) ==
                          proxy.index(position->rowIndex(), position->columnIndex())
                              .data().toString());
            }
            CHECK(cacheCells(table, true) == tableId); // native cells recreate
            finalCellIds = ids;
        }
        source.insertRow(0);
        source.setData(source.index(0, 0), QStringLiteral("Inserted"));
        retireNativeTableRows(tableId);
        source.removeRow(0);
        proxy.sort(0, iteration % 2 ? Qt::AscendingOrder : Qt::DescendingOrder);
        retireNativeTableRows(tableId);
    }
    delete tablePointer;
    app.processEvents();
    CHECK(QAccessible::accessibleInterface(tableId) == nullptr);
    for (auto id : finalCellIds) CHECK(QAccessible::accessibleInterface(id) == nullptr);
#else
    Q_UNUSED(app);
#endif
}

void playing_tracks_reorder_the_real_transition_library(QApplication& app)
{
    gvt::ControlBus bus;
    gvt::AudioEngine engine(&bus); // no audio device, no real music
    gvt::TrackLibrary library;
    gvt::TransitionStore store;
    auto trackA = std::make_shared<gvt::TrackData>();
    auto trackB = std::make_shared<gvt::TrackData>();
    trackA->title = QStringLiteral("Alpha");
    trackA->fingerprint = QStringLiteral("gvfp1:accessibility-alpha");
    trackB->title = QStringLiteral("Zulu");
    trackB->fingerprint = QStringLiteral("gvfp1:accessibility-zulu");
    QStringList paths;
    QList<QByteArray> hashes;
    const auto fileHash = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    for (auto track : {trackA, trackB}) {
        track->bpm = 120;
        track->durationSec = 4;
        track->pcm.resize(4 * gvt::kSampleRate * 2);
        gvt::GvtFile file;
        file.name = track->title + QStringLiteral(" transition");
        file.from.title = track->title;
        file.from.fingerprint = track->fingerprint;
        file.from.bpm = 120;
        file.from.durationSec = 4;
        file.to = file.from;
        file.masterBpm = 120;
        file.endBeat = 4;
        file.events = {{2.0, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0,
                        gvt::Curve::Step}};
        QString error;
        const auto path = store.save(file, &error);
        CHECK(!path.isEmpty());
        if (path.isEmpty()) {
            std::fprintf(stderr, "Fixture save failed: %s\n", qPrintable(error));
            return;
        }
        paths.append(path);
        hashes.append(fileHash(path));
    }
    engine.deck(0).loadTrack(trackA);
    engine.deck(1).loadTrack(trackB);
    {
        gvt::LibraryWidget widget(&library, &engine, &store);
        widget.resize(1000, 430);
        widget.show();
        QMetaObject::invokeMethod(&widget, "showTab", Q_ARG(int, 2));
        app.processEvents();
        auto* table = widget.findChild<QTableView*>(QStringLiteral("transitionLibraryTable"));
        CHECK(table);
        if (!table) return;
        auto* timer = widget.findChild<QTimer*>();
        CHECK(timer);
        if (!timer) return;
        timer->stop();
        const auto tableId = cacheCells(*table, true);
        auto* search = widget.findChild<QLineEdit*>(QStringLiteral("librarySearchField"));
        CHECK(search);
        for (int iteration = 0; iteration < 100; ++iteration) {
            const bool playB = iteration % 2;
            bus.dispatch({0, playB ? gvt::ControlId::Stop : gvt::ControlId::Play, 1}, gvt::Origin::Ui);
            bus.dispatch({1, playB ? gvt::ControlId::Play : gvt::ControlId::Stop, 1}, gvt::Origin::Ui);
            CHECK(engine.deck(0).playing.load() == !playB);
            CHECK(engine.deck(1).playing.load() == playB);
            QMetaObject::invokeMethod(timer, "timeout"); // exact production refresh
            table->sortByColumn(0, iteration % 3 ? Qt::AscendingOrder : Qt::DescendingOrder);
            CHECK(table->model()->index(0, 0).data().toString() == (playB ? trackB->title : trackA->title));
            app.processEvents();
            CHECK(cacheCells(*table, true) == tableId);
            if (iteration % 10 == 0) {
                if (search) {
#ifdef Q_OS_MACOS
                    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
                        retireNativeTableRows(tableId);
#endif
                    search->setText(QStringLiteral("Alpha"));
                    CHECK(table->model()->rowCount() == 1);
                    QMetaObject::invokeMethod(&widget, "showTab", Q_ARG(int, 0));
                    app.processEvents();
                    search->setText(QStringLiteral("No matches"));
                    CHECK(table->model()->rowCount() == 0);
                    QMetaObject::invokeMethod(&widget, "showTab", Q_ARG(int, 2));
                    search->clear();
                    CHECK(table->model()->rowCount() == 2);
                    CHECK(cacheCells(*table, true) == tableId);
                }
                auto* portable = widget.findChild<QCheckBox*>(QStringLiteral("portableTransitionFilter"));
                CHECK(portable);
                if (portable) {
                    portable->setChecked(false);
                    CHECK(table->model()->rowCount() == 0);
                    portable->setChecked(true);
                    CHECK(table->model()->rowCount() == 2);
                }
                store.reload();
                app.processEvents();
                CHECK(cacheCells(*table, true) == tableId);
            }
        }
        bus.dispatch({0, gvt::ControlId::Stop, 1}, gvt::Origin::Ui);
        bus.dispatch({1, gvt::ControlId::Stop, 1}, gvt::Origin::Ui);
        QMetaObject::invokeMethod(timer, "timeout");
    }
    app.processEvents();
    for (int i = 0; i < paths.size(); ++i) CHECK(fileHash(paths[i]) == hashes[i]);
}
} // namespace

int main(int argc, char** argv)
{
    bool native = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--native") native = true;
    qputenv("QT_QPA_PLATFORM", native ? "cocoa" : "offscreen");
    QApplication app(argc, argv);
    const bool patched = app.arguments().contains(QStringLiteral("--unpatched"))
        ? false : gvt::installQtAccessibilityWorkaround();
    if (!app.arguments().contains(QStringLiteral("--unpatched"))) {
        const auto qt = QVersionNumber::fromString(QString::fromLatin1(qVersion()));
        CHECK(patched == (native && (qt == QVersionNumber(6, 11, 0) || qt == QVersionNumber(6, 11, 1))));
        CHECK(gvt::installQtAccessibilityWorkaround() == patched); // idempotent
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("GravitinoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_library_accessibility"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    qputenv("GRAVITINO_CACHE_DIR", temporary.filePath("cache").toUtf8());
    qputenv("GRAVITINO_CATALOG_PATH", temporary.filePath("catalog.json").toUtf8());
    qputenv("GRAVITINO_TRANSITIONS_DIR", temporary.filePath("transitions").toUtf8());
    QAccessible::setActive(true);
    if (native) CHECK(QAccessible::isActive());
    native_row_retirement_keeps_cells_alive_for_search(app);
    table_refreshes_keep_accessibility_alive(app);
    playing_tracks_reorder_the_real_transition_library(app);
    if (failures) return 1;
    std::puts("test_library_accessibility: sorting, filtering, playback, reload and cleanup passed");
}
