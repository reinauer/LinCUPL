#include "error_catalog.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

ErrorCatalog::ErrorCatalog() {
    addBuiltinToolEntries();
}

QString ErrorCatalog::moduleDisplayName(const QString &modCode) {
    const QString m = modCode.toLower();
    if (m == QStringLiteral("ck")) return QStringLiteral("CUPL Kernel (ck)");
    if (m == QStringLiteral("cx")) return QStringLiteral("CUPLX Preprocessor (cx)");
    if (m == QStringLiteral("ca")) return QStringLiteral("CUPLA Parser (ca)");
    if (m == QStringLiteral("cb")) return QStringLiteral("CUPLB Expander (cb)");
    if (m == QStringLiteral("cm")) return QStringLiteral("CUPLM Minimizer (cm)");
    if (m == QStringLiteral("cc")) return QStringLiteral("CUPLC Fuse/Output (cc)");
    if (m == QStringLiteral("sk")) return QStringLiteral("CSIM Kernel (sk)");
    if (m == QStringLiteral("sa")) return QStringLiteral("CSIMA Simulator (sa)");
    if (m == QStringLiteral("bp")) return QStringLiteral("BACKPIN Utility (bp)");
    if (m == QStringLiteral("ba")) return QStringLiteral("CBLD Library Manager (ba)");
    return modCode;
}

void ErrorCatalog::addBuiltinToolEntries() {
    auto addExtra = [this](const QString &code, const QString &mod, const QString &sev,
                           const QString &summary, const QString &expl) {
        if (m_byCode.contains(code.toLower())) {
            return;
        }
        CuplErrorEntry e;
        e.code = code.toLower();
        e.moduleCode = mod.toLower();
        e.moduleName = moduleDisplayName(mod);
        e.severity = sev;
        e.summary = summary;
        e.explanation = expl;
        m_byCode.insert(e.code, m_entries.size());
        m_entries.append(e);
    };

    addExtra(QStringLiteral("0001bp"), QStringLiteral("bp"), QStringLiteral("Fatal"),
             QStringLiteral("could not open: \"filename\""),
             QStringLiteral("Fatal. BACKPIN could not open the specified .PLD, .PLC, or .PPN file. Ensure the fitter placement file exists in the same directory as the source .PLD."));
    addExtra(QStringLiteral("0006bp"), QStringLiteral("bp"), QStringLiteral("Fatal"),
             QStringLiteral("unexpected symbol"),
             QStringLiteral("Fatal. BACKPIN encountered unexpected syntax while scanning PIN/PINNODE declarations or fitter placement output."));
    addExtra(QStringLiteral("0010bp"), QStringLiteral("bp"), QStringLiteral("Warning"),
             QStringLiteral("existing pin number has been changed"),
             QStringLiteral("Warning. BACKPIN updated a previously assigned pin number in the .PLD source to match the fitter placement."));
    addExtra(QStringLiteral("0016bp"), QStringLiteral("bp"), QStringLiteral("Fatal"),
             QStringLiteral("device not in library: \"device\""),
             QStringLiteral("Fatal. The target device mnemonic was not found in the selected .dl device library."));
}

bool ErrorCatalog::loadCatalog(const QString &filePath, QString *errorOut) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Could not open CuplErr.txt: %1").arg(filePath);
        }
        return false;
    }

    const QString text = normalizeConsoleText(QString::fromLatin1(file.readAll()));
    const QStringList lines = text.split(QLatin1Char('\n'));

    static const QRegularExpression headerRe(
        QStringLiteral("^((?:\\d{4}|10xx)[a-zA-Z]{2})\\s+(.*)$"));

    m_entries.clear();
    m_byCode.clear();
    m_catalogPath = QFileInfo(filePath).canonicalFilePath();
    if (m_catalogPath.isEmpty()) {
        m_catalogPath = filePath;
    }

    CuplErrorEntry current;
    QStringList bodyLines;

    auto flushCurrent = [&]() {
        if (!current.code.isEmpty()) {
            while (!bodyLines.isEmpty() && bodyLines.last().trimmed().isEmpty()) {
                bodyLines.removeLast();
            }
            const QString fullBody = bodyLines.join(QLatin1Char('\n')).trimmed();
            current.explanation = fullBody;
            if (fullBody.startsWith(QStringLiteral("Fatal."), Qt::CaseInsensitive)) {
                current.severity = QStringLiteral("Fatal");
            } else if (fullBody.startsWith(QStringLiteral("Warning."), Qt::CaseInsensitive)) {
                current.severity = QStringLiteral("Warning");
            } else {
                current.severity = QStringLiteral("Error");
            }
            m_byCode.insert(current.code.toLower(), m_entries.size());
            m_entries.append(current);
        }
        current = CuplErrorEntry{};
        bodyLines.clear();
    };

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch m = headerRe.match(line);
        if (m.hasMatch()) {
            flushCurrent();
            current.code = m.captured(1).toLower();
            current.moduleCode = current.code.right(2);
            current.moduleName = moduleDisplayName(current.moduleCode);
            current.summary = m.captured(2).trimmed();
        } else if (!current.code.isEmpty()) {
            bodyLines.append(rawLine.trimmed());
        }
    }
    flushCurrent();
    addBuiltinToolEntries();
    return !m_entries.isEmpty();
}

