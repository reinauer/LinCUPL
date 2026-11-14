#include "SimDocument.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <algorithm>

namespace {

QString stripOuterQuotes(const QString &s) {
    QString t = s.trimmed();
    if (t.size() >= 2 && ((t.startsWith(QLatin1Char('"')) && t.endsWith(QLatin1Char('"'))) ||
                          (t.startsWith(QLatin1Char('\'')) && t.endsWith(QLatin1Char('\''))))) {
        return t.mid(1, t.size() - 2);
    }
    return t;
}

QString removeBlockComments(const QString &input, QStringList *extractedComments = nullptr) {
    QString out;
    out.reserve(input.size());
    int i = 0;
    const int n = input.size();
    while (i < n) {
        if (i + 1 < n && input[i] == QLatin1Char('/') && input[i + 1] == QLatin1Char('*')) {
            int start = i;
            i += 2;
            bool closed = false;
            while (i < n) {
                if (i + 1 < n && input[i] == QLatin1Char('*') && input[i + 1] == QLatin1Char('/')) {
                    i += 2;
                    closed = true;
                    break;
                }
                i++;
            }
            if (extractedComments) {
                QString cmt = input.mid(start, i - start).trimmed();
                if (!closed) {
                    cmt += QStringLiteral(" */");
                }
                extractedComments->append(cmt);
            }
            out.append(QLatin1Char(' '));
        } else {
            out.append(input[i]);
            i++;
        }
    }
    return out;
}

// Mask block comments with spaces of identical character length so regex offsets match `input`
QString maskBlockComments(const QString &input) {
    QString masked = input;
    int i = 0;
    const int n = masked.size();
    while (i < n) {
        if (i + 1 < n && masked[i] == QLatin1Char('/') && masked[i + 1] == QLatin1Char('*')) {
            masked[i] = QLatin1Char(' ');
            masked[i + 1] = QLatin1Char(' ');
            i += 2;
            while (i < n) {
                if (i + 1 < n && masked[i] == QLatin1Char('*') && masked[i + 1] == QLatin1Char('/')) {
                    masked[i] = QLatin1Char(' ');
                    masked[i + 1] = QLatin1Char(' ');
                    i += 2;
                    break;
                }
                if (masked[i] != QLatin1Char('\n') && masked[i] != QLatin1Char('\r')) {
                    masked[i] = QLatin1Char(' ');
                }
                i++;
            }
        } else {
            i++;
        }
    }
    return masked;
}

// Arbitrary-width unsigned integer division by small radix on bit array (MSB first: bits[0] = MSB)
QString bitsToRadixString(const QVector<int> &bitsMsbFirst, int radix) {
    if (bitsMsbFirst.isEmpty()) {
        return QStringLiteral("0");
    }
    if (radix == 2) {
        QString s;
        s.reserve(bitsMsbFirst.size());
        for (int b : bitsMsbFirst) {
            s.append(b ? QLatin1Char('1') : QLatin1Char('0'));
        }
        return s;
    }
    if (radix == 16) {
        int pad = (4 - (bitsMsbFirst.size() % 4)) % 4;
        QVector<int> padded(pad, 0);
        padded += bitsMsbFirst;
        QString s;
        for (int i = 0; i < padded.size(); i += 4) {
            int nib = (padded[i] << 3) | (padded[i + 1] << 2) | (padded[i + 2] << 1) | padded[i + 3];
            s.append(QString::number(nib, 16).toUpper());
        }
        return s;
    }
    if (radix == 8) {
        int pad = (3 - (bitsMsbFirst.size() % 3)) % 3;
        QVector<int> padded(pad, 0);
        padded += bitsMsbFirst;
        QString s;
        for (int i = 0; i < padded.size(); i += 3) {
            int oct = (padded[i] << 2) | (padded[i + 1] << 1) | padded[i + 2];
            s.append(QString::number(oct, 8));
        }
        return s;
    }
    // Decimal: accumulate big-endian base-10 digits
    QVector<int> decDigits = {0};
    for (int b : bitsMsbFirst) {
        int carry = b;
        for (int i = decDigits.size() - 1; i >= 0; --i) {
            int v = decDigits[i] * 2 + carry;
            decDigits[i] = v % 10;
            carry = v / 10;
        }
        while (carry > 0) {
            decDigits.prepend(carry % 10);
            carry /= 10;
        }
    }
    QString s;
    s.reserve(decDigits.size());
    for (int d : decDigits) {
        s.append(QLatin1Char('0' + d));
    }
    return s;
}

// Parse string in `radix` into `width` bits (MSB first). Supports X/Z/* digits in hex/oct/bin.
bool radixStringToStates(const QString &rawStr, int radix, int width, bool driveAsOutput,
                         QVector<QChar> &statesOut, QString *errorOut = nullptr) {
    QString s = rawStr.trimmed();
    if (s.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Bus value cannot be empty.");
        return false;
    }
    // Optional CUPL prefix 'b', 'h', 'o', 'd' or 0x / 0b
    if (s.size() >= 3 && s[0] == QLatin1Char('\'') && s[2] == QLatin1Char('\'')) {
        QChar bc = s[1].toLower();
        if (bc == QLatin1Char('h')) { radix = 16; s = s.mid(3); }
        else if (bc == QLatin1Char('d')) { radix = 10; s = s.mid(3); }
        else if (bc == QLatin1Char('o')) { radix = 8; s = s.mid(3); }
        else if (bc == QLatin1Char('b')) { radix = 2; s = s.mid(3); }
    } else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        radix = 16;
        s = s.mid(2);
    } else if (s.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) {
        radix = 2;
        s = s.mid(2);
    }
    s.remove(QLatin1Char('_'));
    s.remove(QLatin1Char(' '));
    if (s.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("No digits in bus value.");
        return false;
    }

    const QChar zeroCh = driveAsOutput ? QLatin1Char('L') : QLatin1Char('0');
    const QChar oneCh = driveAsOutput ? QLatin1Char('H') : QLatin1Char('1');

    // Uniform special shorthand: single "X", "Z", "*"
    if (s.compare(QStringLiteral("X"), Qt::CaseInsensitive) == 0) {
        statesOut = QVector<QChar>(width, QLatin1Char('X'));
        return true;
    }
    if (s.compare(QStringLiteral("Z"), Qt::CaseInsensitive) == 0) {
        statesOut = QVector<QChar>(width, QLatin1Char('Z'));
        return true;
    }
    if (s == QStringLiteral("*")) {
        statesOut = QVector<QChar>(width, QLatin1Char('*'));
        return true;
    }

    if (radix == 2 || radix == 8 || radix == 16) {
        const int bitsPerDigit = (radix == 16) ? 4 : (radix == 8 ? 3 : 1);
        QVector<QChar> expanded;
        expanded.reserve(s.size() * bitsPerDigit);
        for (QChar ch : s) {
            QChar up = ch.toUpper();
            if (up == QLatin1Char('X') || up == QLatin1Char('Z') || up == QLatin1Char('*')) {
                for (int k = 0; k < bitsPerDigit; ++k) expanded.append(up);
                continue;
            }
            if (radix == 2 && (up == QLatin1Char('L') || up == QLatin1Char('H'))) {
                expanded.append(up);
                continue;
            }
            bool ok = false;
            int val = QString(up).toInt(&ok, radix);
            if (!ok || val < 0 || val >= radix) {
                if (errorOut) {
                    *errorOut = QStringLiteral("Invalid base-%1 digit '%2'.").arg(radix).arg(ch);
                }
                return false;
            }
            for (int k = bitsPerDigit - 1; k >= 0; --k) {
                expanded.append(((val >> k) & 1) ? oneCh : zeroCh);
            }
        }
        statesOut.resize(width);
        if (expanded.size() >= width) {
            int offset = expanded.size() - width;
            for (int i = 0; i < offset; ++i) {
                if (expanded[i] != QLatin1Char('0') && expanded[i] != QLatin1Char('L')) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("Bus value '%1' overflows %2-bit bus width.")
                                        .arg(rawStr.trimmed())
                                        .arg(width);
                    }
                    return false;
                }
            }
            // Take least-significant `width` bits (rightmost)
            for (int i = 0; i < width; ++i) {
                statesOut[i] = expanded[offset + i];
            }
        } else {
            int pad = width - expanded.size();
            for (int i = 0; i < pad; ++i) statesOut[i] = zeroCh;
            for (int i = 0; i < expanded.size(); ++i) statesOut[pad + i] = expanded[i];
        }
        return true;
    }

    // Decimal (arbitrary width)
    QVector<int> dec;
    dec.reserve(s.size());
    for (QChar ch : s) {
        if (!ch.isDigit()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Invalid decimal digit '%1'.").arg(ch);
            }
            return false;
        }
        dec.append(ch.digitValue());
    }
    while (dec.size() > 1 && dec.first() == 0) {
        dec.removeFirst();
    }
    if (dec.size() == 1 && dec.first() == 0) {
        dec.clear();
    }

    QVector<int> bitsLsb;
    bitsLsb.reserve(width);
    while (!dec.isEmpty() && bitsLsb.size() < width) {
        int rem = 0;
        QVector<int> next;
        for (int d : dec) {
            int cur = rem * 10 + d;
            int q = cur / 2;
            rem = cur % 2;
            if (!next.isEmpty() || q > 0) {
                next.append(q);
            }
        }
        bitsLsb.append(rem);
        dec = next;
    }
    if (!dec.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Decimal bus value '%1' overflows %2-bit bus width.")
                            .arg(rawStr.trimmed())
                            .arg(width);
        }
        return false;
    }
    while (bitsLsb.size() < width) {
        bitsLsb.append(0);
    }

    statesOut.resize(width);
    for (int i = 0; i < width; ++i) {
        int bit = bitsLsb[width - 1 - i];
        statesOut[i] = bit ? oneCh : zeroCh;
    }
    return true;
}

