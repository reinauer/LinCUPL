#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

struct CuplErrorEntry {
    QString code;        // e.g. "0007ca", "0019sa", "10xxck"
    QString moduleCode;  // e.g. "ck", "cx", "ca", "cb", "cm", "cc", "sk", "sa", "bp", "ba"
    QString moduleName;  // Human-readable module name
    QString severity;    // "Fatal", "Error", "Warning"
    QString summary;     // Headline template from CuplErr.txt
    QString explanation; // Full multi-line explanation text

    bool isValid() const { return !code.isEmpty(); }
};

struct DiagnosticItem {
    QString code;        // e.g. "0009ca"
    QString lookupCode;  // Normalized CuplErr.txt code (e.g. "0007ck" for "0007w")
    QString severity;    // "Fatal", "Error", "Warning"
    QString message;     // Compiler/simulator diagnostic message
    QString filePath;    // Target .pld, .si, .lst, or .so file
    int line = 0;        // 1-based line number (0 if unknown)
    int column = 0;      // 1-based column (0 if unknown)
    int vectorNumber = 0;// 1-based simulation vector number (for csim errors)
};

class ErrorCatalog {
public:
    ErrorCatalog();

    bool loadCatalog(const QString &filePath, QString *errorOut = nullptr);
    bool isLoaded() const { return !m_entries.isEmpty(); }
    QString catalogPath() const { return m_catalogPath; }

    const QList<CuplErrorEntry> &entries() const { return m_entries; }
    QStringList modules() const;

    CuplErrorEntry lookup(const QString &code) const;
    QList<CuplErrorEntry> search(const QString &moduleFilter,
                                 const QString &severityFilter,
                                 const QString &query) const;

    QList<DiagnosticItem> parseToolOutput(const QString &toolOutput,
                                          const QString &primarySourcePath,
                                          const QString &sourceText = QString(),
                                          const QString &lstOrSoPath = QString()) const;

    static QString normalizeConsoleText(const QString &raw);
    static QString moduleDisplayName(const QString &modCode);

private:
    void addBuiltinToolEntries();
    static int inferLineFromPldText(const QString &pldText,
                                    const QString &code,
                                    const QString &message);
    static int findVectorLineInSiOrSo(const QString &filePath, int vectorNum, bool isSoFile);

    QString m_catalogPath;
    QList<CuplErrorEntry> m_entries;
    QHash<QString, int> m_byCode;
};
