#include "help_window.h"

#include "cupl_highlighter.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QXmlStreamReader>

#ifdef LINCUPL_HAS_QTHELP
#include <QHelpContentWidget>
#include <QHelpEngine>
#include <QHelpIndexWidget>
#include <QHelpLink>
#include <QHelpSearchEngine>
#include <QHelpSearchQueryWidget>
#include <QHelpSearchResultWidget>
#endif

namespace {

QString g_cacheDir;
QHash<QString, std::function<void()>> g_actions;

bool isHelpDir(const QString &dir) {
    const QDir d(dir);
#ifdef LINCUPL_HAS_QTHELP
    if (QFileInfo::exists(d.filePath(QStringLiteral("lincupl.qch")))) {
        return true;
    }
#endif
    return QFileInfo::exists(d.filePath(QStringLiteral("lincupl.qhp")))
           && QFileInfo::exists(d.filePath(QStringLiteral("index.html")));
}

#ifdef LINCUPL_HAS_QTHELP
QString canonical(const QString &path) {
    const QString c = QFileInfo(path).canonicalFilePath();
    return c.isEmpty() ? QFileInfo(path).absoluteFilePath() : c;
}
#endif

// The page part of a help URL ("sim-language.html"), for choosing between
// the design and the simulator description of a keyword.
bool isSimulatorPage(const QUrl &url) {
    return QFileInfo(url.path()).fileName().startsWith(QStringLiteral("sim"));
}

} // namespace

// ---------------------------------------------------------------------------
// Help namespace
// ---------------------------------------------------------------------------

