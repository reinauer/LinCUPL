#include "font_settings.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

QFont FontSettings::defaultEditorFont() {
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (mono.pointSize() <= 0) {
        mono.setPointSize(12);
    } else {
        mono.setPointSize(qMax(11, mono.pointSize()));
    }
    return mono;
}

QFont FontSettings::defaultMessagesFont() {
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

QFont FontSettings::defaultSidebarFont() {
    return QApplication::font();
}

// A font is stored as QFont::toString(); a missing or unreadable entry
// leaves the default.
static void loadFont(const QSettings &s, const QString &key, QFont *font) {
    const QString text = s.value(key).toString();
    QFont f;
    if (!text.isEmpty() && f.fromString(text)) {
        *font = f;
    }
}

void FontSettings::loadFromSettings() {
    QSettings s;
    loadFont(s, QStringLiteral("fonts/editor"), &editor);
    loadFont(s, QStringLiteral("fonts/messages"), &messages);
    loadFont(s, QStringLiteral("fonts/sidebar"), &sidebar);
}

// Fonts equal to their default are removed, so a later change of the
// system font still reaches them.
void FontSettings::saveToSettings() const {
    QSettings s;
    const struct {
        const char *key;
        const QFont &font;
        QFont def;
    } entries[] = {
        {"fonts/editor", editor, defaultEditorFont()},
        {"fonts/messages", messages, defaultMessagesFont()},
        {"fonts/sidebar", sidebar, defaultSidebarFont()},
    };
    for (const auto &e : entries) {
        const QString key = QLatin1String(e.key);
        if (e.font == e.def) {
            s.remove(key);
        } else {
            s.setValue(key, e.font.toString());
        }
    }
}

// "Family, 12 pt, bold italic"
static QString describeFont(const QFont &f) {
    QString text = f.family();
    if (f.pointSizeF() > 0) {
        text += QStringLiteral(", %1 pt").arg(f.pointSizeF());
    } else if (f.pixelSize() > 0) {
        text += QStringLiteral(", %1 px").arg(f.pixelSize());
    }
    QStringList style;
    if (f.bold()) style << QStringLiteral("bold");
    if (f.italic()) style << QStringLiteral("italic");
    if (!style.isEmpty()) {
        text += QStringLiteral(", ") + style.join(QLatin1Char(' '));
    }
    return text;
}

FontSettingsDialog::FontSettingsDialog(const FontSettings &fonts, QWidget *parent)
    : QDialog(parent), m_fonts(fonts) {
    setWindowTitle(QStringLiteral("Font"));
    auto *root = new QVBoxLayout(this);
    auto *grid = new QGridLayout();
    grid->setColumnStretch(1, 1);

    auto addRow = [&](int row, const QString &label, const QString &hint, QLabel **sample, QFont *font) {
        auto *name = new QLabel(QStringLiteral("<b>%1</b><br/><small>%2</small>").arg(label, hint), this);
        *sample = new QLabel(this);
        (*sample)->setFrameShape(QFrame::StyledPanel);
        (*sample)->setMinimumWidth(260);
        (*sample)->setMinimumHeight(40);
        auto *btn = new QPushButton(QStringLiteral("Change..."), this);
        connect(btn, &QPushButton::clicked, this, [this, font, label]() {
            chooseFont(font, label);
        });
        grid->addWidget(name, row, 0);
        grid->addWidget(*sample, row, 1);
        grid->addWidget(btn, row, 2);
    };
    addRow(0, QStringLiteral("Editor"), QStringLiteral("source and output tabs"),
           &m_editorSample, &m_fonts.editor);
    addRow(1, QStringLiteral("Messages"), QStringLiteral("diagnostics and build log"),
           &m_messagesSample, &m_fonts.messages);
    addRow(2, QStringLiteral("Sidebar"), QStringLiteral("design and output files"),
           &m_sidebarSample, &m_fonts.sidebar);
    root->addLayout(grid);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("Reset to Defaults"));
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this]() {
        m_fonts = FontSettings{};
        updateSamples();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    updateSamples();
}

void FontSettingsDialog::chooseFont(QFont *font, const QString &title) {
    bool ok = false;
    const QFont chosen = QFontDialog::getFont(&ok, *font, this, QStringLiteral("%1 Font").arg(title));
    if (ok) {
        *font = chosen;
        updateSamples();
    }
}

void FontSettingsDialog::updateSamples() {
    const struct {
        QLabel *label;
        const QFont &font;
    } samples[] = {
        {m_editorSample, m_fonts.editor},
        {m_messagesSample, m_fonts.messages},
        {m_sidebarSample, m_fonts.sidebar},
    };
    for (const auto &s : samples) {
        s.label->setFont(s.font);
        s.label->setText(describeFont(s.font));
    }
}
