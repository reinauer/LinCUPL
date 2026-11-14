#pragma once

#include <QtGlobal>

// Call after QApplication has loaded the platform plugin, before any widgets.
#ifdef Q_OS_MACOS
void installMacAccessibilityWorkaround();
#else
inline void installMacAccessibilityWorkaround() {}
#endif