namespace Help {

QString helpDirectory() {
    QStringList candidates;
    const QString envShare = qEnvironmentVariable("LINCUPL_SHARE");
    if (!envShare.isEmpty()) {
        candidates << QDir(envShare).filePath(QStringLiteral("help"));
    }
    if (QCoreApplication::instance()) {
        const QDir app(QCoreApplication::applicationDirPath());
        candidates << app.filePath(QStringLiteral("../share/lincupl/help"))
                   << app.filePath(QStringLiteral("../../share/lincupl/help"));
    }
#ifdef LINCUPL_DATADIR
    candidates << QStringLiteral(LINCUPL_DATADIR "/help");
#endif
    for (const QString &dir : candidates) {
        if (isHelpDir(dir)) {
            return QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
        }
    }
    return QString();
}

void setCacheDirectory(const QString &dir) {
    g_cacheDir = dir;
}

void setActionHandler(const QString &name, std::function<void()> handler) {
    g_actions.insert(name, std::move(handler));
}

void showContents() {
    HelpWindow *w = HelpWindow::instance();
    w->showContentsTab();
    w->openUrl(w->homeUrl());
}

void addHelpMenuActions(QMenu *menu, Context context) {
    QAction *contents = menu->addAction(QStringLiteral("&Contents"), menu, [context]() {
        if (context == Context::Simulator) {
            showPage(QStringLiteral("sim-overview.html"));
        } else {
            showContents();
        }
    });
    // F1 everywhere; also the platform's help key where that differs
    QList<QKeySequence> keys{QKeySequence(Qt::Key_F1)};
    for (const QKeySequence &k : QKeySequence::keyBindings(QKeySequence::HelpContents)) {
        if (!keys.contains(k)) {
            keys << k;
        }
    }
    contents->setShortcuts(keys);
    // One menu per main window (LinSim opens a window per document): a
    // window shortcut keeps F1 unambiguous when several are open.
    contents->setShortcutContext(Qt::WindowShortcut);
    menu->addAction(QStringLiteral("&Index"), menu, []() { showIndex(); });
    menu->addAction(QStringLiteral("&Search"), menu, []() { showSearch(); });
    installContextHelp();
}

void showIndex() {
    HelpWindow::instance()->showIndexTab();
}

void showSearch() {
    HelpWindow::instance()->showSearchTab();
}

void showPage(const QString &page) {
    HelpWindow *w = HelpWindow::instance();
    w->showContentsTab();
    w->openUrl(w->pageUrl(page));
}

bool showKeyword(const QString &keyword, Context context) {
    HelpWindow *w = HelpWindow::instance();
    const QUrl url = w->topicFor(keyword, context);
    if (!url.isValid()) {
        return false;
    }
    w->showIndexTab();
    w->openUrl(url);
    return true;
}

QString keywordAt(const QString &line, int column) {
    auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    const int n = int(line.size());
    if (n == 0) {
        return QString();
    }
    int pos = qBound(0, column, n);
    if (pos == n || !isWord(line.at(pos))) {
        // Just behind a word, or on an operator
        if (pos > 0 && isWord(line.at(pos - 1))) {
            --pos;
        } else {
            // Bit field operators :& :# :$ and the ':' of a field equality
            auto isOp = [&line, n](int i) {
                return i >= 0 && i < n && (line.at(i) == QLatin1Char('&') || line.at(i) == QLatin1Char('#')
                                           || line.at(i) == QLatin1Char('$'));
            };
            int colon = -1;
            if (pos < n && line.at(pos) == QLatin1Char(':')) {
                colon = pos;
            } else if (pos > 0 && line.at(pos - 1) == QLatin1Char(':')) {
                colon = pos - 1;
            } else if (pos > 1 && line.at(pos - 2) == QLatin1Char(':') && isOp(pos - 1)) {
                colon = pos - 2;
            }
            if (colon >= 0 && colon + 1 < n) {
                const QChar op = line.at(colon + 1);
                if (op == QLatin1Char('&') || op == QLatin1Char('#') || op == QLatin1Char('$')) {
                    return QStringLiteral(":") + op;
                }
            }
            return QString();
        }
    }
    int start = pos;
    while (start > 0 && isWord(line.at(start - 1))) {
        --start;
    }
    int end = pos;
    while (end < n && isWord(line.at(end))) {
        ++end;
    }
    QString word = line.mid(start, end - start);
    if (word.isEmpty() || word.at(0).isDigit()) {
        return QString();
    }
    if (start > 0 && line.at(start - 1) == QLatin1Char('$')) {
        return QLatin1Char('$') + word.toUpper();
    }
    if (start > 1 && line.at(start - 1) == QLatin1Char('.')
        && (isWord(line.at(start - 2)) || line.at(start - 2) == QLatin1Char(']'))) {
        return QLatin1Char('.') + word.toLower();
    }
    return word.toUpper();
}

namespace {

// F1 on a keyword in a CUPL or .si editor
class ContextHelpFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) {
            return false;
        }
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() != Qt::Key_F1 || key->modifiers() != Qt::NoModifier) {
            return false;
        }
        auto *edit = qobject_cast<QPlainTextEdit *>(watched);
        if (edit == nullptr) {
            return false;
        }
        const CuplHighlighter *hl = nullptr;
        for (QSyntaxHighlighter *h : edit->document()->findChildren<QSyntaxHighlighter *>()) {
            if ((hl = dynamic_cast<const CuplHighlighter *>(h)) != nullptr) {
                break;
            }
        }
        if (hl == nullptr || hl->language() == CuplHighlighter::Language::None) {
            return false;
        }
        const bool si = hl->language() == CuplHighlighter::Language::Si
                        || hl->language() == CuplHighlighter::Language::SiListing;
        const Context context = si ? Context::Simulator : Context::Design;
        const QTextCursor cursor = edit->textCursor();
        const QString keyword = keywordAt(cursor.block().text(), cursor.positionInBlock());
        if (keyword.isEmpty() || !HelpWindow::instance()->topicFor(keyword, context).isValid()) {
            return false; // the Help > Contents shortcut takes F1
        }
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept(); // deliver F1 as a key press to the editor
            return true;
        }
        showKeyword(keyword, context);
        return true;
    }
};

} // namespace

void installContextHelp() {
    static QPointer<QObject> filter;
    if (filter == nullptr && qApp != nullptr) {
        filter = new ContextHelpFilter(qApp);
        qApp->installEventFilter(filter);
    }
}

} // namespace Help

// ---------------------------------------------------------------------------
// HelpBrowser
// ---------------------------------------------------------------------------

