#pragma once

// The help window of LinCUPL and LinSim.
//
// The pages come from doc/help and are found in share/lincupl/help next to
// the program (build tree or installation).  Built with Qt Help
// (LINCUPL_HAS_QTHELP), the window reads them from lincupl.qch through a
// QHelpEngine and offers contents, index and full-text search.  Without it
// the window shows the plain HTML pages and takes the contents and index
// from the help project lincupl.qhp.

#include <QList>
#include <QMainWindow>
#include <QMultiMap>
#include <QStringList>
#include <QTextBrowser>
#include <QUrl>

#include <functional>

class QLineEdit;
class QMenu;
class QTabWidget;
class QTreeWidget;
class QListWidget;
class QHelpEngine;

namespace Help {

// Where the help of a keyword is looked for first.
enum class Context { Design, Simulator };

// Add Contents (F1), Index and Search to a Help menu and turn on F1 in
// the editors (installContextHelp).  In the simulator context Contents
// opens the simulator section.
void addHelpMenuActions(QMenu *menu, Context context);

// Open the help window on its start page, the contents, index or search
// tab, or on one page ("sim-language.html#vectors").
void showContents();
void showIndex();
void showSearch();
void showPage(const QString &page);

// Open the topic of an index keyword; false if there is none.  For a
// keyword described both for designs and for .si files the page of the
// given context is preferred.
bool showKeyword(const QString &keyword, Context context = Context::Design);

// The index keyword for the word at `column` of `line`: a CUPL or
// simulator keyword in upper case ("FIELD", "$DEFINE"), a pin extension in
// lower case with its dot (".oe"), or a bit field operator (":&").  Empty
// if the cursor is not on a word.
QString keywordAt(const QString &line, int column);

// F1 in a CUPL or .si editor (a QPlainTextEdit with a CuplHighlighter)
// opens the topic of the keyword under the cursor; elsewhere, or if the
// word has no topic, the F1 shortcut of the Help menu applies.
void installContextHelp();

// Links of the form lincupl:<name> in the pages ask the program to do
// something, e.g. lincupl:error-messages opens the error message viewer.
// A program that offers such an action registers a handler for it.
void setActionHandler(const QString &name, std::function<void()> handler);

// The directory with the help pages, or empty if none was found.
QString helpDirectory();

// Where the Qt Help collection and its search index are kept (default:
// the user's cache directory).  The self-test points it at a scratch
// directory before the window is first used.
void setCacheDirectory(const QString &dir);

} // namespace Help

class HelpBrowser : public QTextBrowser {
    Q_OBJECT

public:
    explicit HelpBrowser(QHelpEngine *engine, QWidget *parent = nullptr);

    QVariant loadResource(int type, const QUrl &name) override;

private:
    QHelpEngine *m_engine;
};

class HelpWindow : public QMainWindow {
    Q_OBJECT

public:
    // The one help window of the program, made on first use.
    static HelpWindow *instance();

    bool usesQtHelp() const;
    bool isAvailable() const { return m_available; }
    QString problem() const { return m_problem; }

    QUrl pageUrl(const QString &page) const;
    QUrl homeUrl() const { return pageUrl(QStringLiteral("index.html")); }
    void openUrl(const QUrl &url);
    QUrl currentUrl() const;
    HelpBrowser *browser() const { return m_browser; }

    // Pages of an index keyword (exact, as listed in the index).
    QList<QUrl> topicsForKeyword(const QString &keyword) const;
    QUrl topicFor(const QString &keyword, Help::Context context) const;

    // Entries of the table of contents (all levels), once it is loaded,
    // and the pages they open.
    int contentsEntryCount();
    QList<QUrl> contentsUrls();
    int indexEntryCount();
    // Full-text search; the number of hits, or -1 without Qt Help or on
    // time-out.  Waits for the search index to be built first.
    int searchBlocking(const QString &query, int timeoutMs = 30000);

    void showContentsTab();
    void showIndexTab();
    void showSearchTab();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    explicit HelpWindow(QWidget *parent = nullptr);
    void buildUi();
    void setupQtHelp();
    void setupPlainPages();
    void onLinkClicked(const QUrl &url);
    void showFindBar();
    void findInPage(bool backward);
    void chooseTopic(const QList<QUrl> &urls, const QStringList &titles);

    QHelpEngine *m_engine = nullptr;
    // The full-text index is being built (from setupQtHelp until the
    // search engine reports indexingFinished)
    bool m_indexing = false;
    bool m_available = false;
    QString m_problem;
    QString m_dir;
    QString m_namespace;

    HelpBrowser *m_browser = nullptr;
    QTabWidget *m_sideTabs = nullptr;
    QWidget *m_findBar = nullptr;
    QLineEdit *m_findEdit = nullptr;

    // Plain pages: contents and index read from lincupl.qhp
    QTreeWidget *m_plainContents = nullptr;
    QLineEdit *m_plainIndexFilter = nullptr;
    QListWidget *m_plainIndex = nullptr;
    QMultiMap<QString, QString> m_plainKeywords; // keyword -> page
};
