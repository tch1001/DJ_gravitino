#pragma once
#include <QColor>
#include <QString>

namespace gvt {

// Dark pro-audio palette shared by the ui/ widgets and the app stylesheet.
inline QColor themeBackground() { return QColor(0x16, 0x18, 0x1d); }
inline QColor themePanel()      { return QColor(0x2a, 0x2e, 0x37); }
inline QColor themeText()       { return QColor(0xd8, 0xdc, 0xe4); }
inline QColor themeDimText()    { return QColor(0x8a, 0x90, 0x9c); }

// Deck accents: 0 = cyan (A), 1 = magenta (B).
inline QColor deckAccent(int deck)
{
    return deck == 0 ? QColor(0x35, 0xc8, 0xe8) : QColor(0xe8, 0x55, 0x9a);
}

QString appStyleSheet(); // defined in MainWindow.cpp

} // namespace gvt
