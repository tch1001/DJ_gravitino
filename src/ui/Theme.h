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

// Serato-style hot-cue slot colors, index 0..7.
inline QColor hotCueColor(int i)
{
    static const QColor kSlots[8] = {
        QColor(0xe0, 0x55, 0x4d), // 1 red
        QColor(0xf0, 0x8c, 0x28), // 2 orange
        QColor(0xf0, 0xd0, 0x3c), // 3 yellow
        QColor(0x54, 0xc1, 0x7a), // 4 green
        QColor(0x35, 0xc8, 0xe8), // 5 cyan
        QColor(0x5a, 0x8f, 0xe8), // 6 blue
        QColor(0xb0, 0x6c, 0xe8), // 7 purple
        QColor(0xe8, 0x55, 0x9a), // 8 magenta
    };
    return kSlots[i & 7];
}

// Frequency-band waveform colors (Serato-ish mapping).
inline QColor waveLowColor()  { return QColor(0xe0, 0x55, 0x4d); }
inline QColor waveMidColor()  { return QColor(0x54, 0xc1, 0x7a); }
inline QColor waveHighColor() { return QColor(0x5a, 0x8f, 0xe8); }

// Transition entry marker (orange).
inline QColor transitionEntryColor() { return QColor(0xff, 0x8c, 0x1a); }

QString appStyleSheet(); // defined in MainWindow.cpp

} // namespace gvt