HelpBrowser::HelpBrowser(QHelpEngine *engine, QWidget *parent)
    : QTextBrowser(parent), m_engine(engine) {
    setOpenLinks(false);
    setFrameShape(QFrame::NoFrame);
}

QVariant HelpBrowser::loadResource(int type, const QUrl &name) {
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr && name.scheme() == QLatin1String("qthelp")) {
        const QByteArray data = m_engine->fileData(name);
        if (type == QTextDocument::HtmlResource || type == QTextDocument::StyleSheetResource) {
            return QString::fromUtf8(data);
        }
        return data;
    }
#endif
    return QTextBrowser::loadResource(type, name);
}

// ---------------------------------------------------------------------------
// HelpWindow
// ---------------------------------------------------------------------------

HelpWindow *HelpWindow::instance() {
    static QPointer<HelpWindow> window;
    if (window == nullptr) {
        window = new HelpWindow();
    }
    return window;
}

HelpWindow::HelpWindow(QWidget *parent)
    : QMainWindow(parent) {
    setObjectName(QStringLiteral("HelpWindow"));
    setWindowTitle(QStringLiteral("LinCUPL Help"));
    m_dir = Help::helpDirectory();
#ifdef LINCUPL_HAS_QTHELP
    if (!m_dir.isEmpty() && QFileInfo::exists(QDir(m_dir).filePath(QStringLiteral("lincupl.qch")))) {
        setupQtHelp();
    }
#endif
    buildUi();
    if (m_engine == nullptr) {
        setupPlainPages();
    }
    if (m_available) {
        // QTextBrowser does not apply the pages' <link rel="stylesheet">, so
        // the style sheet becomes the document's default one.
        const QVariant css = m_browser->loadResource(QTextDocument::StyleSheetResource,
                                                     pageUrl(QStringLiteral("style.css")));
        m_browser->document()->setDefaultStyleSheet(
            css.typeId() == QMetaType::QByteArray ? QString::fromUtf8(css.toByteArray()) : css.toString());
    }
    if (!m_available) {
        m_browser->setHtml(QStringLiteral(
            "<h2>Help not found</h2><p>%1</p><p>The help pages are looked for in "
            "<tt>share/lincupl/help</tt> next to the program, in <tt>$LINCUPL_SHARE/help</tt> "
            "and in the installed data directory.</p>").arg(m_problem.toHtmlEscaped()));
    }

    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("help/geometry")).toByteArray())) {
        resize(1000, 720);
    }
}

