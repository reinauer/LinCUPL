#pragma once

// Sets the application icon from the PNGs that lincupl_app_icon() (see
// app_icon.cmake) compiles into the program.  On macOS this is also the Dock
// icon; on Linux the desktop file name lets the window be matched with the
// installed <name>.desktop entry and its icon.

#include <QApplication>
#include <QIcon>
#include <QString>

inline void lincuplSetAppIcon(const QString &name) {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
        icon.addFile(QStringLiteral(":/icons/%1-%2.png").arg(name).arg(size), QSize(size, size));
    }
    QApplication::setWindowIcon(icon);
    QGuiApplication::setDesktopFileName(name);
}