void parseSimMetadataFile(const QString &simPath,
                          QMap<QString, FieldDef> &fields,
                          QMap<QString, PinMeta> &pinMeta) {
    QFile f(simPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QString content = QString::fromLatin1(f.readAll());
    const QStringList lines = content.split(QRegularExpression(QStringLiteral("\r?\n")));

    enum Section { None, SignalSec, FieldSec, EquationSec };
    Section sec = None;

    QString curEqLhs;
    QStringList curEqLines;
    auto flushEq = [&]() {
        if (curEqLhs.isEmpty()) return;
        const QString body = curEqLines.join(QLatin1Char('\n')).trimmed();
        const QStringList lhsTokens = curEqLhs.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString &lhsTok : lhsTokens) {
            int dot = lhsTok.indexOf(QLatin1Char('.'));
            QString stem = (dot > 0) ? lhsTok.left(dot) : lhsTok;
            QString ext = (dot > 0) ? lhsTok.mid(dot + 1).toLower() : QString();
            if (stem.startsWith(QLatin1Char('!'))) stem = stem.mid(1);
            const QString key = stem.toLower();
            PinMeta &pm = pinMeta[key];
            if (pm.name.isEmpty()) pm.name = stem;
            if (ext == QLatin1Char('o') + QStringLiteral("e") && body == QStringLiteral("0")) {
                // Pure input (.oe => 0)
            } else {
                pm.isOutput = true;
            }
            const QString formatted = QStringLiteral("%1 => %2").arg(lhsTok, body.isEmpty() ? QStringLiteral("(none)") : body);
            if (!pm.equation.isEmpty()) pm.equation += QStringLiteral("\n");
            pm.equation += formatted;
        }
        curEqLhs.clear();
        curEqLines.clear();
    };

    for (const QString &rawLine : lines) {
        const QString trimmed = rawLine.trimmed();
        if (trimmed.compare(QStringLiteral("%SIGNAL"), Qt::CaseInsensitive) == 0) {
            flushEq();
            sec = SignalSec;
            continue;
        }
        if (trimmed.compare(QStringLiteral("%FIELD"), Qt::CaseInsensitive) == 0) {
            flushEq();
            sec = FieldSec;
            continue;
        }
        if (trimmed.compare(QStringLiteral("%EQUATION"), Qt::CaseInsensitive) == 0) {
            flushEq();
            sec = EquationSec;
            continue;
        }
        if (trimmed.compare(QStringLiteral("%END"), Qt::CaseInsensitive) == 0) {
            flushEq();
            sec = None;
            continue;
        }

        if (sec == SignalSec) {
            // Format: PIN  17 =  Q0   or   PIN  11 = !oe
            static const QRegularExpression rePin(
                QStringLiteral("^PIN(?:NODE)?\\s+(\\S+)\\s*=\\s*(!?)([A-Za-z0-9_\\.]+)"),
                QRegularExpression::CaseInsensitiveOption);
            auto m = rePin.match(trimmed);
            if (m.hasMatch()) {
                QString pinNum = m.captured(1);
                bool actLow = !m.captured(2).isEmpty();
                QString sigName = m.captured(3);
                PinMeta &pm = pinMeta[sigName.toLower()];
                pm.name = sigName;
                pm.pinNumber = pinNum;
                pm.activeLowInDesign = actLow;
            }
        } else if (sec == FieldSec) {
            // Format: FIELD input = D7,D6,D5,D4,D3,D2,D1,D0
            static const QRegularExpression reField(
                QStringLiteral("^FIELD\\s+([A-Za-z0-9_]+)\\s*=\\s*(.+)$"),
                QRegularExpression::CaseInsensitiveOption);
            auto m = reField.match(trimmed);
            if (m.hasMatch()) {
                QString fname = m.captured(1).trimmed();
                QString rhs = m.captured(2).trimmed();
                if (rhs.endsWith(QLatin1Char(';'))) rhs.chop(1);
                QStringList members = SimDocument::expandBracketList(rhs);
                if (!members.isEmpty() && !fields.contains(fname.toLower())) {
                    FieldDef fd;
                    fd.name = fname;
                    fd.members = members;
                    fd.definedInSi = false;
                    fields.insert(fname.toLower(), fd);
                }
            }
        } else if (sec == EquationSec) {
            int arrowIdx = rawLine.indexOf(QStringLiteral("=>"));
            if (arrowIdx >= 0) {
                flushEq();
                curEqLhs = rawLine.left(arrowIdx).trimmed();
                QString rhsRest = rawLine.mid(arrowIdx + 2).trimmed();
                if (!rhsRest.isEmpty()) curEqLines.append(rhsRest);
            } else if (!trimmed.isEmpty() && !curEqLhs.isEmpty()) {
                curEqLines.append(trimmed);
            }
        }
    }
    flushEq();
}