QStringList ErrorCatalog::modules() const {
    QStringList list = {QStringLiteral("All Modules")};
    for (const CuplErrorEntry &e : m_entries) {
        if (!list.contains(e.moduleName)) {
            list.append(e.moduleName);
        }
    }
    return list;
}

CuplErrorEntry ErrorCatalog::lookup(const QString &code) const {
    QString key = code.trimmed().toLower();
    if (key.startsWith(QLatin1Char('[')) && key.endsWith(QLatin1Char(']'))) {
        key = key.mid(1, key.size() - 2);
    }

    // Kernel shorthand [0007w] / [0001f] -> 0007ck / 0001ck
    static const QRegularExpression shortKernelRe(QStringLiteral("^(\\d{4})[fw]$"));
    const QRegularExpressionMatch km = shortKernelRe.match(key);
    if (km.hasMatch()) {
        key = km.captured(1) + QStringLiteral("ck");
    }

    int idx = m_byCode.value(key, -1);
    if (idx >= 0 && idx < m_entries.size()) {
        return m_entries.at(idx);
    }

    // Fallback for 10xx<mod> internal/program error family
    if (key.size() == 6 && key.startsWith(QStringLiteral("10"))) {
        const QString wildcard = QStringLiteral("10xx") + key.right(2);
        idx = m_byCode.value(wildcard, -1);
        if (idx >= 0 && idx < m_entries.size()) {
            return m_entries.at(idx);
        }
    }

    return CuplErrorEntry{};
}