void HelpWindow::closeEvent(QCloseEvent *event) {
    QSettings settings;
    settings.setValue(QStringLiteral("help/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}

bool HelpWindow::usesQtHelp() const {
    return m_engine != nullptr;
}

void HelpWindow::setupQtHelp() {
#ifdef LINCUPL_HAS_QTHELP
    const QDir dir(m_dir);
    const QString qch = dir.filePath(QStringLiteral("lincupl.qch"));

    // The engine writes to its collection (registration, search index), so
    // it keeps its own in the cache directory and registers the installed
    // lincupl.qch there; the installed files may be read-only.  (The
    // lincupl.qhc installed next to the pages is for Qt Assistant:
    // assistant -collectionFile lincupl.qhc.)
    QString cacheDir = g_cacheDir;
    if (cacheDir.isEmpty()) {
        cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    }
    cacheDir = QDir(cacheDir).filePath(QStringLiteral("help"));
    QDir().mkpath(cacheDir);
    const QString collection = QDir(cacheDir).filePath(QStringLiteral("lincupl.qhc"));
    const QString searchIndex = QDir(cacheDir).filePath(QStringLiteral(".lincupl"));

    const QFileInfo collInfo(collection);
    if (collInfo.exists() && collInfo.lastModified() < QFileInfo(qch).lastModified()) {
        // The pages changed since they were registered: start over
        QFile::remove(collection);
        QDir(searchIndex).removeRecursively();
    }

    m_engine = new QHelpEngine(collection, this);
    m_engine->setReadOnly(false);
    if (!m_engine->setupData()) {
        m_problem = QStringLiteral("Cannot open the help collection %1: %2")
                        .arg(collection, m_engine->error());
        delete m_engine;
        m_engine = nullptr;
        return;
    }
    // Register the pages, or register them again if the collection names
    // another lincupl.qch (e.g. of a build tree that has moved).
    const QString ns = QHelpEngineCore::namespaceName(qch);
    m_namespace = ns;
    if (canonical(m_engine->documentationFileName(ns)) != canonical(qch)) {
        if (m_engine->registeredDocumentations().contains(ns)) {
            m_engine->unregisterDocumentation(ns);
        }
        QDir(searchIndex).removeRecursively();
        if (!m_engine->registerDocumentation(qch)) {
            m_problem = QStringLiteral("Cannot register %1: %2").arg(qch, m_engine->error());
            delete m_engine;
            m_engine = nullptr;
            return;
        }
    }
    m_available = true;
#endif
}

void HelpWindow::setupPlainPages() {
    if (m_dir.isEmpty()) {
        m_problem = QStringLiteral("The help pages were not found.");
        return;
    }
    QFile qhp(QDir(m_dir).filePath(QStringLiteral("lincupl.qhp")));
    if (!qhp.open(QIODevice::ReadOnly)) {
        m_problem = QStringLiteral("Cannot read %1.").arg(qhp.fileName());
        return;
    }

    // <section title=".." ref=".."> nest in <toc>; <keyword name=".." ref=".."/>
    QXmlStreamReader xml(&qhp);
    QList<QTreeWidgetItem *> open;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("section")) {
            const QString title = xml.attributes().value(QLatin1String("title")).toString();
            const QString ref = xml.attributes().value(QLatin1String("ref")).toString();
            auto *item = open.isEmpty() ? new QTreeWidgetItem(m_plainContents)
                                        : new QTreeWidgetItem(open.last());
            item->setText(0, title);
            item->setData(0, Qt::UserRole, ref);
            open.append(item);
        } else if (xml.isEndElement() && xml.name() == QLatin1String("section")) {
            if (!open.isEmpty()) {
                open.removeLast();
            }
        } else if (xml.isStartElement() && xml.name() == QLatin1String("keyword")) {
            m_plainKeywords.insert(xml.attributes().value(QLatin1String("name")).toString(),
                                   xml.attributes().value(QLatin1String("ref")).toString());
        }
    }
    if (xml.hasError()) {
        m_problem = QStringLiteral("%1: %2").arg(qhp.fileName(), xml.errorString());
        return;
    }
    if (m_plainContents->topLevelItemCount() > 0) {
        m_plainContents->topLevelItem(0)->setExpanded(true);
    }
    QStringList names = m_plainKeywords.uniqueKeys();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    m_plainIndex->addItems(names);
    m_available = true;
}

void HelpWindow::buildUi() {
    m_browser = new HelpBrowser(m_engine, this);
    m_browser->setSearchPaths({m_dir});
    connect(m_browser, &QTextBrowser::anchorClicked, this, &HelpWindow::onLinkClicked);

    // Navigation
    QToolBar *tb = addToolBar(QStringLiteral("Navigation"));
    tb->setObjectName(QStringLiteral("HelpNavigation"));
    tb->setMovable(false);
    QAction *back = tb->addAction(style()->standardIcon(QStyle::SP_ArrowBack), QStringLiteral("Back"),
                                  m_browser, &QTextBrowser::backward);
    back->setShortcut(QKeySequence::Back);
    back->setEnabled(false);
    QAction *forward = tb->addAction(style()->standardIcon(QStyle::SP_ArrowForward), QStringLiteral("Forward"),
                                     m_browser, &QTextBrowser::forward);
    forward->setShortcut(QKeySequence::Forward);
    forward->setEnabled(false);
    tb->addAction(style()->standardIcon(QStyle::SP_DirHomeIcon), QStringLiteral("Home"),
                  this, [this]() { openUrl(homeUrl()); });
    connect(m_browser, &QTextBrowser::backwardAvailable, back, &QAction::setEnabled);
    connect(m_browser, &QTextBrowser::forwardAvailable, forward, &QAction::setEnabled);
    tb->addSeparator();
    QAction *find = tb->addAction(QStringLiteral("Find in Page"), this, &HelpWindow::showFindBar);
    find->setShortcut(QKeySequence::Find);
    QAction *zoomIn = new QAction(this);
    zoomIn->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+="))});
    connect(zoomIn, &QAction::triggered, m_browser, [this]() { m_browser->zoomIn(1); });
    addAction(zoomIn);
    QAction *zoomOut = new QAction(this);
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, m_browser, [this]() { m_browser->zoomOut(1); });
    addAction(zoomOut);
    QAction *closeAct = new QAction(this);
    closeAct->setShortcut(QKeySequence::Close);
    connect(closeAct, &QAction::triggered, this, &QWidget::close);
    addAction(closeAct);

    // Find bar under the page
    m_findBar = new QWidget(this);
    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(6, 2, 6, 4);
    findLayout->addWidget(new QLabel(QStringLiteral("Find:"), m_findBar));
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setClearButtonEnabled(true);
    findLayout->addWidget(m_findEdit, 1);
    auto *prevBtn = new QPushButton(QStringLiteral("Previous"), m_findBar);
    auto *nextBtn = new QPushButton(QStringLiteral("Next"), m_findBar);
    auto *closeBtn = new QToolButton(m_findBar);
    closeBtn->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    closeBtn->setAutoRaise(true);
    findLayout->addWidget(prevBtn);
    findLayout->addWidget(nextBtn);
    findLayout->addWidget(closeBtn);
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this]() { findInPage(false); });
    connect(m_findEdit, &QLineEdit::textEdited, this, [this]() {
        // Search again from the top of the page as the text changes
        QTextCursor c = m_browser->textCursor();
        c.movePosition(QTextCursor::Start);
        m_browser->setTextCursor(c);
        findInPage(false);
    });
    connect(prevBtn, &QPushButton::clicked, this, [this]() { findInPage(true); });
    connect(nextBtn, &QPushButton::clicked, this, [this]() { findInPage(false); });
    connect(closeBtn, &QToolButton::clicked, m_findBar, &QWidget::hide);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, m_findBar, &QWidget::hide);
    QAction *findNext = new QAction(this);
    findNext->setShortcut(QKeySequence::FindNext);
    connect(findNext, &QAction::triggered, this, [this]() { findInPage(false); });
    addAction(findNext);
    QAction *findPrev = new QAction(this);
    findPrev->setShortcut(QKeySequence::FindPrevious);
    connect(findPrev, &QAction::triggered, this, [this]() { findInPage(true); });
    addAction(findPrev);
    m_findBar->hide();

    auto *pageArea = new QWidget(this);
    auto *pageLayout = new QVBoxLayout(pageArea);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(m_browser, 1);
    pageLayout->addWidget(m_findBar);

    // Contents / Index / Search beside the page
    m_sideTabs = new QTabWidget(this);
    m_sideTabs->setDocumentMode(true);
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr) {
        QHelpContentWidget *contents = m_engine->contentWidget();
        m_sideTabs->addTab(contents, QStringLiteral("Contents"));
        connect(contents, &QHelpContentWidget::linkActivated, this, &HelpWindow::openUrl);
        connect(m_browser, &QTextBrowser::sourceChanged, contents, [contents](const QUrl &url) {
            const QModelIndex idx = contents->indexOf(url);
            if (idx.isValid()) {
                contents->setCurrentIndex(idx);
            }
        });

        auto *indexPage = new QWidget(this);
        auto *indexLayout = new QVBoxLayout(indexPage);
        indexLayout->setContentsMargins(4, 4, 4, 4);
        auto *indexFilter = new QLineEdit(indexPage);
        indexFilter->setPlaceholderText(QStringLiteral("Look for keyword"));
        indexFilter->setClearButtonEnabled(true);
        QHelpIndexWidget *index = m_engine->indexWidget();
        indexLayout->addWidget(indexFilter);
        indexLayout->addWidget(index, 1);
        m_sideTabs->addTab(indexPage, QStringLiteral("Index"));
        connect(indexFilter, &QLineEdit::textChanged, index, [index](const QString &text) {
            index->filterIndices(text);
        });
        connect(indexFilter, &QLineEdit::returnPressed, index, &QHelpIndexWidget::activateCurrentItem);
        connect(index, &QHelpIndexWidget::documentActivated, this,
                [this](const QHelpLink &link, const QString &) { openUrl(link.url); });
        connect(index, &QHelpIndexWidget::documentsActivated, this,
                [this](const QList<QHelpLink> &links, const QString &) {
                    QList<QUrl> urls;
                    QStringList titles;
                    for (const QHelpLink &l : links) {
                        urls << l.url;
                        titles << l.title;
                    }
                    chooseTopic(urls, titles);
                });

        QHelpSearchEngine *search = m_engine->searchEngine();
        auto *searchPage = new QWidget(this);
        auto *searchLayout = new QVBoxLayout(searchPage);
        searchLayout->setContentsMargins(4, 4, 4, 4);
        searchLayout->addWidget(search->queryWidget());
        searchLayout->addWidget(search->resultWidget(), 1);
        m_sideTabs->addTab(searchPage, QStringLiteral("Search"));
        connect(search->queryWidget(), &QHelpSearchQueryWidget::search, search, [search]() {
            search->search(search->queryWidget()->searchInput());
        });
        connect(search->resultWidget(), &QHelpSearchResultWidget::requestShowLink,
                this, &HelpWindow::openUrl);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_engine->contentModel()->createContentsForCurrentFilter();
        m_engine->indexModel()->createIndexForCurrentFilter();
#else
        m_engine->contentModel()->createContents(QString());
        m_engine->indexModel()->createIndex(QString());
#endif
        // Builds the full-text index when it is missing or out of date
        connect(search, &QHelpSearchEngine::indexingStarted, this, [this]() { m_indexing = true; });
        connect(search, &QHelpSearchEngine::indexingFinished, this, [this]() { m_indexing = false; });
        m_indexing = true;
        search->scheduleIndexDocumentation();
    }