void parsePldMetadataFile(const QString &pldPath,
                          QMap<QString, FieldDef> &fields,
                          QMap<QString, PinMeta> &pinMeta) {
    QFile f(pldPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QString clean = removeBlockComments(QString::fromLatin1(f.readAll()));
    const QStringList stmts = clean.split(QLatin1Char(';'));
    for (const QString &rawStmt : stmts) {
        const QString stmt = rawStmt.trimmed();
        if (stmt.isEmpty()) continue;

        // FIELD name = [list]
        static const QRegularExpression reField(
            QStringLiteral("^FIELD\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.+)$"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        auto mf = reField.match(stmt);
        if (mf.hasMatch()) {
            const QString fname = mf.captured(1).trimmed();
            const QString rhs = mf.captured(2).trimmed();
            const QStringList members = SimDocument::expandBracketList(rhs);
            if (!members.isEmpty() && !fields.contains(fname.toLower())) {
                FieldDef fd;
                fd.name = fname;
                fd.members = members;
                fd.definedInSi = false;
                fields.insert(fname.toLower(), fd);
            }
            continue;
        }

        // PIN / PINNODE [=] ...
        static const QRegularExpression rePin(
            QStringLiteral("^PIN(?:NODE)?\\s*([^=]*)=(.+)$"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        auto mp = rePin.match(stmt);
        if (mp.hasMatch()) {
            const QString pinPart = mp.captured(1).trimmed();
            const QString sigPart = mp.captured(2).trimmed();
            const QStringList pinNums = pinPart.isEmpty() ? QStringList{} : SimDocument::expandBracketList(pinPart);
            QString sBody = sigPart.trimmed();
            bool groupNeg = false;
            if (sBody.startsWith(QLatin1Char('!')) && sBody.mid(1).trimmed().startsWith(QLatin1Char('['))) {
                groupNeg = true;
                sBody = sBody.mid(1).trimmed();
            }
            const QStringList sigNames = SimDocument::expandBracketList(sBody);
            for (int i = 0; i < sigNames.size(); ++i) {
                QString sn = sigNames[i].trimmed();
                bool actLow = groupNeg;
                if (sn.startsWith(QLatin1Char('!'))) {
                    actLow = true;
                    sn = sn.mid(1).trimmed();
                }
                if (sn.isEmpty()) continue;
                PinMeta &pm = pinMeta[sn.toLower()];
                if (pm.name.isEmpty()) pm.name = sn;
                if (pm.pinNumber.isEmpty() && i < pinNums.size()) {
                    pm.pinNumber = pinNums[i];
                }
                if (actLow) pm.activeLowInDesign = true;
            }
        }
    }
}

void parseWoFieldLines(const QString &woPath, QMap<QString, FieldDef> &fields) {
    QFile f(woPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine()).trimmed();
        if (line.startsWith(QStringLiteral("#F "), Qt::CaseInsensitive)) {
            QString rest = line.mid(3).trimmed();
            if (rest.endsWith(QLatin1Char(';'))) rest.chop(1);
            int eq = rest.indexOf(QLatin1Char('='));
            if (eq > 0) {
                QString fname = rest.left(eq).trimmed();
                QStringList members = SimDocument::expandBracketList(rest.mid(eq + 1).trimmed());
                if (!members.isEmpty() && !fields.contains(fname.toLower())) {
                    FieldDef fd;
                    fd.name = fname;
                    fd.members = members;
                    fd.definedInSi = false;
                    fields.insert(fname.toLower(), fd);
                }
            }
        }
    }
}

} // namespace

void SimDocument::clear() {
    header = SimHeader{};
    fileBase = BusRadix::Hex;
    hasExplicitBase = false;
    headerComments.clear();
    leadingSpacing = 0;
    signalsList.clear();
    vectors.clear();
    trailingMessages.clear();
    fields.clear();
    pinMeta.clear();
    filePath.clear();
    loadedFromWo = false;
    modified = false;
}

SimDocument SimDocument::createDefaultNew() {
    SimDocument doc;
    doc.header.name = QStringLiteral("Design");
    doc.header.partNo = QStringLiteral("00");
    doc.header.date = QDate::currentDate().toString(QStringLiteral("MM/dd/yyyy"));
    doc.header.revision = QStringLiteral("01");
    doc.header.designer = QStringLiteral("Engineer");
    doc.header.company = QStringLiteral("None");
    doc.header.assembly = QStringLiteral("None");
    doc.header.location = QStringLiteral("None");
    doc.header.device = QStringLiteral("g16v8a");

    SignalItem clk;
    clk.kind = SignalItem::Kind::Scalar;
    clk.name = QStringLiteral("clk");
    clk.members = {QStringLiteral("clk")};
    doc.signalsList.append(clk);

    SignalItem in0;
    in0.kind = SignalItem::Kind::Scalar;
    in0.name = QStringLiteral("in0");
    in0.members = {QStringLiteral("in0")};
    in0.spacingAfter = 1;
    doc.signalsList.append(in0);

    SignalItem out0;
    out0.kind = SignalItem::Kind::Scalar;
    out0.name = QStringLiteral("out0");
    out0.members = {QStringLiteral("out0")};
    doc.signalsList.append(out0);
    doc.pinMeta[QStringLiteral("out0")] = PinMeta{QStringLiteral("out0"), QString(), false, true, QString()};

    doc.insertVector(0, 8, QLatin1Char('0'), QLatin1Char('*'), false);
    doc.modified = false;
    return doc;
}

QChar SimDocument::normalizeStateChar(QChar c) {
    QChar up = c.toUpper();
    if (up == QLatin1Char('N')) {
        return QLatin1Char('N');
    }
    return up;
}

bool SimDocument::isValidStateChar(QChar c) {
    switch (c.toUpper().unicode()) {
    case '0':
    case '1':
    case 'C':
    case 'K':
    case 'P':
    case 'L':
    case 'H':
    case 'Z':
    case 'X':
    case '*':
    case 'N':
        return true;
    default:
        return false;
    }
}

bool SimDocument::expandRangeToken(const QString &token, QStringList &membersOut,
                                   bool *activeLowOut, QString *cleanRangeNameOut) {
    QString t = token.trimmed();
    bool actLow = false;
    if (t.startsWith(QLatin1Char('!'))) {
        actLow = true;
        t = t.mid(1).trimmed();
    }
    int dots = t.indexOf(QStringLiteral(".."));
    if (dots <= 0) {
        return false;
    }
    QString left = t.left(dots).trimmed();
    QString right = t.mid(dots + 2).trimmed();
    if (right.startsWith(QLatin1Char('!'))) {
        right = right.mid(1).trimmed();
    }
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }

    static const QRegularExpression reSplit(QStringLiteral("^(.*?)(\\d+)$"));
    auto ml = reSplit.match(left);
    if (!ml.hasMatch()) {
        return false;
    }
    const QString prefixL = ml.captured(1);
    const QString numStrL = ml.captured(2);
    bool okL = false;
    int startNum = numStrL.toInt(&okL);
    if (!okL) return false;

    QString prefixR;
    QString numStrR;
    bool onlyDigitsR = false;
    int endNum = right.toInt(&onlyDigitsR);
    if (onlyDigitsR) {
        prefixR = prefixL;
        numStrR = right;
    } else {
        auto mr = reSplit.match(right);
        if (!mr.hasMatch()) return false;
        prefixR = mr.captured(1);
        numStrR = mr.captured(2);
        bool okR = false;
        endNum = numStrR.toInt(&okR);
        if (!okR) return false;
        if (prefixL.compare(prefixR, Qt::CaseInsensitive) != 0) {
            return false;
        }
    }

    const int span = qAbs(endNum - startNum);
    if (span > 512) {
        return false;
    }

    int padWidth = 0;
    if (numStrL.size() > 1 && numStrL.startsWith(QLatin1Char('0')) && numStrL.size() == numStrR.size()) {
        padWidth = numStrL.size();
    }

    membersOut.clear();
    membersOut.reserve(span + 1);
    int step = (startNum <= endNum) ? 1 : -1;
    for (int v = startNum;; v += step) {
        QString numPart = (padWidth > 0)
                              ? QStringLiteral("%1").arg(v, padWidth, 10, QLatin1Char('0'))
                              : QString::number(v);
        membersOut.append(prefixL + numPart);
        if (v == endNum) break;
    }

    if (activeLowOut) *activeLowOut = actLow;
    if (cleanRangeNameOut) *cleanRangeNameOut = t;
    return true;
}

QStringList SimDocument::expandBracketList(const QString &bracketBody) {
    QString body = bracketBody.trimmed();
    if (body.startsWith(QLatin1Char('[')) && body.endsWith(QLatin1Char(']'))) {
        body = body.mid(1, body.size() - 2).trimmed();
    }
    QStringList out;
    const QStringList parts = body.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &rawPart : parts) {
        const QString p = rawPart.trimmed();
        if (p.isEmpty()) continue;
        if (p.contains(QStringLiteral(".."))) {
            QStringList expanded;
            bool actLow = false;
            if (expandRangeToken(p, expanded, &actLow)) {
                for (const QString &m : expanded) {
                    out.append(actLow ? (QStringLiteral("!") + m) : m);
                }
                continue;
            }
        }
        // Pure integer range like 2..9
        static const QRegularExpression reIntRange(QStringLiteral("^(\\d+)\\.\\.(\\d+)$"));
        auto mir = reIntRange.match(p);
        if (mir.hasMatch()) {
            int a = mir.captured(1).toInt();
            int b = mir.captured(2).toInt();
            int step = (a <= b) ? 1 : -1;
            if (qAbs(b - a) <= 512) {
                for (int v = a;; v += step) {
                    out.append(QString::number(v));
                    if (v == b) break;
                }
                continue;
            }
        }
        out.append(p);
    }
    return out;
}

QString SimDocument::findCompanionFile(const QString &basePath, const QString &extWithDot) {
    if (basePath.isEmpty()) return QString();
    QFileInfo fi(basePath);
    QDir dir = fi.dir();
    if (!dir.exists()) return QString();

    const QString targetName = fi.completeBaseName() + extWithDot;
    const QString direct = dir.filePath(targetName);
    if (QFileInfo::exists(direct)) {
        return direct;
    }
    const QString baseStem = fi.baseName() + extWithDot;
    const QStringList entries = dir.entryList(QDir::Files | QDir::Readable);
    for (const QString &entry : entries) {
        if (entry.compare(targetName, Qt::CaseInsensitive) == 0 ||
            entry.compare(baseStem, Qt::CaseInsensitive) == 0) {
            return dir.filePath(entry);
        }
    }
    return QString();
}

void SimDocument::loadCompanionMetadata(const QString &contextFilePath) {
    if (contextFilePath.isEmpty()) return;

    QStringList candidateBases = {contextFilePath};
    if (!header.name.trimmed().isEmpty()) {
        QFileInfo fi(contextFilePath);
        candidateBases.append(fi.dir().filePath(header.name.trimmed() + QStringLiteral(".si")));
    }

    for (const QString &cand : candidateBases) {
        const QString pldPath = findCompanionFile(cand, QStringLiteral(".pld"));
        if (!pldPath.isEmpty()) {
            parsePldMetadataFile(pldPath, fields, pinMeta);
        }
        const QString simPath = findCompanionFile(cand, QStringLiteral(".sim"));
        if (!simPath.isEmpty()) {
            parseSimMetadataFile(simPath, fields, pinMeta);
        }
        const QString woPath = findCompanionFile(cand, QStringLiteral(".wo"));
        if (!woPath.isEmpty() && woPath.compare(contextFilePath, Qt::CaseInsensitive) != 0) {
            parseWoFieldLines(woPath, fields);
        }
    }
}

bool SimDocument::loadFile(const QString &path, QString *errorOut) {
    if (errorOut) errorOut->clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("Could not open file: %1").arg(path);
        return false;
    }
    QByteArray raw = f.readAll();
    f.close();

    if (raw.contains('\0')) {
        if (errorOut) *errorOut = QStringLiteral("Binary or non-text file cannot be opened as .SI/.WO: %1").arg(path);
        return false;
    }
    raw.replace('\x1a', ' '); // Ctrl-Z marks the end of file in some inputs (e.g. Join2500.si)
    const QString text = QString::fromLatin1(raw);

    const QString trimmed = text.trimmed();
    bool isWo = trimmed.startsWith(QStringLiteral("#WAVEFORM"), Qt::CaseInsensitive) ||
                path.endsWith(QStringLiteral(".wo"), Qt::CaseInsensitive);

    bool ok = isWo ? loadWoText(text, path, errorOut) : loadSiText(text, path, errorOut);
    if (ok) {
        filePath = QFileInfo(path).absoluteFilePath();
        loadedFromWo = isWo;
        modified = false;
    }
    return ok;
}

