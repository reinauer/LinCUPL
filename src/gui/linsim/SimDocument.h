#pragma once

#include <QColor>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

enum class BusRadix {
    Hex = 16,
    Decimal = 10,
    Octal = 8,
    Binary = 2
};

struct SimHeader {
    QString name;
    QString partNo;
    QString date;
    QString revision;
    QString designer;
    QString company;
    QString assembly;
    QString location;
    QString device;
};

struct PinMeta {
    QString name;
    QString pinNumber;
    bool activeLowInDesign = false;
    bool isOutput = false;
    QString equation;
};

struct FieldDef {
    QString name;
    QStringList members; // MSB first
    bool definedInSi = false;
};

struct SignalItem {
    enum class Kind {
        Scalar,
        FieldBus,
        RangeBus
    };

    Kind kind = Kind::Scalar;
    QString name;           // e.g. "clk", "oe", "shift", "Q3..Q0", "L0..L19"
    bool activeLow = false; // '!' prefix in ORDER:
    QStringList members;    // size 1 for Scalar, size >= 1 for Bus (MSB first)
    int spacingAfter = 0;   // %N token following this item in ORDER:
    bool expanded = false;  // UI expand/collapse state for buses
    BusRadix displayRadix = BusRadix::Hex;
    QColor customColor;     // invalid QColor => use theme default

    bool isBus() const {
        return kind != Kind::Scalar || members.size() > 1;
    }

    int width() const {
        return members.isEmpty() ? 1 : members.size();
    }

    QString displayLabel() const {
        return activeLow ? (QStringLiteral("!") + name) : name;
    }

    QString orderToken() const {
        return activeLow ? (QStringLiteral("!") + name) : name;
    }
};

struct SimError {
    int code = 19;
    QChar expected = QLatin1Char('?');
    QString pinName;
};

struct VectorRow {
    QHash<QString, QChar> pinStates; // key = lowercase pin name, value in {0,1,C,K,P,L,H,Z,X,*,N}
    QString comment;                 // preserved inline /* ... */ comment
    QStringList messagesBefore;      // $MSG "..." lines preceding this vector
    QVector<SimError> errors;        // #E mismatch diagnostics from .wo
};

class SimDocument {
public:
    SimHeader header;
    BusRadix fileBase = BusRadix::Hex;
    bool hasExplicitBase = false;
    QStringList headerComments;      // /* ... */ blocks between headers and VECTORS:
    int leadingSpacing = 0;          // %N before first signal in ORDER:
    QVector<SignalItem> signalsList; // items in ORDER:
    QVector<VectorRow> vectors;
    QStringList trailingMessages;    // $MSG lines after final vector

    // Metadata discovered from .si, .wo, companion .sim, or companion .pld
    QMap<QString, FieldDef> fields;   // key = lowercase field name
    QMap<QString, PinMeta> pinMeta;   // key = lowercase pin name

    QString filePath; // current .si or .wo path
    bool loadedFromWo = false;
    bool modified = false;

    void clear();
    static SimDocument createDefaultNew();

    bool loadFile(const QString &path, QString *errorOut = nullptr);
    bool loadSiText(const QString &text, const QString &contextFilePath, QString *errorOut = nullptr);
    bool loadWoText(const QString &text, const QString &contextFilePath, QString *errorOut = nullptr);

    void loadCompanionMetadata(const QString &contextFilePath);

    QString serializeSi(const QString &targetSiPath = QString()) const;
    bool saveSiFile(const QString &path, QString *errorOut = nullptr);

    bool mergeSimulatedWo(const QString &woPath, QString *errorOut = nullptr);
    void assignSimulatorResults();

    // Pin & Bus state accessors
    QStringList allOrderedPins() const;
    QChar pinState(int vecIdx, const QString &pinName) const;
    void setPinState(int vecIdx, const QString &pinName, QChar state);
    bool isPinOutput(const QString &pinName) const;
    bool isSignalOutput(const SignalItem &sig) const;

    const SimError *findError(int vecIdx, const QString &pinName) const;
    bool hasErrorInSignal(int vecIdx, const SignalItem &sig) const;

    QString formatBusValue(const SignalItem &sig, int vecIdx, BusRadix radix,
                           bool includeSuffix = true, bool *isCleanNumeric = nullptr) const;
    QString busBitString(const SignalItem &sig, int vecIdx) const;

    bool setBusValueFromString(int sigIdx, int startVec, int endVec,
                               const QString &valueText, BusRadix radix,
                               bool driveAsOutput, QString *errorOut = nullptr);

    // Signal & Vector editing helpers
    int insertVector(int index, int count = 1, QChar defaultIn = QLatin1Char('0'),
                     QChar defaultOut = QLatin1Char('*'), bool copyFromNeighbor = false);
    void duplicateVector(int index);
    void deleteVectors(int startVec, int endVec);

    bool addSignal(const SignalItem &item, int insertIndex = -1,
                   QChar initialState = QLatin1Char('\0'), QString *errorOut = nullptr);
    bool renameSignal(int sigIdx, const QString &newName, bool newActiveLow, QString *errorOut = nullptr);
    void deleteSignal(int sigIdx);
    void moveSignal(int fromIdx, int toIdx);

    // Utility functions exposed for dialogs & parser
    static bool expandRangeToken(const QString &token, QStringList &membersOut,
                                 bool *activeLowOut = nullptr, QString *cleanRangeNameOut = nullptr);
    static QStringList expandBracketList(const QString &bracketBody);
    static QChar normalizeStateChar(QChar c);
    static bool isValidStateChar(QChar c);
    static QString findCompanionFile(const QString &basePath, const QString &extWithDot);
};