QList<CuplErrorEntry> ErrorCatalog::search(const QString &moduleFilter,
                                           const QString &severityFilter,
                                           const QString &query) const {
    const QString q = query.trimmed().toLower();
    const bool allMods = moduleFilter.isEmpty() ||
                         moduleFilter.compare(QStringLiteral("All Modules"), Qt::CaseInsensitive) == 0;
    const bool allSev = severityFilter.isEmpty() ||
                        severityFilter.compare(QStringLiteral("All Severities"), Qt::CaseInsensitive) == 0;

    QList<CuplErrorEntry> results;
    results.reserve(m_entries.size());

    for (const CuplErrorEntry &e : m_entries) {
        if (!allMods &&
            e.moduleName.compare(moduleFilter, Qt::CaseInsensitive) != 0 &&
            e.moduleCode.compare(moduleFilter, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!allSev && e.severity.compare(severityFilter, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!q.isEmpty()) {
            const bool hit = e.code.contains(q) ||
                             e.summary.toLower().contains(q) ||
                             e.explanation.toLower().contains(q) ||
                             e.moduleName.toLower().contains(q);
            if (!hit) {
                continue;
            }
        }
        results.append(e);
    }
    return results;
}

QString ErrorCatalog::normalizeConsoleText(const QString &raw) {
    QString out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c == QLatin1Char('\r')) {
            if (i + 1 < raw.size() && raw.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            out.append(QLatin1Char('\n'));
        } else if (c == QLatin1Char('\n')) {
            if (i + 1 < raw.size() && raw.at(i + 1) == QLatin1Char('\r')) {
                ++i;
            }
            out.append(QLatin1Char('\n'));
        } else {
            out.append(c);
        }
    }
    return out;
}

int ErrorCatalog::inferLineFromPldText(const QString &pldText,
                                       const QString &code,
                                       const QString &message) {
    if (pldText.isEmpty()) {
        return 0;
    }

    const QStringList lines = normalizeConsoleText(pldText).split(QLatin1Char('\n'));

    // Try matching "pin/node <num>" first (e.g. [0012cb] pin/node 99 invalid usage: b)
    static const QRegularExpression pinNumRe(
        QStringLiteral("pin/node\\s+\"?(\\d+)\"?"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch pm = pinNumRe.match(message);
    if (pm.hasMatch()) {
        const QString num = pm.captured(1);
        const QRegularExpression declRe(
            QStringLiteral("^\\s*(?:pin|pinnode|node)\\b[^;]*\\b%1\\b")
                .arg(QRegularExpression::escape(num)),
            QRegularExpression::CaseInsensitiveOption);
        for (int i = 0; i < lines.size(); ++i) {
            if (declRe.match(lines.at(i)).hasMatch()) {
                return i + 1;
            }
        }
    }

    // Try trailing identifier after ':' (e.g. "excessive number of product terms: carry")
    static const QRegularExpression trailingSymRe(
        QStringLiteral(":\\s*\"?([A-Za-z_$][A-Za-z0-9_$]*)\"?\\s*$"));
    const QRegularExpressionMatch sm = trailingSymRe.match(message.trimmed());
    if (sm.hasMatch()) {
        const QString sym = sm.captured(1);
        const QRegularExpression symRe(
            QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(sym)),
            QRegularExpression::CaseInsensitiveOption);
        bool inComment = false;
        for (int i = 0; i < lines.size(); ++i) {
            QString clean = lines.at(i);
            if (inComment) {
                const int endIdx = clean.indexOf(QStringLiteral("*/"));
                if (endIdx < 0) {
                    continue;
                }
                clean = clean.mid(endIdx + 2);
                inComment = false;
            }
            while (true) {
                const int startIdx = clean.indexOf(QStringLiteral("/*"));
                if (startIdx < 0) {
                    break;
                }
                const int endIdx = clean.indexOf(QStringLiteral("*/"), startIdx + 2);
                if (endIdx < 0) {
                    clean = clean.left(startIdx);
                    inComment = true;
                    break;
                }
                clean.remove(startIdx, endIdx - startIdx + 2);
            }
            if (symRe.match(clean).hasMatch()) {
                return i + 1;
            }
        }
    }

    if (code.endsWith(QStringLiteral("ca")) || code.endsWith(QStringLiteral("ck"))) {
        if (message.contains(QStringLiteral("device"), Qt::CaseInsensitive)) {
            static const QRegularExpression devLineRe(
                QStringLiteral("^\\s*device\\b"), QRegularExpression::CaseInsensitiveOption);
            for (int i = 0; i < lines.size(); ++i) {
                if (devLineRe.match(lines.at(i)).hasMatch()) {
                    return i + 1;
                }
            }
        }
    }

    return 0;
}

int ErrorCatalog::findVectorLineInSiOrSo(const QString &filePath, int vectorNum, bool isSoFile) {
    if (filePath.isEmpty() || vectorNum <= 0) {
        return 0;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QStringList lines = normalizeConsoleText(QString::fromLatin1(f.readAll())).split(QLatin1Char('\n'));
    if (isSoFile) {
        const QString prefix = QStringLiteral("%1:").arg(vectorNum, 4, 10, QLatin1Char('0'));
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).trimmed().startsWith(prefix)) {
                return i + 1;
            }
        }
        return 0;
    }

    // Locate VECTORS: section in .SI and count non-empty, non-comment vector rows
    bool inVectors = false;
    bool inComment = false;
    int seenVectors = 0;
    static const QRegularExpression vecKwRe(
        QStringLiteral("\\bvectors\\s*:"), QRegularExpression::CaseInsensitiveOption);

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (inComment) {
            const int endC = line.indexOf(QStringLiteral("*/"));
            if (endC < 0) continue;
            line = line.mid(endC + 2);
            inComment = false;
        }
        while (true) {
            const int begC = line.indexOf(QStringLiteral("/*"));
            if (begC < 0) break;
            const int endC = line.indexOf(QStringLiteral("*/"), begC + 2);
            if (endC < 0) {
                line = line.left(begC);
                inComment = true;
                break;
            }
            line.remove(begC, endC - begC + 2);
        }

        if (!inVectors) {
            const QRegularExpressionMatch vm = vecKwRe.match(line);
            if (vm.hasMatch()) {
                inVectors = true;
                line = line.mid(vm.capturedEnd()).trimmed();
                if (line.isEmpty()) {
                    continue;
                }
            } else {
                continue;
            }
        }

        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('$')) || trimmed == QStringLiteral(";")) {
            continue;
        }
        ++seenVectors;
        if (seenVectors == vectorNum) {
            return i + 1;
        }
    }
    return 0;
}