static void parseHeaderStmt(const QString &stmt, SimHeader &hdr,
                            BusRadix &fileBase, bool &hasExplicitBase,
                            QMap<QString, FieldDef> &fields) {
    const QString s = stmt.trimmed();
    if (s.isEmpty()) return;

    // BASE: octal | decimal | hex | binary
    static const QRegularExpression reBase(
        QStringLiteral("^BASE\\s*:\\s*([A-Za-z]+)$"),
        QRegularExpression::CaseInsensitiveOption);
    auto mb = reBase.match(s);
    if (mb.hasMatch()) {
        const QString b = mb.captured(1).toLower();
        hasExplicitBase = true;
        if (b.startsWith(QStringLiteral("oct"))) fileBase = BusRadix::Octal;
        else if (b.startsWith(QStringLiteral("dec"))) fileBase = BusRadix::Decimal;
        else if (b.startsWith(QStringLiteral("bin"))) fileBase = BusRadix::Binary;
        else fileBase = BusRadix::Hex;
        return;
    }

    // FIELD name = [...]
    static const QRegularExpression reField(
        QStringLiteral("^FIELD\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    auto mf = reField.match(s);
    if (mf.hasMatch()) {
        const QString fname = mf.captured(1).trimmed();
        const QStringList members = SimDocument::expandBracketList(mf.captured(2));
        if (!members.isEmpty()) {
            FieldDef fd;
            fd.name = fname;
            fd.members = members;
            fd.definedInSi = true;
            fields.insert(fname.toLower(), fd);
        }
        return;
    }

    // Standard CUPL header keyword: Key Value
    static const QRegularExpression reHdr(
        QStringLiteral("^(Name|PartNo|Date|Revision|Rev|Designer|Engineer|Company|Assembly|Location|Device)\\b(.*)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    auto mh = reHdr.match(s);
    if (mh.hasMatch()) {
        const QString key = mh.captured(1).toLower();
        const QString val = mh.captured(2).trimmed();
        if (key == QStringLiteral("name")) hdr.name = val;
        else if (key == QStringLiteral("partno")) hdr.partNo = val;
        else if (key == QStringLiteral("date")) hdr.date = val;
        else if (key == QStringLiteral("revision") || key == QStringLiteral("rev")) hdr.revision = val;
        else if (key == QStringLiteral("designer") || key == QStringLiteral("engineer")) hdr.designer = val;
        else if (key == QStringLiteral("company")) hdr.company = val;
        else if (key == QStringLiteral("assembly")) hdr.assembly = val;
        else if (key == QStringLiteral("location")) hdr.location = val;
        else if (key == QStringLiteral("device")) hdr.device = val;
    }
}

static QVector<SignalItem> parseOrderClause(const QString &orderBody,
                                            const QMap<QString, FieldDef> &fields,
                                            int *leadingSpacingOut) {
    QVector<SignalItem> items;
    if (leadingSpacingOut) *leadingSpacingOut = 0;

    const QString clean = removeBlockComments(orderBody);
    const QStringList tokens = clean.split(QLatin1Char(','));
    for (const QString &rawTok : tokens) {
        QString tok = rawTok.trimmed();
        if (tok.isEmpty()) continue;

        if (tok.startsWith(QLatin1Char('%'))) {
            bool ok = false;
            int sp = tok.mid(1).trimmed().toInt(&ok);
            if (ok && sp > 0) {
                if (items.isEmpty()) {
                    if (leadingSpacingOut) *leadingSpacingOut += sp;
                } else {
                    items.back().spacingAfter += sp;
                }
            }
            continue;
        }

        if (tok.contains(QStringLiteral(".."))) {
            QStringList rangeMembers;
            bool actLow = false;
            QString cleanRange;
            if (SimDocument::expandRangeToken(tok, rangeMembers, &actLow, &cleanRange)) {
                SignalItem si;
                si.kind = SignalItem::Kind::RangeBus;
                si.name = cleanRange;
                si.activeLow = actLow;
                si.members = rangeMembers;
                items.append(si);
                continue;
            }
        }

        bool actLow = false;
        if (tok.startsWith(QLatin1Char('!'))) {
            actLow = true;
            tok = tok.mid(1).trimmed();
        }
        if (tok.isEmpty()) continue;

        auto fit = fields.find(tok.toLower());
        if (fit != fields.end() && fit->members.size() > 1) {
            SignalItem si;
            si.kind = SignalItem::Kind::FieldBus;
            si.name = tok;
            si.activeLow = actLow;
            QStringList cleanMembers;
            for (QString m : fit->members) {
                if (m.startsWith(QLatin1Char('!'))) m = m.mid(1).trimmed();
                cleanMembers.append(m);
            }
            si.members = cleanMembers;
            items.append(si);
        } else {
            SignalItem si;
            si.kind = SignalItem::Kind::Scalar;
            si.name = tok;
            si.activeLow = actLow;
            si.members = {tok};
            items.append(si);
        }
    }
    return items;
}

bool SimDocument::loadSiText(const QString &rawText, const QString &contextFilePath, QString *errorOut) {
    if (errorOut) errorOut->clear();
    clear();

    QString text = rawText;
    text.replace(QChar(0x1A), QLatin1Char(' '));

    // Build comment-masked string upfront so "Order:" / "Vectors:" inside /* ... */ comments
    // (e.g. GATES.SI, ROULETTE.si) never falsely match keyword scans!
    const QString masked = maskBlockComments(text);

    // Pre-scan header statements before first ORDER:/VECTORS: so header.name is available for companion lookup
    {
        int firstOrderOrVec = masked.indexOf(QRegularExpression(
            QStringLiteral("\\b(ORDER|VECTORS)\\s*:"), QRegularExpression::CaseInsensitiveOption));
        const QString pre = (firstOrderOrVec >= 0) ? text.left(firstOrderOrVec) : text;
        QStringList extractedComments;
        const QString preNoComments = removeBlockComments(pre, &extractedComments);
        headerComments = extractedComments;
        const QStringList stmts = preNoComments.split(QLatin1Char(';'));
        for (const QString &st : stmts) {
            parseHeaderStmt(st, header, fileBase, hasExplicitBase, fields);
        }
    }

    // Load companion .pld / .sim / .wo metadata (will NOT overwrite fields with definedInSi=true)
    loadCompanionMetadata(contextFilePath);

    // Re-apply .SI-defined fields/headers so .SI takes precedence over companion files
    {
        int firstVec = masked.indexOf(QRegularExpression(
            QStringLiteral("\\bVECTORS\\s*:"), QRegularExpression::CaseInsensitiveOption));
        const QString pre = (firstVec >= 0) ? text.left(firstVec) : text;
        const QString preNoComments = removeBlockComments(pre);
        // Strip ORDER: ... ; clauses before splitting header statements
        QString withoutOrders = preNoComments;
        withoutOrders.remove(QRegularExpression(
            QStringLiteral("\\bORDER\\s*:[^;]*;"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
        for (const QString &st : withoutOrders.split(QLatin1Char(';'))) {
            parseHeaderStmt(st, header, fileBase, hasExplicitBase, fields);
        }
    }

    // Locate all ORDER: ... ; VECTORS: ... sections (handles standard single section & multi-ORDER like Join2500.si)
    struct SectionSpan {
        QString orderBody;
        QString vectorsBody;
    };
    QVector<SectionSpan> sections;

    static const QRegularExpression reOrder(
        QStringLiteral("\\bORDER\\s*:(.*?);"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression reVectors(
        QStringLiteral("\\bVECTORS\\s*:"),
        QRegularExpression::CaseInsensitiveOption);

    auto orderIt = reOrder.globalMatch(masked);
    QVector<QRegularExpressionMatch> orderMatches;
    while (orderIt.hasNext()) {
        orderMatches.append(orderIt.next());
    }

    if (orderMatches.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Missing ORDER: statement in .SI file.");
        return false;
    }

    for (int oi = 0; oi < orderMatches.size(); ++oi) {
        const auto &om = orderMatches[oi];
        const QString orderRaw = text.mid(om.capturedStart(1), om.capturedLength(1));
        int searchStart = om.capturedEnd(0);
        int nextOrderStart = (oi + 1 < orderMatches.size()) ? orderMatches[oi + 1].capturedStart(0) : text.size();

        auto vm = reVectors.match(masked, searchStart);
        QString vecRaw;
        if (vm.hasMatch() && vm.capturedStart(0) < nextOrderStart) {
            int vecBodyStart = vm.capturedEnd(0);
            vecRaw = text.mid(vecBodyStart, nextOrderStart - vecBodyStart);
        }
        sections.append(SectionSpan{orderRaw, vecRaw});
    }

    // Build master signalsList (merging unique items across sections if multiple ORDER: blocks exist)
    QSet<QString> seenPinsLower;
    QVector<QVector<SignalItem>> sectionOrders;
    sectionOrders.reserve(sections.size());

    for (int si = 0; si < sections.size(); ++si) {
        int secLead = 0;
        QVector<SignalItem> secItems = parseOrderClause(sections[si].orderBody, fields, &secLead);
        if (si == 0) {
            leadingSpacing = secLead;
        }
        sectionOrders.append(secItems);
        for (const SignalItem &item : secItems) {
            bool anyNew = false;
            for (const QString &m : item.members) {
                if (!seenPinsLower.contains(m.toLower())) {
                    anyNew = true;
                }
            }
            if (anyNew) {
                signalsList.append(item);
                for (const QString &m : item.members) {
                    seenPinsLower.insert(m.toLower());
                }
            }
        }
    }

    if (signalsList.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("No signals defined in ORDER: clause.");
        return false;
    }

    // Parse VECTORS: for each section
    const int radix = static_cast<int>(fileBase);

    for (int secIdx = 0; secIdx < sections.size(); ++secIdx) {
        const QVector<SignalItem> &secOrder = sectionOrders[secIdx];
        if (secOrder.isEmpty()) continue;
        const QString &vtext = sections[secIdx].vectorsBody;
        const int n = vtext.size();
        int pos = 0;

        QStringList pendingMsgs;
        int pendingRepeat = 1;

        auto skipWsAndStandaloneComments = [&]() {
            while (pos < n) {
                if (vtext[pos].isSpace() || vtext[pos] == QLatin1Char(';')) {
                    pos++;
                } else if (pos + 1 < n && vtext[pos] == QLatin1Char('/') && vtext[pos + 1] == QLatin1Char('*')) {
                    pos += 2;
                    while (pos < n) {
                        if (pos + 1 < n && vtext[pos] == QLatin1Char('*') && vtext[pos + 1] == QLatin1Char('/')) {
                            pos += 2;
                            break;
                        }
                        pos++;
                    }
                } else {
                    break;
                }
            }
        };

        while (pos < n) {
            skipWsAndStandaloneComments();
            if (pos >= n) break;

            // Check for $MSG or $REPEAT
            if (vtext[pos] == QLatin1Char('$')) {
                int kwStart = pos + 1;
                int kwEnd = kwStart;
                while (kwEnd < n && vtext[kwEnd].isLetter()) kwEnd++;
                const QString kw = vtext.mid(kwStart, kwEnd - kwStart).toUpper();
                pos = kwEnd;
                while (pos < n && (vtext[pos] == QLatin1Char(' ') || vtext[pos] == QLatin1Char('\t'))) pos++;

                if (kw == QStringLiteral("MSG")) {
                    QString msgStr;
                    if (pos < n && (vtext[pos] == QLatin1Char('"') || vtext[pos] == QLatin1Char('\''))) {
                        QChar qc = vtext[pos++];
                        int mStart = pos;
                        while (pos < n && vtext[pos] != qc && vtext[pos] != QLatin1Char('\n')) pos++;
                        msgStr = vtext.mid(mStart, pos - mStart);
                        if (pos < n && vtext[pos] == qc) pos++;
                    } else {
                        int mStart = pos;
                        while (pos < n && vtext[pos] != QLatin1Char(';') && vtext[pos] != QLatin1Char('\n')) pos++;
                        msgStr = vtext.mid(mStart, pos - mStart).trimmed();
                    }
                    while (pos < n && (vtext[pos] == QLatin1Char(' ') || vtext[pos] == QLatin1Char('\t'))) pos++;
                    if (pos < n && vtext[pos] == QLatin1Char(';')) pos++;
                    pendingMsgs.append(msgStr);
                    continue;
                } else if (kw == QStringLiteral("REPEAT")) {
                    int dStart = pos;
                    while (pos < n && vtext[pos].isDigit()) pos++;
                    bool okR = false;
                    int rep = vtext.mid(dStart, pos - dStart).toInt(&okR);
                    if (okR && rep > 0) pendingRepeat = rep;
                    if (pos < n && vtext[pos] == QLatin1Char(';')) pos++;
                    continue;
                }
            }

            // Parse one full vector across `secOrder`
            VectorRow row;
            // Default all signals not in this section to 'X'
            for (const SignalItem &masterSig : signalsList) {
                for (const QString &m : masterSig.members) {
                    row.pinStates.insert(m.toLower(), QLatin1Char('X'));
                }
            }

            for (int itemIdx = 0; itemIdx < secOrder.size(); ++itemIdx) {
                const SignalItem &item = secOrder[itemIdx];
                const int w = item.width();
                int bitIdx = 0;

                while (bitIdx < w) {
                    skipWsAndStandaloneComments();
                    if (pos >= n) {
                        if (errorOut) {
                            *errorOut = QStringLiteral("Unexpected end of VECTORS: while reading signal '%1'.")
                                            .arg(item.displayLabel());
                        }
                        return false;
                    }

                    const QChar ch = vtext[pos];
                    if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
                        const bool driveOut = (ch == QLatin1Char('"'));
                        const QChar qc = ch;
                        pos++;
                        int qStart = pos;
                        while (pos < n && vtext[pos] != qc && vtext[pos] != QLatin1Char('\n')) {
                            pos++;
                        }
                        const QString lit = vtext.mid(qStart, pos - qStart);
                        if (pos < n && vtext[pos] == qc) pos++;

                        const int targetWidth = w - bitIdx;
                        QVector<QChar> expanded;
                        QString convErr;
                        if (!radixStringToStates(lit, radix, targetWidth, driveOut, expanded, &convErr)) {
                            if (errorOut) *errorOut = convErr;
                            return false;
                        }
                        for (int k = 0; k < targetWidth; ++k) {
                            row.pinStates.insert(item.members[bitIdx + k].toLower(), expanded[k]);
                        }
                        bitIdx = w;
                    } else if (isValidStateChar(ch)) {
                        row.pinStates.insert(item.members[bitIdx].toLower(), normalizeStateChar(ch));
                        pos++;
                        bitIdx++;
                    } else {
                        if (errorOut) {
                            *errorOut = QStringLiteral("Invalid vector state character '%1' near signal '%2'.")
                                            .arg(ch, item.displayLabel());
                        }
                        return false;
                    }
                }
            }

            // Capture optional inline /* ... */ comment on the remainder of the same line
            while (pos < n && (vtext[pos] == QLatin1Char(' ') || vtext[pos] == QLatin1Char('\t') || vtext[pos] == QLatin1Char(';'))) {
                pos++;
            }
            if (pos + 1 < n && vtext[pos] == QLatin1Char('/') && vtext[pos + 1] == QLatin1Char('*')) {
                pos += 2;
                int cStart = pos;
                while (pos < n) {
                    if (pos + 1 < n && vtext[pos] == QLatin1Char('*') && vtext[pos + 1] == QLatin1Char('/')) {
                        row.comment = vtext.mid(cStart, pos - cStart).trimmed();
                        pos += 2;
                        break;
                    }
                    pos++;
                }
            }

            row.messagesBefore = pendingMsgs;
            pendingMsgs.clear();

            for (int r = 0; r < pendingRepeat; ++r) {
                VectorRow copy = row;
                if (r > 0) copy.messagesBefore.clear();
                vectors.append(copy);
            }
            pendingRepeat = 1;
        }

        if (!pendingMsgs.isEmpty()) {
            trailingMessages.append(pendingMsgs);
        }
    }

    // Infer output flags from vector states if not already known from .sim
    for (const SignalItem &sig : signalsList) {
        for (const QString &m : sig.members) {
            const QString key = m.toLower();
            PinMeta &pm = pinMeta[key];
            if (pm.name.isEmpty()) pm.name = m;
            if (sig.activeLow && sig.members.size() == 1) {
                pm.activeLowInDesign = true;
            }
            if (!pm.isOutput) {
                for (const VectorRow &vr : vectors) {
                    QChar st = vr.pinStates.value(key, QLatin1Char('X'));
                    if (st == QLatin1Char('L') || st == QLatin1Char('H') ||
                        st == QLatin1Char('Z') || st == QLatin1Char('*')) {
                        pm.isOutput = true;
                        break;
                    }
                }
            }
        }
    }

    return true;
}

bool SimDocument::loadWoText(const QString &rawText, const QString &contextFilePath, QString *errorOut) {
    if (errorOut) errorOut->clear();
    clear();

    QString text = rawText;
    text.replace(QChar(0x1A), QLatin1Char(' '));

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")));
    QStringList hLines;
    QVector<QPair<int, QString>> vLines;
    struct PendingErr {
        int vecIndex;
        SimError err;
    };
    QVector<PendingErr> errLines;

    for (const QString &rawLine : lines) {
        const QString trimmed = rawLine.trimmed();
        if (trimmed.isEmpty()) continue;
        if (trimmed.startsWith(QStringLiteral("#WAVEFORM"), Qt::CaseInsensitive)) {
            continue;
        }
        if (rawLine.startsWith(QStringLiteral("#H")) &&
            (rawLine.size() == 2 || rawLine[2].isSpace())) {
            hLines.append(rawLine.size() > 3 ? rawLine.mid(3) : QString());
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("#F "), Qt::CaseInsensitive)) {
            QString rest = trimmed.mid(3).trimmed();
            if (rest.endsWith(QLatin1Char(';'))) rest.chop(1);
            int eq = rest.indexOf(QLatin1Char('='));
            if (eq > 0) {
                QString fname = rest.left(eq).trimmed();
                QStringList members = expandBracketList(rest.mid(eq + 1).trimmed());
                if (!members.isEmpty()) {
                    FieldDef fd;
                    fd.name = fname;
                    fd.members = members;
                    fd.definedInSi = false;
                    fields.insert(fname.toLower(), fd);
                }
            }
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("#V "), Qt::CaseInsensitive)) {
            static const QRegularExpression reV(QStringLiteral("^#V\\s+(\\d+)\\s+(\\S+)"),
                                                QRegularExpression::CaseInsensitiveOption);
            auto mv = reV.match(trimmed);
            if (mv.hasMatch()) {
                vLines.append(qMakePair(mv.captured(1).toInt(), mv.captured(2)));
            }
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("#E "), Qt::CaseInsensitive)) {
            static const QRegularExpression reE(QStringLiteral("^#E\\s+(\\d+)\\s+(\\S)\\s+(\\S+)"),
                                                QRegularExpression::CaseInsensitiveOption);
            auto me = reE.match(trimmed);
            if (me.hasMatch() && !vLines.isEmpty()) {
                SimError se;
                se.code = me.captured(1).toInt();
                se.expected = me.captured(2).at(0).toUpper();
                se.pinName = me.captured(3).trimmed();
                errLines.append(PendingErr{static_cast<int>(vLines.size() - 1), se});
            }
            continue;
        }
    }

    // Parse #H header & ORDER: lines (which are exact copies of the pre-VECTORS .SI lines!)
    const QString hText = hLines.join(QLatin1Char('\n'));
    QStringList extractedComments;
    const QString hNoComments = removeBlockComments(hText, &extractedComments);
    headerComments = extractedComments;

    // Parse headers & .SI-local FIELD statements from #H
    {
        QString withoutOrders = hNoComments;
        withoutOrders.remove(QRegularExpression(
            QStringLiteral("\\bORDER\\s*:[^;]*;"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
        for (const QString &st : withoutOrders.split(QLatin1Char(';'))) {
            parseHeaderStmt(st, header, fileBase, hasExplicitBase, fields);
        }
    }

    // Load companion .pld / .sim metadata
    loadCompanionMetadata(contextFilePath);

    // Parse ORDER: from #H
    static const QRegularExpression reOrder(
        QStringLiteral("\\bORDER\\s*:(.*?);"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    auto om = reOrder.match(hNoComments);
    if (om.hasMatch()) {
        signalsList = parseOrderClause(om.captured(1), fields, &leadingSpacing);
    }

    if (signalsList.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Missing #H ORDER: definition in .WO file.");
        return false;
    }

    const QStringList flatPins = allOrderedPins();
    vectors.reserve(vLines.size());
    for (const auto &vp : vLines) {
        const QString &bits = vp.second;
        VectorRow row;
        for (int i = 0; i < flatPins.size(); ++i) {
            QChar st = (i < bits.size() && isValidStateChar(bits[i]))
                           ? normalizeStateChar(bits[i])
                           : QLatin1Char('X');
            row.pinStates.insert(flatPins[i].toLower(), st);
        }
        vectors.append(row);
    }

    for (const PendingErr &pe : errLines) {
        if (pe.vecIndex >= 0 && pe.vecIndex < vectors.size()) {
            vectors[pe.vecIndex].errors.append(pe.err);
        }
    }

    // Preserve vector comments & $MSG annotations from companion .si if vector counts match
    const QString companionSi = findCompanionFile(contextFilePath, QStringLiteral(".si"));
    if (!companionSi.isEmpty()) {
        SimDocument siDoc;
        if (siDoc.loadFile(companionSi) && siDoc.vectors.size() == vectors.size()) {
            for (int i = 0; i < vectors.size(); ++i) {
                vectors[i].comment = siDoc.vectors[i].comment;
                vectors[i].messagesBefore = siDoc.vectors[i].messagesBefore;
            }
            trailingMessages = siDoc.trailingMessages;
        }
    }

    // Infer output pins from states if not already marked
    for (const QString &pin : flatPins) {
        const QString key = pin.toLower();
        PinMeta &pm = pinMeta[key];
        if (pm.name.isEmpty()) pm.name = pin;
        if (!pm.isOutput) {
            for (const VectorRow &vr : vectors) {
                QChar st = vr.pinStates.value(key, QLatin1Char('X'));
                if (st == QLatin1Char('L') || st == QLatin1Char('H') ||
                    st == QLatin1Char('Z') || st == QLatin1Char('*')) {
                    pm.isOutput = true;
                    break;
                }
            }
        }
    }

    return true;
}

QStringList SimDocument::allOrderedPins() const {
    QStringList out;
    for (const SignalItem &sig : signalsList) {
        for (const QString &m : sig.members) {
            out.append(m);
        }
    }
    return out;
}

QChar SimDocument::pinState(int vecIdx, const QString &pinName) const {
    if (vecIdx < 0 || vecIdx >= vectors.size()) return QLatin1Char('X');
    return vectors[vecIdx].pinStates.value(pinName.toLower(), QLatin1Char('X'));
}

void SimDocument::setPinState(int vecIdx, const QString &pinName, QChar state) {
    if (vecIdx < 0 || vecIdx >= vectors.size()) return;
    if (!isValidStateChar(state)) return;
    const QString key = pinName.toLower();
    vectors[vecIdx].pinStates[key] = normalizeStateChar(state);
    // Clear any stale #E mismatch for this edited pin
    auto &errs = vectors[vecIdx].errors;
    for (int i = errs.size() - 1; i >= 0; --i) {
        if (errs[i].pinName.compare(pinName, Qt::CaseInsensitive) == 0) {
            errs.removeAt(i);
        }
    }
    modified = true;
}

bool SimDocument::isPinOutput(const QString &pinName) const {
    auto it = pinMeta.find(pinName.toLower());
    if (it != pinMeta.end() && it->isOutput) return true;
    for (const VectorRow &vr : vectors) {
        QChar st = vr.pinStates.value(pinName.toLower(), QLatin1Char('X'));
        if (st == QLatin1Char('L') || st == QLatin1Char('H') ||
            st == QLatin1Char('Z') || st == QLatin1Char('*')) {
            return true;
        }
    }
    return false;
}

bool SimDocument::isSignalOutput(const SignalItem &sig) const {
    for (const QString &m : sig.members) {
        if (isPinOutput(m)) return true;
    }
    return false;
}

const SimError *SimDocument::findError(int vecIdx, const QString &pinName) const {
    if (vecIdx < 0 || vecIdx >= vectors.size()) return nullptr;
    for (const SimError &se : vectors[vecIdx].errors) {
        if (se.pinName.compare(pinName, Qt::CaseInsensitive) == 0) {
            return &se;
        }
    }
    return nullptr;
}

bool SimDocument::hasErrorInSignal(int vecIdx, const SignalItem &sig) const {
    if (vecIdx < 0 || vecIdx >= vectors.size()) return false;
    if (vectors[vecIdx].errors.isEmpty()) return false;
    for (const QString &m : sig.members) {
        if (findError(vecIdx, m) != nullptr) return true;
    }
    return false;
}

QString SimDocument::busBitString(const SignalItem &sig, int vecIdx) const {
    QString s;
    s.reserve(sig.members.size());
    for (const QString &m : sig.members) {
        s.append(pinState(vecIdx, m));
    }
    return s;
}

QString SimDocument::formatBusValue(const SignalItem &sig, int vecIdx, BusRadix radix,
                                    bool includeSuffix, bool *isCleanNumeric) const {
    if (isCleanNumeric) *isCleanNumeric = false;
    if (vecIdx < 0 || vecIdx >= vectors.size() || sig.members.isEmpty()) {
        return QStringLiteral("?");
    }

    bool allZ = true;
    bool allX = true;
    bool allSim = true;
    bool allNumeric = true;
    QVector<int> bits;
    bits.reserve(sig.members.size());

    for (const QString &m : sig.members) {
        QChar st = pinState(vecIdx, m);
        if (st != QLatin1Char('Z')) allZ = false;
        if (st != QLatin1Char('X') && st != QLatin1Char('N')) allX = false;
        if (st != QLatin1Char('*')) allSim = false;

        if (st == QLatin1Char('0') || st == QLatin1Char('L')) {
            bits.append(0);
        } else if (st == QLatin1Char('1') || st == QLatin1Char('H')) {
            bits.append(1);
        } else {
            allNumeric = false;
        }
    }

    if (allZ) return QStringLiteral("High-Z");
    if (allX) return QStringLiteral("X");
    if (allSim) return QStringLiteral("*");

    if (!allNumeric) {
        return busBitString(sig, vecIdx);
    }

    if (isCleanNumeric) *isCleanNumeric = true;
    const QString digits = bitsToRadixString(bits, static_cast<int>(radix));
    if (!includeSuffix) {
        return digits;
    }
    switch (radix) {
    case BusRadix::Hex:
        return digits + QLatin1Char('h');
    case BusRadix::Binary:
        return digits + QLatin1Char('b');
    case BusRadix::Octal:
        return digits + QLatin1Char('o');
    case BusRadix::Decimal:
        return digits + QLatin1Char('d');
    }
    return digits;
}

bool SimDocument::setBusValueFromString(int sigIdx, int startVec, int endVec,
                                        const QString &valueText, BusRadix radix,
                                        bool driveAsOutput, QString *errorOut) {
    if (sigIdx < 0 || sigIdx >= signalsList.size()) return false;
    if (vectors.isEmpty()) return false;

    int v0 = qBound(0, qMin(startVec, endVec), vectors.size() - 1);
    int v1 = qBound(0, qMax(startVec, endVec), vectors.size() - 1);
    const SignalItem &sig = signalsList[sigIdx];

    QVector<QChar> expanded;
    if (!radixStringToStates(stripOuterQuotes(valueText), static_cast<int>(radix),
                              sig.width(), driveAsOutput, expanded, errorOut)) {
        return false;
    }

    for (int v = v0; v <= v1; ++v) {
        for (int b = 0; b < sig.members.size(); ++b) {
            setPinState(v, sig.members[b], expanded[b]);
        }
    }
    modified = true;
    return true;
}

QString SimDocument::serializeSi(const QString &targetSiPath) const {
    QString out;
    QTextStream ts(&out);

    auto writeHdr = [&](const char *label, const QString &val, const char *fallback = "") {
        const QString v = val.isEmpty() ? QString::fromLatin1(fallback) : val;
        ts << QString::fromLatin1(label) << v << ";\n";
    };

    writeHdr("Name     ", header.name, "Design");
    writeHdr("PartNo   ", header.partNo, "00");
    writeHdr("Date     ", header.date);
    writeHdr("Revision ", header.revision, "01");
    writeHdr("Designer ", header.designer);
    writeHdr("Company  ", header.company);
    writeHdr("Assembly ", header.assembly, "None");
    writeHdr("Location ", header.location, "None");
    writeHdr("Device   ", header.device);
    ts << "\n";

    for (const QString &c : headerComments) {
        ts << c << "\n";
    }
    if (!headerComments.isEmpty()) {
        ts << "\n";
    }

    if (hasExplicitBase || fileBase == BusRadix::Octal || fileBase == BusRadix::Decimal) {
        if (fileBase == BusRadix::Octal) ts << "BASE: octal;\n\n";
        else if (fileBase == BusRadix::Decimal) ts << "BASE: decimal;\n\n";
        else if (fileBase == BusRadix::Hex && hasExplicitBase) ts << "BASE: hex;\n\n";
    }

    // Check external companion .pld/.sim fields so we NEVER emit duplicate FIELD definitions
    QMap<QString, FieldDef> externalFields;
    QMap<QString, PinMeta> dummyPins;
    const QString lookupBase = targetSiPath.isEmpty() ? filePath : targetSiPath;
    if (!lookupBase.isEmpty()) {
        QStringList candBases = {lookupBase};
        if (!header.name.trimmed().isEmpty()) {
            candBases.append(QFileInfo(lookupBase).dir().filePath(header.name.trimmed() + QStringLiteral(".si")));
        }
        for (const QString &cb : candBases) {
            const QString pld = findCompanionFile(cb, QStringLiteral(".pld"));
            if (!pld.isEmpty()) parsePldMetadataFile(pld, externalFields, dummyPins);
            const QString sim = findCompanionFile(cb, QStringLiteral(".sim"));
            if (!sim.isEmpty()) parseSimMetadataFile(sim, externalFields, dummyPins);
        }
    }

    QSet<QString> emittedFields;
    // Emit any FieldBus used in ORDER: or explicitly defined in .SI that isn't already in .pld/.sim
    for (const SignalItem &sig : signalsList) {
        if (sig.kind == SignalItem::Kind::FieldBus) {
            const QString key = sig.name.toLower();
            if (!externalFields.contains(key) && !emittedFields.contains(key)) {
                ts << "FIELD " << sig.name << " = [" << sig.members.join(QLatin1Char(',')) << "];\n";
                emittedFields.insert(key);
            }
        }
    }
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (it->definedInSi && !externalFields.contains(it.key()) && !emittedFields.contains(it.key())) {
            ts << "FIELD " << it->name << " = [" << it->members.join(QLatin1Char(',')) << "];\n";
            emittedFields.insert(it.key());
        }
    }
    if (!emittedFields.isEmpty()) {
        ts << "\n";
    }

    // ORDER: clause (preserving %N spacing tokens & ! active-low polarity & Q3..Q0 ranges)
    ts << "ORDER: ";
    QStringList orderTokens;
    if (leadingSpacing > 0) {
        orderTokens.append(QStringLiteral("%%1").arg(leadingSpacing));
    }
    for (const SignalItem &sig : signalsList) {
        orderTokens.append(sig.orderToken());
        if (sig.spacingAfter > 0) {
            orderTokens.append(QStringLiteral("%%1").arg(sig.spacingAfter));
        }
    }
    ts << orderTokens.join(QStringLiteral(", ")) << ";\n\n\n";

    // VECTORS: section
    // Preserve original expected test-vector state whenever an unresolved #E mismatch is present
    // (until manually edited or explicitly resolved via Assign Simulator Results).
    auto serializedPinState = [&](int vecIdx, const QString &pinName) -> QChar {
        if (const SimError *se = findError(vecIdx, pinName)) {
            if (isValidStateChar(se->expected)) {
                return normalizeStateChar(se->expected);
            }
        }
        return pinState(vecIdx, pinName);
    };

    ts << "VECTORS:\n";
    for (int v = 0; v < vectors.size(); ++v) {
        const VectorRow &row = vectors[v];
        for (const QString &msg : row.messagesBefore) {
            ts << "$MSG  \"" << msg << "\";\n";
        }

        QString vecLine;
        bool prevWasQuoted = false;
        for (int s = 0; s < signalsList.size(); ++s) {
            const SignalItem &sig = signalsList[s];
            if (!sig.isBus()) {
                if (prevWasQuoted) vecLine.append(QLatin1Char(' '));
                vecLine.append(serializedPinState(v, sig.members.first()));
                prevWasQuoted = false;
            } else {
                const int w = sig.width();
                bool allIn01 = true;
                bool allOutLH = true;
                QVector<int> bits;
                bits.reserve(w);
                for (const QString &m : sig.members) {
                    QChar st = serializedPinState(v, m);
                    if (st == QLatin1Char('0')) {
                        allOutLH = false;
                        bits.append(0);
                    } else if (st == QLatin1Char('1')) {
                        allOutLH = false;
                        bits.append(1);
                    } else if (st == QLatin1Char('L')) {
                        allIn01 = false;
                        bits.append(0);
                    } else if (st == QLatin1Char('H')) {
                        allIn01 = false;
                        bits.append(1);
                    } else {
                        allIn01 = false;
                        allOutLH = false;
                    }
                }

                bool canQuoteHex = (fileBase == BusRadix::Hex && w >= 4 && (w % 4 == 0) && (allIn01 || allOutLH));
                bool canQuoteOct = (fileBase == BusRadix::Octal && w >= 3 && (w % 3 == 0) && (allIn01 || allOutLH));
                if (canQuoteHex || canQuoteOct) {
                    if (!vecLine.isEmpty() && !vecLine.endsWith(QLatin1Char(' '))) {
                        vecLine.append(QLatin1Char(' '));
                    }
                    const int r = canQuoteHex ? 16 : 8;
                    const QChar qc = allOutLH ? QLatin1Char('"') : QLatin1Char('\'');
                    vecLine.append(qc);
                    vecLine.append(bitsToRadixString(bits, r));
                    vecLine.append(qc);
                    prevWasQuoted = true;
                } else {
                    if (prevWasQuoted) vecLine.append(QLatin1Char(' '));
                    for (const QString &m : sig.members) {
                        vecLine.append(serializedPinState(v, m));
                    }
                    prevWasQuoted = false;
                }
            }
        }

        if (!row.comment.isEmpty()) {
            vecLine.append(QStringLiteral("  /* %1 */").arg(row.comment));
        }
        ts << vecLine << "\n";
    }

    for (const QString &msg : trailingMessages) {
        ts << "$MSG  \"" << msg << "\";\n";
    }

    return out;
}

bool SimDocument::saveSiFile(const QString &path, QString *errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Could not write file: %1").arg(path);
        return false;
    }
    const QString siText = serializeSi(path);
    f.write(siText.toLatin1());
    f.close();

    filePath = QFileInfo(path).absoluteFilePath();
    loadedFromWo = false;
    modified = false;
    return true;
}

bool SimDocument::mergeSimulatedWo(const QString &woPath, QString *errorOut) {
    SimDocument woDoc;
    if (!woDoc.loadFile(woPath, errorOut)) {
        return false;
    }

    // Update field & pin metadata discovered during simulation
    for (auto it = woDoc.fields.begin(); it != woDoc.fields.end(); ++it) {
        if (!fields.contains(it.key())) {
            fields.insert(it.key(), it.value());
        }
    }
    for (auto it = woDoc.pinMeta.begin(); it != woDoc.pinMeta.end(); ++it) {
        PinMeta &dst = pinMeta[it.key()];
        if (dst.name.isEmpty()) dst.name = it->name;
        if (dst.pinNumber.isEmpty()) dst.pinNumber = it->pinNumber;
        if (it->isOutput) dst.isOutput = true;
        if (dst.equation.isEmpty()) dst.equation = it->equation;
    }

    // Preserve existing signalsList (including expanded state, custom colors, radix, bus groupings),
    // while updating vector pin states & #E mismatch records from woDoc!
    if (vectors.size() == woDoc.vectors.size()) {
        for (int v = 0; v < vectors.size(); ++v) {
            for (auto pit = woDoc.vectors[v].pinStates.begin(); pit != woDoc.vectors[v].pinStates.end(); ++pit) {
                vectors[v].pinStates[pit.key()] = pit.value();
            }
            vectors[v].errors = woDoc.vectors[v].errors;
        }
    } else {
        vectors = woDoc.vectors;
    }
    return true;
}

void SimDocument::assignSimulatorResults() {
    for (VectorRow &vr : vectors) {
        vr.errors.clear();
    }
    modified = true;
}

int SimDocument::insertVector(int index, int count, QChar defaultIn, QChar defaultOut, bool copyFromNeighbor) {
    if (count <= 0) return index;
    int insertPos = qBound(0, index, vectors.size());

    VectorRow templateRow;
    if (copyFromNeighbor && !vectors.isEmpty()) {
        int srcIdx = qBound(0, insertPos - 1, vectors.size() - 1);
        templateRow = vectors[srcIdx];
        templateRow.comment.clear();
        templateRow.messagesBefore.clear();
        templateRow.errors.clear();
    } else {
        for (const SignalItem &sig : signalsList) {
            for (const QString &m : sig.members) {
                templateRow.pinStates.insert(m.toLower(), isPinOutput(m) ? defaultOut : defaultIn);
            }
        }
    }

    for (int i = 0; i < count; ++i) {
        vectors.insert(insertPos + i, templateRow);
    }
    modified = true;
    return insertPos;
}

void SimDocument::duplicateVector(int index) {
    if (index < 0 || index >= vectors.size()) return;
    VectorRow copy = vectors[index];
    copy.messagesBefore.clear();
    copy.errors.clear();
    vectors.insert(index + 1, copy);
    modified = true;
}

void SimDocument::deleteVectors(int startVec, int endVec) {
    if (vectors.isEmpty()) return;
    int v0 = qBound(0, qMin(startVec, endVec), vectors.size() - 1);
    int v1 = qBound(0, qMax(startVec, endVec), vectors.size() - 1);
    vectors.remove(v0, v1 - v0 + 1);
    modified = true;
}

bool SimDocument::addSignal(const SignalItem &item, int insertIndex, QChar initialState, QString *errorOut) {
    if (item.name.trimmed().isEmpty() || item.members.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Signal name and members cannot be empty.");
        return false;
    }

    // Check for duplicate pin names in ORDER:
    QSet<QString> existingPins;
    for (const SignalItem &s : signalsList) {
        if (s.name.compare(item.name, Qt::CaseInsensitive) == 0) {
            if (errorOut) *errorOut = QStringLiteral("Duplicate signal name '%1' in ORDER:.").arg(item.name);
            return false;
        }
        for (const QString &m : s.members) {
            existingPins.insert(m.toLower());
        }
    }
    for (const QString &m : item.members) {
        if (existingPins.contains(m.toLower())) {
            if (errorOut) {
                *errorOut = QStringLiteral("Pin '%1' is already present in ORDER:.").arg(m);
            }
            return false;
        }
    }

    if (item.kind == SignalItem::Kind::FieldBus && !fields.contains(item.name.toLower())) {
        FieldDef fd;
        fd.name = item.name;
        fd.members = item.members;
        fd.definedInSi = true;
        fields.insert(item.name.toLower(), fd);
    }

    int pos = (insertIndex < 0 || insertIndex > signalsList.size()) ? signalsList.size() : insertIndex;
    signalsList.insert(pos, item);

    for (VectorRow &vr : vectors) {
        for (const QString &m : item.members) {
            QChar st = initialState != QLatin1Char('\0')
                           ? initialState
                           : (isPinOutput(m) ? QLatin1Char('*') : QLatin1Char('0'));
            vr.pinStates.insert(m.toLower(), st);
        }
    }

    modified = true;
    return true;
}

bool SimDocument::renameSignal(int sigIdx, const QString &newName, bool newActiveLow, QString *errorOut) {
    if (sigIdx < 0 || sigIdx >= signalsList.size()) return false;
    QString trimmed = newName.trimmed();
    if (trimmed.startsWith(QLatin1Char('!'))) {
        newActiveLow = true;
        trimmed = trimmed.mid(1).trimmed();
    }
    if (trimmed.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Signal name cannot be empty.");
        return false;
    }

    for (int i = 0; i < signalsList.size(); ++i) {
        if (i != sigIdx && signalsList[i].name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            if (errorOut) *errorOut = QStringLiteral("Signal '%1' already exists.").arg(trimmed);
            return false;
        }
    }

    SignalItem &sig = signalsList[sigIdx];
    const QString oldName = sig.name;

    if (sig.kind == SignalItem::Kind::Scalar) {
        const QString oldKey = oldName.toLower();
        const QString newKey = trimmed.toLower();
        sig.name = trimmed;
        sig.activeLow = newActiveLow;
        sig.members = {trimmed};
        if (oldKey != newKey) {
            for (VectorRow &vr : vectors) {
                QChar st = vr.pinStates.take(oldKey);
                vr.pinStates.insert(newKey, st.isNull() ? QLatin1Char('0') : st);
            }
            if (pinMeta.contains(oldKey) && !pinMeta.contains(newKey)) {
                PinMeta pm = pinMeta.take(oldKey);
                pm.name = trimmed;
                pinMeta.insert(newKey, pm);
            }
        }
    } else if (sig.kind == SignalItem::Kind::RangeBus && trimmed.contains(QStringLiteral(".."))) {
        QStringList newMembers;
        bool rngActLow = false;
        QString cleanRng;
        if (!expandRangeToken(trimmed, newMembers, &rngActLow, &cleanRng) ||
            newMembers.size() != sig.members.size()) {
            if (errorOut) {
                *errorOut = QStringLiteral("New range must have the same bit width (%1 bits).").arg(sig.width());
            }
            return false;
        }
        for (VectorRow &vr : vectors) {
            QVector<QChar> saved;
            for (const QString &m : sig.members) saved.append(vr.pinStates.take(m.toLower()));
            for (int b = 0; b < newMembers.size(); ++b) {
                vr.pinStates.insert(newMembers[b].toLower(), saved[b]);
            }
        }
        sig.name = cleanRng;
        sig.activeLow = newActiveLow || rngActLow;
        sig.members = newMembers;
    } else {
        // Rename FieldBus (or convert RangeBus to named FieldBus keeping same member pins)
        const QString oldKey = oldName.toLower();
        sig.kind = SignalItem::Kind::FieldBus;
        sig.name = trimmed;
        sig.activeLow = newActiveLow;
        FieldDef fd;
        fd.name = trimmed;
        fd.members = sig.members;
        fd.definedInSi = true;
        fields.remove(oldKey);
        fields.insert(trimmed.toLower(), fd);
    }

    modified = true;
    return true;
}

void SimDocument::deleteSignal(int sigIdx) {
    if (sigIdx < 0 || sigIdx >= signalsList.size()) return;
    const SignalItem removed = signalsList.takeAt(sigIdx);
    for (VectorRow &vr : vectors) {
        for (const QString &m : removed.members) {
            vr.pinStates.remove(m.toLower());
        }
    }
    modified = true;
}

void SimDocument::moveSignal(int fromIdx, int toIdx) {
    if (fromIdx < 0 || fromIdx >= signalsList.size()) return;
    if (toIdx < 0 || toIdx >= signalsList.size() || fromIdx == toIdx) return;
    signalsList.move(fromIdx, toIdx);
    modified = true;
}
