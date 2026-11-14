#include "device_library.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

namespace {

constexpr int kDlHeaderSize = 199;          // 0xc7
constexpr uint16_t kDlMagic = 0xd431u;      // -0x2bcf
constexpr uint16_t kDlAccessKey = 0x0028u;  // 40
constexpr int kDlDirMaxEntries = 1000;      // 0x3e8
constexpr int kDlDirEntrySize = 26;         // 0x1a
constexpr int kDlMinFileSize = kDlHeaderSize + kDlDirMaxEntries * kDlDirEntrySize; // 26199
constexpr int kDlNumSections = 13;
constexpr int kDlSecHdrSize = 20;

uint16_t readU16LE(const uint8_t *p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32LE(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

QString readBoundedCString(const uint8_t *buf, int maxLen) {
    int len = 0;
    while (len < maxLen && buf[len] != 0) {
        ++len;
    }
    return QString::fromLatin1(reinterpret_cast<const char *>(buf), len).trimmed();
}

} // namespace

bool DeviceLibrary::loadLibrary(const QString &dlPath, QString *errorOut) {
    QFile file(dlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Could not open device library: %1").arg(dlPath);
        }
        return false;
    }

    const QByteArray raw = file.readAll();
    if (raw.size() < kDlMinFileSize) {
        if (errorOut) {
            *errorOut = QStringLiteral("File is too small to be a valid CUPL .dl library: %1").arg(dlPath);
        }
        return false;
    }

    const auto *bytes = reinterpret_cast<const uint8_t *>(raw.constData());
    const uint64_t totalSize = static_cast<uint64_t>(raw.size());

    const uint16_t magic = readU16LE(bytes);
    if (magic != kDlMagic) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid .dl magic (0x%1, expected 0xd431) in %2")
                            .arg(magic, 4, 16, QLatin1Char('0'))
                            .arg(dlPath);
        }
        return false;
    }

    const uint16_t key = readU16LE(bytes + 2);
    if (key != kDlAccessKey) {
        if (errorOut) {
            *errorOut = QStringLiteral("Unsupported .dl access key (%1, expected %2) in %3")
                            .arg(key)
                            .arg(kDlAccessKey)
                            .arg(dlPath);
        }
        return false;
    }

    m_libraryPath = QFileInfo(dlPath).canonicalFilePath();
    if (m_libraryPath.isEmpty()) {
        m_libraryPath = dlPath;
    }
    m_accessKey = key;
    m_libraryName = readBoundedCString(bytes + 0x04, 0x11);
    m_libraryRevision = readBoundedCString(bytes + 0x15, 0x11);
    m_libraryDescription = readBoundedCString(bytes + 0x26, kDlHeaderSize - 0x26);

    m_devices.clear();
    m_indexByMnemonic.clear();
    m_devices.reserve(256);

    for (int slot = 0; slot < kDlDirMaxEntries; ++slot) {
        const uint8_t *entry = bytes + kDlHeaderSize + slot * kDlDirEntrySize;
        const uint8_t flags = entry[0x11];
        if ((flags & 0x01u) == 0) {
            continue;
        }

        const QString mnemonic = readBoundedCString(entry, 0x11).toLower();
        if (mnemonic.isEmpty()) {
            continue;
        }

        const uint32_t recSize = readU32LE(entry + 0x12);
        const uint32_t recOff = readU32LE(entry + 0x16);
        if (recSize < static_cast<uint32_t>(kDlNumSections * kDlSecHdrSize) ||
            static_cast<uint64_t>(recOff) + static_cast<uint64_t>(recSize) > totalSize) {
            continue;
        }

        const uint8_t *rec = bytes + recOff;
        DeviceInfo info;
        info.mnemonic = mnemonic;
        info.directoryIndex = slot;
        info.recordOffset = recOff;
        info.recordSize = recSize;

        // Section 3 header sits at offset 3 * 20 = 0x3c; roff is at 0x44
        const uint32_t sec3Off = readU32LE(rec + 0x44);
        if (static_cast<uint64_t>(sec3Off) + 0x1fu <= recSize) {
            const uint8_t *sec3 = rec + sec3Off;
            info.revision = readU16LE(sec3 + 0x00);
            info.pins = static_cast<int>(sec3[0x08]);
            info.fitterType = sec3[0x12];
            info.fuses = readU32LE(sec3 + 0x15);
            info.productTerms = readU16LE(sec3 + 0x1d);
        }

        // Section 4 header sits at offset 4 * 20 = 0x50: cnt at +0x54, rsize at +0x56, roff at +0x58
        const uint16_t extCnt = readU16LE(rec + 0x54);
        const uint16_t extRSize = readU16LE(rec + 0x56);
        const uint32_t extOff = readU32LE(rec + 0x58);
        const uint32_t stride = (extRSize > 0) ? extRSize : 0x15u;

        if (extCnt > 0 &&
            static_cast<uint64_t>(extOff) + static_cast<uint64_t>(extCnt) * stride <= recSize) {
            for (uint16_t j = 0; j < extCnt; ++j) {
                const uint8_t *extPtr = rec + extOff + static_cast<uint32_t>(j) * stride;
                const QString ext = readBoundedCString(extPtr, static_cast<int>(stride)).toUpper();
                if (!ext.isEmpty() && !info.extensions.contains(ext)) {
                    info.extensions.append(ext);
                }
            }
        }

        m_indexByMnemonic.insert(info.mnemonic, m_devices.size());
        m_devices.append(info);
    }

    applyCategoriesToDevices();
    return true;
}