#endif
    if (m_engine == nullptr) {
        m_plainContents = new QTreeWidget(this);
        m_plainContents->setHeaderHidden(true);
        m_sideTabs->addTab(m_plainContents, QStringLiteral("Contents"));
        connect(m_plainContents, &QTreeWidget::itemActivated, this,
                [this](QTreeWidgetItem *item) { openUrl(pageUrl(item->data(0, Qt::UserRole).toString())); });
        connect(m_plainContents, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem *item) { openUrl(pageUrl(item->data(0, Qt::UserRole).toString())); });

        auto *indexPage = new QWidget(this);
        auto *indexLayout = new QVBoxLayout(indexPage);
        indexLayout->setContentsMargins(4, 4, 4, 4);
        m_plainIndexFilter = new QLineEdit(indexPage);
        m_plainIndexFilter->setPlaceholderText(QStringLiteral("Look for keyword"));
        m_plainIndexFilter->setClearButtonEnabled(true);
        m_plainIndex = new QListWidget(indexPage);
        indexLayout->addWidget(m_plainIndexFilter);
        indexLayout->addWidget(m_plainIndex, 1);
        m_sideTabs->addTab(indexPage, QStringLiteral("Index"));
        connect(m_plainIndexFilter, &QLineEdit::textChanged, this, [this](const QString &text) {
            QListWidgetItem *first = nullptr;
            for (int i = 0; i < m_plainIndex->count(); ++i) {
                QListWidgetItem *it = m_plainIndex->item(i);
                const bool show = it->text().contains(text, Qt::CaseInsensitive);
                it->setHidden(!show);
                if (show && first == nullptr) {
                    first = it;
                }
            }
            m_plainIndex->setCurrentItem(first);
        });
        auto openKeyword = [this](QListWidgetItem *item) {
            if (item == nullptr) {
                return;
            }
            const QStringList refs = m_plainKeywords.values(item->text());
            QList<QUrl> urls;
            for (const QString &ref : refs) {
                urls << pageUrl(ref);
            }
            chooseTopic(urls, refs);
        };
        connect(m_plainIndex, &QListWidget::itemActivated, this, openKeyword);
        connect(m_plainIndex, &QListWidget::itemClicked, this, openKeyword);
        connect(m_plainIndexFilter, &QLineEdit::returnPressed, this,
                [this, openKeyword]() { openKeyword(m_plainIndex->currentItem()); });
    }

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_sideTabs);
    splitter->addWidget(pageArea);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 740});
    setCentralWidget(splitter);

    connect(m_browser, &QTextBrowser::sourceChanged, this, [this]() {
        const QString title = m_browser->documentTitle();
        setWindowTitle(title.isEmpty() ? QStringLiteral("LinCUPL Help")
                                       : title + QStringLiteral(" - LinCUPL Help"));
    });
}

