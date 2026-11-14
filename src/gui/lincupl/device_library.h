#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>

struct DeviceSectionHeader {
    uint8_t ord = 0;
    uint8_t seg = 0;
    uint16_t max = 0;
    uint16_t cnt = 0;
    uint16_t rsize = 0;
    uint32_t roff = 0;
};

struct DeviceInfo {
    QString mnemonic;
    int directoryIndex = -1;
    uint16_t revision = 0;
    int pins = 0;
    uint8_t fitterType = 0;
    uint32_t fuses = 0;
    uint16_t productTerms = 0;
    uint32_t recordOffset = 0;
    uint32_t recordSize = 0;
    QStringList extensions;
    QString category;       // e.g. "GAL", "Atmel", "PAL 20+", "Other", "CUPL Extended"
    QString packageGroup;   // "DIP / Standard" or "LCC / PLCC"
    int categoryOrder = 999999;

    bool isValid() const { return !mnemonic.isEmpty() && directoryIndex >= 0; }
};

struct CategoryRule {
    QString category;
    QString packageGroup;
    int order = 0;
};

class DeviceLibrary {
public:
    enum class PackageFilter {
        All,
        DipStandard,
        LccPlcc
    };

    DeviceLibrary() = default;

    bool loadLibrary(const QString &dlPath, QString *errorOut = nullptr);
    bool loadCategories(const QString &catPath, QString *errorOut = nullptr);

    bool isLoaded() const { return !m_devices.isEmpty(); }
    QString libraryPath() const { return m_libraryPath; }
    QString libraryName() const { return m_libraryName; }
    QString libraryRevision() const { return m_libraryRevision; }
    QString libraryDescription() const { return m_libraryDescription; }
    uint16_t accessKey() const { return m_accessKey; }
    QString categoryFilePath() const { return m_categoryPath; }

    const QList<DeviceInfo> &devices() const { return m_devices; }
    QStringList categories() const;
    DeviceInfo findDevice(const QString &mnemonic) const;

    QList<DeviceInfo> filterDevices(const QString &category,
                                    PackageFilter pkgFilter,
                                    const QString &searchQuery) const;

private:
    void applyCategoriesToDevices();
    static QString inferPackageGroup(const QString &mnemonic);

    QString m_libraryPath;
    QString m_libraryName;
    QString m_libraryRevision;
    QString m_libraryDescription;
    uint16_t m_accessKey = 0;
    QString m_categoryPath;

    QList<DeviceInfo> m_devices;
    QHash<QString, int> m_indexByMnemonic;
    QStringList m_categoryOrder;
    QHash<QString, CategoryRule> m_categoryRules;
};
