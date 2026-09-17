// Shared application styling for the main mixer and standalone tooling windows.
#include "Theme.h"

namespace gvt {
QString appStyleSheet()
{
    return QStringLiteral(R"(
* { font-size: 11px; }
QMainWindow, QDialog { background: #16181d; }
QWidget { color: #d8dce4; background: #16181d; }
QWidget[panel="true"] { background: #2a2e37; border-radius: 6px; }
QLabel { background: transparent; }
QPushButton {
    background: #383d48; border: 1px solid #4a505c; border-radius: 4px;
    padding: 2px 8px; color: #d8dce4;
}
QPushButton:hover { background: #434956; }
QPushButton:pressed { background: #2a2e37; }
QPushButton:disabled { color: #6a707d; background: #2e323b; }
QLineEdit {
    background: #16181d; border: 1px solid #4a505c; border-radius: 4px;
    padding: 4px 6px; selection-background-color: #35c8e8;
    selection-color: black;
}
QTableView, QListWidget {
    background: #1c1f25; alternate-background-color: #21242b;
    border: 1px solid #383d48; gridline-color: #2a2e37;
    selection-background-color: #35c8e8; selection-color: black;
}
QHeaderView::section {
    background: #2a2e37; color: #8a909c; border: none;
    border-right: 1px solid #383d48; padding: 2px 5px;
}
QSlider:vertical { min-height: 84px; min-width: 22px; }
QSlider:horizontal { min-width: 120px; min-height: 22px; }
QSlider::groove:vertical { background: #383d48; width: 6px; border-radius: 3px; }
QSlider::groove:horizontal { background: #383d48; height: 6px; border-radius: 3px; }
QSlider::handle:vertical {
    background: #b8bec9; height: 14px; margin: 0 -6px; border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #b8bec9; width: 14px; margin: -6px 0; border-radius: 3px;
}
QSlider::handle:hover { background: #ffffff; }
QDial { background: #383d48; }
QProgressBar {
    background: #16181d; border: 1px solid #383d48; border-radius: 4px;
}
QProgressBar::chunk { background: #35c8e8; border-radius: 3px; }
QMenuBar { background: #16181d; }
QMenuBar::item:selected { background: #2a2e37; }
QMenu { background: #2a2e37; border: 1px solid #4a505c; }
QMenu::item:selected { background: #35c8e8; color: black; }
QStatusBar { background: #1c1f25; color: #8a909c; }
QToolTip { background: #2a2e37; color: #d8dce4; border: 1px solid #4a505c; }
QScrollBar { background: #16181d; }
QSplitter::handle { background: #383d48; }
QSplitter::handle:vertical { height: 5px; }
QSplitter::handle:horizontal { width: 5px; }
)");
}
}