QUrl HelpWindow::pageUrl(const QString &page) const {
    QString path = page;
    QString fragment;
    const int hash = int(path.indexOf(QLatin1Char('#')));
    if (hash >= 0) {
        fragment = path.mid(hash + 1);
        path.truncate(hash);
    }
    QUrl url;
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr) {
        url.setScheme(QStringLiteral("qthelp"));
        url.setHost(m_namespace);
        url.setPath(QStringLiteral("/doc/") + path);
    }
#endif
    if (m_engine == nullptr) {
        url = QUrl::fromLocalFile(QDir(m_dir).filePath(path));
    }
    if (!fragment.isEmpty()) {
        url.setFragment(fragment);
    }
    return url;
}

void HelpWindow::openUrl(const QUrl &url) {
    if (!m_available) {
        show();
        raise();
        activateWindow();
        return;
    }
    m_browser->setSource(url);
    show();
    raise();
    activateWindow();
}

QUrl HelpWindow::currentUrl() const {
    return m_browser->source();
}

void HelpWindow::onLinkClicked(const QUrl &url) {
    if (url.scheme() == QLatin1String("lincupl")) {
        const QString name = url.path();
        const auto it = g_actions.constFind(name);
        if (it != g_actions.constEnd() && it.value()) {
            it.value()();
        } else {
            QMessageBox::information(this, QStringLiteral("LinCUPL Help"),
                                     QStringLiteral("This is available in LinCUPL."));
        }
        return;
    }
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")
        || url.scheme() == QLatin1String("mailto")) {
        QDesktopServices::openUrl(url);
        return;
    }
    openUrl(url);
}

