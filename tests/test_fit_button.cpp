#include "ui/FitButton.h"

#include <QApplication>
#include <QFontMetricsF>

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    QFont base = app.font();
    base.setPixelSize(14);
    base.setBold(true);

    const QRect generous(0, 0, 140, 26);
    const QFont shortFont = gvt::buttonFontFittedTo(
        base, QStringLiteral("SYNC"), generous);
    CHECK(shortFont.pixelSize() == 14);

    const QRect compact(0, 0, 58, 18);
    const QString longLabel = QStringLiteral("STOP && SAVE");
    const QFont fitted = gvt::buttonFontFittedTo(base, longLabel, compact);
    CHECK(fitted.pixelSize() > 0);
    CHECK(fitted.pixelSize() < base.pixelSize());
    const QFontMetricsF metrics(fitted);
    CHECK(metrics.horizontalAdvance(QStringLiteral("STOP & SAVE")) <=
          compact.width() - 4);
    CHECK(metrics.height() <= compact.height() - 2);
    CHECK(gvt::visibleButtonText(longLabel) ==
          QStringLiteral("STOP & SAVE"));

    if (failures != 0)
        return 1;
    std::printf("test_fit_button: compact labels shrink and remain complete\n");
    return 0;
}
