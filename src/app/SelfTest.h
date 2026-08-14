#pragma once
#include <QStringList>

namespace gvt {
// Headless offline render check (implemented in SelfTest.cpp by the
// orchestrator). Returns a process exit code.
int runSelfTest(const QStringList& args);
} // namespace gvt
