#pragma once

// Toolbar and menu icons, compiled in by lincupl_action_icons() (see
// app_icon.cmake) from icons/actions/png, rendered from the SVG sources in
// icons/actions by icons/render.sh.

#include <QIcon>
#include <QString>

inline QIcon lincuplIcon(const QString &name) {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64}) {
        icon.addFile(QStringLiteral(":/actions/%1-%2.png").arg(name).arg(size), QSize(size, size));
    }
    return icon;
}