QList<DiagnosticItem> ErrorCatalog::parseToolOutput(const QString &toolOutput,
                                                    const QString &primarySourcePath,
                                                    const QString &sourceText,
                                                    const QString &lstOrSoPath) const {
    QList<DiagnosticItem> items;
    const QString norm = normalizeConsoleText(toolOutput);
    const QStringList lines = norm.split(QLatin1Char('\n'));

    // e.g. "  4:Pin 99 = b;" (compiler line echo) vs "0001: C100  LLLL L" (simulator vector)
    static const QRegularExpression simVecRe(QStringLiteral("^(\\d{4}):\\s+(.*)$"));
    static const QRegularExpression srcEchoRe(QStringLiteral("^\\s*(\\d+):(.*)$"));
    static const QRegularExpression caretRe(QStringLiteral("^(\\s*)\\^"));
    static const QRegularExpression diagRe(
        QStringLiteral("^\\s*\\[((?:\\d{4}|10xx)[a-zA-Z]{1,2})\\]\\s*(.*)$"));
    static const QRegularExpression inlineLineRe(
        QStringLiteral("\\bline\\s*\\(\\s*(\\d+)\\s*\\),?\\s*"),
        QRegularExpression::CaseInsensitiveOption);

    int lastSrcLine = 0;
    int lastCaretCol = 0;
    int lastPrefixWidth = 0;
    int lastVectorNum = 0;

    for (const QString &line : lines) {
        const QRegularExpressionMatch vm = simVecRe.match(line);
        if (vm.hasMatch()) {
            lastVectorNum = vm.captured(1).toInt();
            lastSrcLine = 0;
            lastCaretCol = 0;
            continue;
        }

        const QRegularExpressionMatch em = srcEchoRe.match(line);
        if (em.hasMatch()) {
            lastSrcLine = em.captured(1).toInt();
            const int colonPos = line.indexOf(QLatin1Char(':'));
            lastPrefixWidth = (colonPos >= 0) ? (colonPos + 1) : 0;
            lastCaretCol = 0;
            lastVectorNum = 0;
            continue;
        }

        const QRegularExpressionMatch cm = caretRe.match(line);
        if (cm.hasMatch()) {
            const int rawCaret = cm.captured(1).size() + 1;
            lastCaretCol = qMax(1, rawCaret - lastPrefixWidth);
            continue;
        }

        const QRegularExpressionMatch dm = diagRe.match(line);
        if (dm.hasMatch()) {
            DiagnosticItem diag;
            diag.code = dm.captured(1).toLower();
            QString msg = dm.captured(2).trimmed();

            const QRegularExpressionMatch ilm = inlineLineRe.match(msg);
            if (ilm.hasMatch()) {
                diag.line = ilm.captured(1).toInt();
                msg.remove(ilm.capturedStart(), ilm.capturedLength());
                msg = msg.trimmed();
            } else if (lastSrcLine > 0) {
                diag.line = lastSrcLine;
                diag.column = lastCaretCol;
            }

            diag.vectorNumber = lastVectorNum;
            diag.message = msg;

            const CuplErrorEntry entry = lookup(diag.code);
            diag.lookupCode = entry.isValid() ? entry.code : diag.code;

            if (msg.startsWith(QStringLiteral("Warning"), Qt::CaseInsensitive) ||
                msg.startsWith(QStringLiteral("Please note"), Qt::CaseInsensitive) ||
                diag.code.endsWith(QLatin1Char('w'))) {
                diag.severity = QStringLiteral("Warning");
            } else if (entry.isValid()) {
                diag.severity = entry.severity;
            } else if (diag.code.endsWith(QLatin1Char('f'))) {
                diag.severity = QStringLiteral("Fatal");
            } else {
                diag.severity = QStringLiteral("Error");
            }

            diag.filePath = primarySourcePath;
            if (diag.line <= 0 && diag.vectorNumber > 0) {
                // Jump to vector line in .SI if primarySourcePath is .SI, or .SO if available
                const bool primaryIsSi = primarySourcePath.endsWith(QStringLiteral(".si"), Qt::CaseInsensitive);
                if (primaryIsSi) {
                    diag.line = findVectorLineInSiOrSo(primarySourcePath, diag.vectorNumber, false);
                }
                if (diag.line <= 0 && !lstOrSoPath.isEmpty()) {
                    const int soLine = findVectorLineInSiOrSo(lstOrSoPath, diag.vectorNumber, true);
                    if (soLine > 0) {
                        diag.filePath = lstOrSoPath;
                        diag.line = soLine;
                    }
                }
            } else if (diag.line <= 0 && !sourceText.isEmpty()) {
                diag.line = inferLineFromPldText(sourceText, diag.lookupCode, diag.message);
            }

            items.append(diag);
        }
    }

    return items;
}
