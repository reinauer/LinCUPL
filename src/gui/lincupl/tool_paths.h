#pragma once

#include <QString>
#include <QStringList>

namespace ToolPaths {

// Locate a sibling CLI/GUI executable (cupl, csim, cbld, backpin, cfitr, linsim).
// Checks QSettings custom overrides first, then <appDir>, ../bin, build/bin, and PATH.
QString findExecutable(const QString &toolName);

// Return all discovered .dl device libraries (e.g., lincupl.dl, Atmel.dl, cupl.dl).
QStringList findDeviceLibraries();

// Return the active .dl library path (honoring QSettings override, LIBCUPL, or lincupl.dl/Atmel.dl/cupl.dl).
QString activeDeviceLibrary();
void setActiveDeviceLibrary(const QString &path);

// Locate Atmel.cat device category file.
QString findAtmelCat();

// Locate CuplErr.txt compiler/simulator error catalog.
QString findCuplErrTxt();

// Locate example directory (for New/Open examples and self-test).
QString findExamplesDir();

// Locate shipped CUPL macro library directory (Macro/*.MAC).
QString findMacroDir();

// Optional custom tool directory override in QSettings.
QString customBinDir();
void setCustomBinDir(const QString &dirPath);

} // namespace ToolPaths