bool DeviceLibrary::loadCategories(const QString &catPath, QString *errorOut) {
    QFile file(catPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Could not open category file: %1").arg(catPath);
        }
        return false;
    }

    const QString content = QString::fromLatin1(file.readAll());
    m_categoryPath = QFileInfo(catPath).canonicalFilePath();
    if (m_categoryPath.isEmpty()) {
        m_categoryPath = catPath;
    }

    m_categoryOrder.clear();
    m_categoryRules.clear();

    int runningOrder = 0;
    const QStringList blocks = content.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &rawBlock : blocks) {
        const QString block = rawBlock.trimmed();
        if (block.isEmpty()) {
            continue;
        }
        const int colonIdx = block.indexOf(QLatin1Char(':'));
        if (colonIdx <= 0) {
            continue;
        }

        const QString categoryName = block.left(colonIdx).trimmed();
        if (categoryName.isEmpty()) {
            continue;
        }
        if (!m_categoryOrder.contains(categoryName)) {
            m_categoryOrder.append(categoryName);
        }

        const QString body = block.mid(colonIdx + 1);
        const QStringList pkgParts = body.split(QLatin1Char('|'));

        auto addList = [&](const QString &csv, const QString &pkgGroup) {
            const QStringList tokens = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &tok : tokens) {
                const QString mnem = tok.trimmed().toLower();
                if (!mnem.isEmpty()) {
                    CategoryRule rule;
                    rule.category = categoryName;
                    rule.packageGroup = pkgGroup;
                    rule.order = runningOrder++;
                    m_categoryRules.insert(mnem, rule);
                }
            }
        };

        if (!pkgParts.isEmpty()) {
            addList(pkgParts.at(0), QStringLiteral("DIP / Standard"));
        }
        if (pkgParts.size() > 1) {
            addList(pkgParts.at(1), QStringLiteral("LCC / PLCC"));
        }
    }

    applyCategoriesToDevices();
    return true;
}