void HelpWindow::chooseTopic(const QList<QUrl> &urls, const QStringList &titles) {
    if (urls.isEmpty()) {
        return;
    }
    if (urls.size() == 1) {
        openUrl(urls.first());
        return;
    }
    QMenu menu(this);
    for (int i = 0; i < urls.size(); ++i) {
        QString text = i < titles.size() && !titles.at(i).isEmpty() ? titles.at(i) : urls.at(i).toString();
        const QUrl url = urls.at(i);
        menu.addAction(text, this, [this, url]() { openUrl(url); });
    }
    menu.exec(QCursor::pos());
}

QList<QUrl> HelpWindow::topicsForKeyword(const QString &keyword) const {
    QList<QUrl> urls;
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr) {
        for (const QHelpLink &link : m_engine->documentsForKeyword(keyword)) {
            urls << link.url;
        }
        return urls;
    }
#endif
    for (const QString &ref : m_plainKeywords.values(keyword)) {
        urls << pageUrl(ref);
    }
    return urls;
}

QUrl HelpWindow::topicFor(const QString &keyword, Help::Context context) const {
    if (keyword.isEmpty() || !m_available) {
        return QUrl();
    }
    const QList<QUrl> urls = topicsForKeyword(keyword);
    if (urls.isEmpty()) {
        return QUrl();
    }
    const bool wantSim = context == Help::Context::Simulator;
    for (const QUrl &url : urls) {
        if (isSimulatorPage(url) == wantSim) {
            return url;
        }
    }
    return urls.first();
}

int HelpWindow::contentsEntryCount() {
    return int(contentsUrls().size());
}

