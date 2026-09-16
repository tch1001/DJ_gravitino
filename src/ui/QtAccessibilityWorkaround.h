// Protect Qt's native macOS table accessibility ownership on affected runtimes.
#pragma once

namespace gvt {
// Call after QApplication construction and before creating any widgets.
// Returns true only when the affected Cocoa implementation was patched.
bool installQtAccessibilityWorkaround();
}