QString DeviceLibrary::inferPackageGroup(const QString &mnemonic) {
    const QString m = mnemonic.toLower();
    if (m.endsWith(QStringLiteral("lcc")) ||
        m.endsWith(QStringLiteral("plcc")) ||
        m.endsWith(QStringLiteral("jlcc")) ||
        m.endsWith(QStringLiteral("pga")) ||
        m.contains(QStringLiteral("plcc")) ||
        m.contains(QStringLiteral("tqfp")) ||
        m.contains(QStringLiteral("pqfp")) ||
        m.contains(QStringLiteral("qfp"))) {
        // Note: In Atmel.cat, f1502plcc44..f1508isppqfp160 are listed before '|' under Atmel,
        // so Atmel.cat rules take precedence when present; this fallback handles cupl.dl extras.
        return QStringLiteral("LCC / PLCC");
    }
    return QStringLiteral("DIP / Standard");
}

void DeviceLibrary::applyCategoriesToDevices() {
    for (int i = 0; i < m_devices.size(); ++i) {
        DeviceInfo &dev = m_devices[i];
        const auto it = m_categoryRules.constFind(dev.mnemonic);
        if (it != m_categoryRules.constEnd()) {
            dev.category = it->category;
            dev.packageGroup = it->packageGroup;
            dev.categoryOrder = it->order;
        } else {
            dev.category = QStringLiteral("CUPL Extended");
            dev.packageGroup = inferPackageGroup(dev.mnemonic);
            dev.categoryOrder = 100000 + dev.directoryIndex;
        }
    }
}

QStringList DeviceLibrary::categories() const {
    QStringList cats;
    cats.append(QStringLiteral("All Devices"));
    for (const QString &c : m_categoryOrder) {
        bool hasAny = false;
        for (const DeviceInfo &d : m_devices) {
            if (d.category == c) {
                hasAny = true;
                break;
            }
        }
        if (hasAny && !cats.contains(c)) {
            cats.append(c);
        }
    }
    for (const DeviceInfo &d : m_devices) {
        if (!d.category.isEmpty() && !cats.contains(d.category)) {
            cats.append(d.category);
        }
    }
    return cats;
}

DeviceInfo DeviceLibrary::findDevice(const QString &mnemonic) const {
    const QString key = mnemonic.trimmed().toLower();
    const int idx = m_indexByMnemonic.value(key, -1);
    if (idx >= 0 && idx < m_devices.size()) {
        return m_devices.at(idx);
    }
    return DeviceInfo{};
}

QList<DeviceInfo> DeviceLibrary::filterDevices(const QString &category,
                                               PackageFilter pkgFilter,
                                               const QString &searchQuery) const {
    const QString q = searchQuery.trimmed().toLower();
    const bool allCats = category.isEmpty() ||
                         category.compare(QStringLiteral("All Devices"), Qt::CaseInsensitive) == 0;

    QList<DeviceInfo> out;
    out.reserve(m_devices.size());

    for (const DeviceInfo &dev : m_devices) {
        if (!allCats && dev.category.compare(category, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (pkgFilter == PackageFilter::DipStandard &&
            dev.packageGroup != QStringLiteral("DIP / Standard")) {
            continue;
        }
        if (pkgFilter == PackageFilter::LccPlcc &&
            dev.packageGroup != QStringLiteral("LCC / PLCC")) {
            continue;
        }
        if (!q.isEmpty()) {
            const QString extJoined = dev.extensions.join(QLatin1Char(' ')).toLower();
            const bool matches =
                dev.mnemonic.contains(q) ||
                dev.category.toLower().contains(q) ||
                dev.packageGroup.toLower().contains(q) ||
                QString::number(dev.pins).contains(q) ||
                extJoined.contains(q);
            if (!matches) {
                continue;
            }
        }
        out.append(dev);
    }

    std::sort(out.begin(), out.end(), [](const DeviceInfo &a, const DeviceInfo &b) {
        if (a.categoryOrder != b.categoryOrder) {
            return a.categoryOrder < b.categoryOrder;
        }
        return a.mnemonic < b.mnemonic;
    });

    return out;
}