QList<QUrl> HelpWindow::contentsUrls() {
    QList<QUrl> urls;
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr) {
        QHelpContentModel *model = m_engine->contentModel();
        if (model->isCreatingContents()) {
            QEventLoop loop;
            connect(model, &QHelpContentModel::contentsCreated, &loop, &QEventLoop::quit);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
            for (int r = 0; r < model->rowCount(parent); ++r) {
                const QModelIndex idx = model->index(r, 0, parent);
                if (QHelpContentItem *item = model->contentItemAt(idx)) {
                    urls << item->url();
                }
                walk(idx);
            }
        };
        walk(QModelIndex());
        return urls;
    }
#endif
    if (m_plainContents != nullptr) {
        for (QTreeWidgetItemIterator it(m_plainContents); *it; ++it) {
            urls << pageUrl((*it)->data(0, Qt::UserRole).toString());
        }
    }
    return urls;
}

int HelpWindow::indexEntryCount() {
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr) {
        QHelpIndexModel *model = m_engine->indexModel();
        if (model->isCreatingIndex()) {
            QEventLoop loop;
            connect(model, &QHelpIndexModel::indexCreated, &loop, &QEventLoop::quit);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return model->rowCount();
    }
#endif
    return int(m_plainKeywords.uniqueKeys().size());
}

int HelpWindow::searchBlocking(const QString &query, int timeoutMs) {
#ifdef LINCUPL_HAS_QTHELP
    if (m_engine != nullptr) {
        QHelpSearchEngine *search = m_engine->searchEngine();
        QElapsedTimer timer;
        timer.start();
        // Wait for the indexing scheduled by setupQtHelp.  Starting another
        // one here would cancel it, and its indexingFinished would end the
        // wait while the index is still incomplete.
        while (m_indexing && timer.elapsed() < timeoutMs) {
            QEventLoop loop;
            connect(search, &QHelpSearchEngine::indexingFinished, &loop, &QEventLoop::quit);
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
        }
        if (m_indexing) {
            return -1;
        }
        int hits = -1;
        QEventLoop loop;
        connect(search, &QHelpSearchEngine::searchingFinished, &loop, [&](int count) {
            hits = count;
            loop.quit();
        });
        QTimer::singleShot(qMax(1000, timeoutMs - int(timer.elapsed())), &loop, &QEventLoop::quit);
        search->search(query);
        loop.exec();
        return hits;
    }
#endif
    Q_UNUSED(query);
    Q_UNUSED(timeoutMs);
    return -1;
}

void HelpWindow::showContentsTab() {
    m_sideTabs->setCurrentIndex(0);
    if (!isVisible()) {
        openUrl(homeUrl());
    }
}

void HelpWindow::showIndexTab() {
    m_sideTabs->setCurrentIndex(1);
    if (!isVisible()) {
        openUrl(homeUrl());
    }
}

void HelpWindow::showSearchTab() {
    if (m_sideTabs->count() > 2) {
        m_sideTabs->setCurrentIndex(2);
        if (!isVisible()) {
            openUrl(homeUrl());
        }
        m_sideTabs->currentWidget()->setFocus();
        return;
    }
    // Plain pages: no full-text search; find in the page instead
    if (!isVisible()) {
        openUrl(homeUrl());
    }
    showFindBar();
}

void HelpWindow::showFindBar() {
    m_findBar->show();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void HelpWindow::findInPage(bool backward) {
    const QString text = m_findEdit->text();
    if (text.isEmpty()) {
        return;
    }
    QTextDocument::FindFlags flags;
    if (backward) {
        flags |= QTextDocument::FindBackward;
    }
    if (!m_browser->find(text, flags)) {
        // Wrap around
        QTextCursor c = m_browser->textCursor();
        c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        m_browser->setTextCursor(c);
        const bool found = m_browser->find(text, flags);
        QPalette pal = m_findEdit->palette();
        pal.setColor(QPalette::Base, found ? QApplication::palette().color(QPalette::Base)
                                           : QColor(255, 120, 120));
        m_findEdit->setPalette(pal);
        return;
    }
    m_findEdit->setPalette(QApplication::palette());
}
