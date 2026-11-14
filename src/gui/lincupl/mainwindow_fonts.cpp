// Options > Font...: the editor, message pane and sidebar fonts.

#include "mainwindow.h"

#include "code_editor.h"

#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeWidget>

// Every editor tab gets the editor font as its new base font and keeps its
// zoom steps; editors opened later start with it.
void MainWindow::applyFontSettings(const FontSettings &fonts) {
    m_fonts = fonts;
    CodeEditor::setStandardFont(fonts.editor);
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (CodeEditor *ed = editorAt(i)) {
            ed->setBaseFont(fonts.editor);
        }
    }
    m_buildLogEdit->setFont(fonts.messages);
    m_diagTree->setFont(fonts.messages);
    m_errorExplainBrowser->setFont(fonts.messages);
    m_companionTree->setFont(fonts.sidebar);
}

void MainWindow::actionFont() {
    FontSettingsDialog dlg(m_fonts, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const FontSettings fonts = dlg.fonts();
    fonts.saveToSettings();
    applyFontSettings(fonts);
}
