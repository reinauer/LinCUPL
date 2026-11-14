#pragma once

#include <QDialog>
#include <QFont>

class QLabel;

// The three fonts of Options > Font...: the editor (every CodeEditor tab,
// the read-only output tabs included), the message panes (diagnostics,
// error explanation and build log) and the sidebar (design and output
// files).  Kept in QSettings under fonts/.
struct FontSettings {
    QFont editor = defaultEditorFont();
    QFont messages = defaultMessagesFont();
    QFont sidebar = defaultSidebarFont();

    static QFont defaultEditorFont();   // system fixed-width font, at least 11 pt
    static QFont defaultMessagesFont(); // system fixed-width font
    static QFont defaultSidebarFont();  // the application font

    void loadFromSettings();
    void saveToSettings() const;

    bool operator==(const FontSettings &o) const {
        return editor == o.editor && messages == o.messages && sidebar == o.sidebar;
    }
};

// Options > Font...: one row per font with a sample and a button opening
// QFontDialog, plus "Reset to Defaults".
class FontSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit FontSettingsDialog(const FontSettings &fonts, QWidget *parent = nullptr);

    FontSettings fonts() const { return m_fonts; }

private:
    void chooseFont(QFont *font, const QString &title);
    void updateSamples();

    FontSettings m_fonts;
    QLabel *m_editorSample = nullptr;
    QLabel *m_messagesSample = nullptr;
    QLabel *m_sidebarSample = nullptr;
};
