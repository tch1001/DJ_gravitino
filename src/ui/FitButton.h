#pragma once

#include <QFontMetricsF>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>

#include <algorithm>

namespace gvt {

// Return the visible part of a button label for width measurement. Qt uses
// '&' as a mnemonic marker and '&&' as a literal ampersand.
inline QString visibleButtonText(const QString& text)
{
    QString visible;
    visible.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('&')) {
            visible.append(text.at(i));
            continue;
        }
        if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
            visible.append(QLatin1Char('&'));
            ++i;
        }
    }
    return visible;
}

inline QFont buttonFontFittedTo(
    QFont font, const QString& label, const QRect& bounds)
{
    const QString visible = visibleButtonText(label);
    const qreal availableWidth = std::max(1, bounds.width() - 4);
    const qreal availableHeight = std::max(1, bounds.height() - 2);
    const auto fits = [&](const QFont& candidate) {
        const QFontMetricsF metrics(candidate);
        return metrics.horizontalAdvance(visible) <= availableWidth &&
               metrics.height() <= availableHeight;
    };
    if (visible.isEmpty() || fits(font))
        return font;

    // Stylesheets often express compact controls in pixels, whereas the
    // platform default uses points. Stay in the font's native unit and only
    // shrink—never enlarge a button that already fits.
    if (font.pixelSize() > 0) {
        for (int pixels = font.pixelSize() - 1; pixels >= 1; --pixels) {
            font.setPixelSize(pixels);
            if (fits(font)) break;
        }
    } else {
        qreal points = font.pointSizeF();
        if (!(points > 0.0)) points = 11.0;
        for (points -= 0.5; points >= 0.5; points -= 0.5) {
            font.setPointSizeF(points);
            if (fits(font)) break;
        }
    }
    return font;
}

// Qt's stock buttons clip text when a layout makes them narrower than their
// size hint. These variants preserve the native/QSS frame, hover, pressed,
// checked, focus, menu-arrow, and palette behavior, but paint the full label
// with a font fitted to the live contents rectangle.
class FitPushButton : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        const QString label = option.text;
        option.text.clear();
        painter.drawControl(QStyle::CE_PushButton, option);

        QRect contents = style()->subElementRect(
            QStyle::SE_PushButtonContents, &option, this);
        painter.setFont(buttonFontFittedTo(font(), label, contents));
        style()->drawItemText(
            &painter, contents,
            Qt::AlignCenter | Qt::TextShowMnemonic,
            palette(), isEnabled(), label, QPalette::ButtonText);
    }
};

class FitToolButton : public QToolButton {
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        const QString label = option.text;
        option.text.clear();
        painter.drawComplexControl(QStyle::CC_ToolButton, option);

        QRect contents = style()->subControlRect(
            QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, this);
        if (option.features.testFlag(QStyleOptionToolButton::HasMenu)) {
            contents.adjust(
                0, 0,
                -style()->pixelMetric(
                    QStyle::PM_MenuButtonIndicator, &option, this),
                0);
        }
        painter.setFont(buttonFontFittedTo(font(), label, contents));
        style()->drawItemText(
            &painter, contents,
            Qt::AlignCenter | Qt::TextShowMnemonic,
            palette(), isEnabled(), label, QPalette::ButtonText);
    }
};

} // namespace gvt
